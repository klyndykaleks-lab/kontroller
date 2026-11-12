"""Host regression checks against the actual storage helpers and MDB FSM."""
from pathlib import Path
import subprocess, tempfile
root = Path(__file__).resolve().parents[1]
src = root / 'blackpill-f411-controller/Core/Src'
text = (src / 'ftasks.c').read_text()
helpers = text[text.index('volatile bool g_mdb_session_active'):text.index('static void SendF411State(void);')]
test = r'''
#include <assert.h>
#include <stdio.h>
#include "mdb.h"
static uint32_t mask;
static uint32_t __get_PRIMASK(void) { return mask; }
static void __disable_irq(void) { mask = 1; }
static void RestoreInterruptState(uint32_t saved) { mask = saved; }
HELPERS
static MdbCashlessFsm_t ready(void) {
    MdbCashlessFsm_t f; MdbFsmInit(&f);
    f.pollQueueCount=0; f.pollQueueHead=0;
    f.readerEnabled=true; f.setupSeen=true; f.sessionState=MDB_SESSION_IDLE;
    return f;
}
int main(void) {
    MdbCashlessFsm_t f = ready(); MdbPollReply_t reply;
    assert(SdTryBeginOperation());
    g_mdb_session_active=true;
    SdCompleteOperation(); /* in-flight completion MUST NOT unblock payment */
    assert(!SdTryBeginOperation());
    mask=1; assert(!SdTryBeginOperation()); assert(mask==1); mask=0;
    g_mdb_session_active=false;
    assert(SdTryBeginOperation()); assert(!SdTryBeginOperation()); SdCompleteOperation();
    assert(MdbFsmHandleVendRequest(&f,1,300)==MDB_FSM_ACCEPTED);
    g_mdb_session_active=true;
    uint32_t tx=f.currentTransactionId;
    assert(MdbFsmHandleVendRequest(&f,1,300)==MDB_FSM_DUPLICATE);
    assert(MdbFsmApplyPaymentResult(&f,tx+1,1)==MDB_FSM_STALE);
    SdObserveMdbSession(&f); assert(g_mdb_session_active);
    assert(MdbFsmApplyPaymentResult(&f,tx,1)==MDB_FSM_ACCEPTED);
    SdObserveMdbSession(&f); assert(g_mdb_session_active);
    assert(MdbFsmNextPollReply(&f,&reply));
    assert(MdbFsmHandleVendResult(&f,2)==MDB_FSM_ACCEPTED);
    SdObserveMdbSession(&f); assert(g_mdb_session_active);
    assert(MdbFsmHandleSessionComplete(&f)==MDB_FSM_ACCEPTED);
    SdObserveMdbSession(&f); assert(g_mdb_session_active);
    while(MdbFsmNextPollReply(&f,&reply)) {}
    SdObserveMdbSession(&f); assert(!g_mdb_session_active);
    f=ready(); assert(MdbFsmHandleVendRequest(&f,1,300)==MDB_FSM_ACCEPTED);
    g_mdb_session_active=true; MdbFsmSetDeadline(&f,0,60000);
    assert(MdbFsmCheckDeadline(&f,59999)==MDB_FSM_STALE);
    assert(MdbFsmCheckDeadline(&f,60000)==MDB_FSM_ACCEPTED);
    SdObserveMdbSession(&f); assert(g_mdb_session_active); /* denial pending */
    MdbFsmReset(&f); SdObserveMdbSession(&f); assert(!g_mdb_session_active);
    f=ready(); MdbFsmHandleVendRequest(&f,1,300); g_mdb_session_active=true;
    MdbFsmHandleVendCancel(&f); SdObserveMdbSession(&f); assert(g_mdb_session_active);
    MdbFsmQueueCommandOos(&f); SdObserveMdbSession(&f); assert(!g_mdb_session_active);
    puts("PASS: SD admission race, IRQ preservation, busy exclusion, duplicate/stale payment, approval, success, END_SESSION delivery, deadline boundary, reset, cancel, OOS");
}
'''.replace('HELPERS',helpers)
with tempfile.TemporaryDirectory() as tmp:
    c=Path(tmp)/'checks.c'; c.write_text(test)
    exe=Path(tmp)/'checks'
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-I',str(src.parent/'Inc'),str(c),str(src/'mdb.c'),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
