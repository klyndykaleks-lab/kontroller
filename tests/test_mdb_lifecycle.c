#include <assert.h>
#include <stdio.h>
#include "mdb.h"
static void drain(MdbCashlessFsm_t*f){MdbPollReply_t p;unsigned n=0;while(MdbFsmNextPollReply(f,&p))assert(++n<12);}
static MdbCashlessFsm_t ready(void){MdbCashlessFsm_t f;MdbFsmInit(&f);drain(&f);assert(MdbFsmHandleSetupConfig(&f)==MDB_FSM_ACCEPTED);assert(MdbFsmHandleReaderEnable(&f)==MDB_FSM_ACCEPTED);drain(&f);assert(f.sessionState==MDB_SESSION_IDLE);return f;}
static uint32_t request(MdbCashlessFsm_t*f){assert(MdbFsmHandleVendRequest(f,7,370)==MDB_FSM_ACCEPTED);return f->currentTransactionId;}
static void end(MdbCashlessFsm_t*f){assert(MdbFsmHandleSessionComplete(f)==MDB_FSM_ACCEPTED);MdbPollReply_t p;assert(MdbFsmPeekPollReply(f,&p));assert(f->currentTransactionId!=0);drain(f);assert(!f->currentTransactionId);assert(f->sessionState==MDB_SESSION_IDLE);}
int main(void){
 MdbCashlessFsm_t f=ready();uint32_t a=request(&f);
 assert(MdbFsmApplyPaymentResult(&f,a,MDB_PAYMENT_RESULT_APPROVED)==MDB_FSM_ACCEPTED);drain(&f);assert(f.sessionState==MDB_SESSION_APPROVED);assert(MdbFsmHandleVendResult(&f,2)==MDB_FSM_ACCEPTED);end(&f);
 uint32_t b=request(&f);assert(b>a);assert(MdbFsmApplyPaymentResult(&f,b,MDB_PAYMENT_RESULT_DENIED)==MDB_FSM_ACCEPTED);drain(&f);end(&f);
 uint32_t c=request(&f);assert(c>b);assert(MdbFsmApplyPaymentResult(&f,b,MDB_PAYMENT_RESULT_APPROVED)==MDB_FSM_STALE);assert(MdbFsmApplyPaymentResult(&f,c,MDB_PAYMENT_RESULT_APPROVED)==MDB_FSM_ACCEPTED);drain(&f);assert(MdbFsmHandleVendResult(&f,2)==MDB_FSM_ACCEPTED);end(&f);
 puts("PASS T01-T03: available -> unavailable -> available, no reboot, stale previous result");
 uint32_t d=request(&f);assert(MdbFsmHandleVendCancel(&f)==MDB_FSM_ACCEPTED);assert(MdbFsmApplyPaymentResult(&f,d,MDB_PAYMENT_RESULT_APPROVED)==MDB_FSM_DUPLICATE);drain(&f);end(&f);assert(MdbFsmApplyPaymentResult(&f,d,1)==MDB_FSM_STALE);
 puts("PASS T04: cancel before payment, late approval cannot authorize vend");
 d=request(&f);MdbFsmSetDeadline(&f,0xFFFF8000U,60000U);assert(MdbFsmCheckDeadline(&f,0xFFFF8000U+59999U)==MDB_FSM_STALE);assert(MdbFsmCheckDeadline(&f,0xFFFF8000U+60000U)==MDB_FSM_ACCEPTED);assert(MdbFsmApplyPaymentResult(&f,d,1)==MDB_FSM_DUPLICATE);drain(&f);end(&f);
 puts("PASS T05: disconnected payment timeout, tick wrap, late result rejected");
 d=request(&f);assert(MdbFsmApplyPaymentResult(&f,d,1)==MDB_FSM_ACCEPTED);MdbPollReply_t p;assert(MdbFsmPeekPollReply(&f,&p));assert(f.sessionState==MDB_SESSION_APPROVAL_PENDING);assert(MdbFsmPeekPollReply(&f,&p));assert(f.sessionState==MDB_SESSION_APPROVAL_PENDING);assert(MdbFsmAcknowledgePollReply(&f,&p));assert(f.sessionState==MDB_SESSION_APPROVED);assert(!MdbFsmAcknowledgePollReply(&f,&p));assert(MdbFsmHandleVendResult(&f,3)==MDB_FSM_ACCEPTED);end(&f);
 puts("PASS T06-T07: lost ACK keeps response pending; single ACK commit; VEND_FAILURE lifecycle (bank refund is external)");
 d=request(&f);assert(MdbFsmHandleSessionComplete(&f)==MDB_FSM_COMPAT_CANCEL);drain(&f);assert(MdbFsmApplyPaymentResult(&f,d,1)==MDB_FSM_STALE);
 puts("PASS T08: early SESSION_COMPLETE retires operation without approval");
 d=request(&f);MdbFsmReset(&f);assert(MdbFsmApplyPaymentResult(&f,d,1)==MDB_FSM_STALE);assert(f.sessionState==MDB_SESSION_INACTIVE);
 f=ready();request(&f);assert(MdbFsmQueueCommandOos(&f)==MDB_FSM_ABORTED_OUT_OF_SEQUENCE);assert(!f.resetRequired);drain(&f);assert(MdbFsmHandleVendRequest(&f,7,370)==MDB_FSM_ACCEPTED);
 puts("PASS T09-T10: reset rejects old result; configured OOS compatibility permits next vend");
}
