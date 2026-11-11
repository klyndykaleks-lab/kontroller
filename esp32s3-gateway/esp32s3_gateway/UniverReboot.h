#pragma once
#include <esp_random.h>
#include "RebootTrace.h"
// Main-loop owner. HTTP task reads only the bounded published snapshot below.
struct RebootRecord { uint32_t magic, token, stage, blackBoot, discard, checksum; };
static RebootRecord rebootRecord={};
static uint32_t rebootPhaseSince, rebootLastTx, rebootSaveRetry;
static bool rebootConfirmationAllowed;
static HttpsCommandHandle rebootConfirmationHandle;
static Preferences rebootPrefs;
static bool rebootJournalReady;
static portMUX_TYPE rebootSnapshotMux=portMUX_INITIALIZER_UNLOCKED;
static RebootRecord rebootSnapshot={};
static char rebootError[40]="none", rebootSnapshotError[40]="none";
RTC_NOINIT_ATTR static RebootTraceStore rebootRtcTrace;
static RebootTrace rebootTrace;
RTC_NOINIT_ATTR static RebootTraceStore rebootHttpRtcTrace;
static RebootTrace rebootHttpTrace;
static portMUX_TYPE rebootHttpTraceMux=portMUX_INITIALIZER_UNLOCKED;
static bool rebootHttpReplyPending;
static uint32_t rebootHttpReplySince;
static void setRebootHttpReplyPending(bool pending){
  const uint32_t now=millis();portENTER_CRITICAL(&rebootHttpTraceMux);
  rebootHttpReplyPending=pending;if(pending)rebootHttpReplySince=now;
  portEXIT_CRITICAL(&rebootHttpTraceMux);
}
static bool rebootHttpReplyOutstanding(){
  const uint32_t now=millis();portENTER_CRITICAL(&rebootHttpTraceMux);
  const bool pending=rebootHttpReplyPending;const uint32_t since=rebootHttpReplySince;
  portEXIT_CRITICAL(&rebootHttpTraceMux);
  // A concurrent writer can be one tick newer; treat that as still pending.
  return pending&&(int32_t)(now-since)<4000;
}
static uint32_t rebootDiagnosticBoot, rebootRequests, rebootTxAttempts, rebootTxSent;
static uint32_t rebootRxLines, rebootRxMatched, rebootSaveFailures, rebootEraseFailures;
static uint32_t rebootRestartCalls, rebootConfirmationEnqueues;
static void recordRebootCheckpoint(const char* step,const char* detail="",uint32_t value=0){
  rebootTrace.mark(rebootDiagnosticBoot,millis(),rebootRecord.stage,rebootRecord.token,step,detail,value);
}
static void beginRebootDiagnostics(uint32_t boot,bool retained){
  rebootDiagnosticBoot=boot;rebootTrace.begin(rebootRtcTrace,retained);
  rebootHttpTrace.begin(rebootHttpRtcTrace,retained);
  recordRebootCheckpoint("boot",retained?"rtcRetained":"coldStart");
}
static void recordHttpRebootCheckpoint(const char* step,uint32_t value){
  const uint32_t now=millis();
  portENTER_CRITICAL(&rebootHttpTraceMux);
  rebootHttpTrace.mark(rebootDiagnosticBoot,now,0,0,step,"",value);
  portEXIT_CRITICAL(&rebootHttpTraceMux);
}
static bool sendRebootLine(const String& line){
  ++rebootTxAttempts;recordRebootCheckpoint("uartTxBefore",line.c_str());
  const bool sent=sendLineToStm(line);if(sent)++rebootTxSent;
  recordRebootCheckpoint("uartTxAfter",line.c_str(),sent?1:0);return sent;
}
static void restartForReboot(const char* reason){
  ++rebootRestartCalls;recordRebootCheckpoint("espRestartBefore",reason);ESP.restart();
}

static uint32_t rebootChecksum(const RebootRecord& r) {
  return r.magic ^ r.token ^ r.stage ^ r.blackBoot ^ r.discard ^ 0xB00710A1U;
}
static void publishRebootSnapshot() {
  portENTER_CRITICAL(&rebootSnapshotMux);
  rebootSnapshot=rebootRecord;memcpy(rebootSnapshotError,rebootError,sizeof(rebootError));
  portEXIT_CRITICAL(&rebootSnapshotMux);
}
static bool saveRebootStage(uint32_t stage,uint32_t token,uint32_t blackBoot) {
  RebootRecord next={0x52424632U,token,stage,blackBoot,rebootRecord.discard,0};next.checksum=rebootChecksum(next);
  recordRebootCheckpoint("stageSaveBefore","",stage);
  if(!rebootJournalReady||rebootPrefs.putBytes("record",&next,sizeof(next))!=sizeof(next)){
    ++rebootSaveFailures;recordRebootCheckpoint("stageSaveFailed","",stage);return false;
  }
  rebootRecord=next;rebootPhaseSince=millis();rebootLastTx=millis()-1000U;
  recordRebootCheckpoint("stageSaved","",stage);publishRebootSnapshot();return true;
}
static bool univerRebootActive(){return rebootRecord.stage>=1&&rebootRecord.stage<=3;}
static bool univerRebootBlocksBusiness(){return univerRebootActive()||rebootRecord.discard!=0;}
// Called before any operation-journal restore/worker startup. Preserve all counters/settings.
static bool discardRebootOperationJournals(){
  const char* spaces[]={"payment","vend","direct","final70","final70"};
  const char* keys[]={"flow","result","record","vend","direct"};
  for(unsigned i=0;i<5;++i){
    recordRebootCheckpoint("contextEraseBefore",spaces[i],i);
    Preferences p;if(!p.begin(spaces[i],false)){++rebootEraseFailures;recordRebootCheckpoint("contextOpenFailed",spaces[i],i);return false;}
    const bool ok=!p.isKey(keys[i])||p.remove(keys[i]);p.end();
    if(!ok){++rebootEraseFailures;recordRebootCheckpoint("contextEraseFailed",spaces[i],i);return false;}
    recordRebootCheckpoint("contextErased",spaces[i],i);
  }
  return true;
}
static void beginUniverReboot() {
  recordRebootCheckpoint("journalLoadBefore");
  rebootJournalReady=rebootPrefs.begin("rebootHardPair",false);
  if(rebootJournalReady&&rebootPrefs.getBytesLength("record")==sizeof(RebootRecord)){
    RebootRecord saved={};const size_t read=rebootPrefs.getBytes("record",&saved,sizeof(saved));
    if(read==sizeof(saved)&&saved.magic==0x52424632U&&saved.stage<=5&&saved.discard<=1&&saved.checksum==rebootChecksum(saved))rebootRecord=saved;
    else {rebootRecord.stage=5;strcpy(rebootError,"invalidRebootJournal");}
  }
  if(rebootRecord.stage==5&&strcmp(rebootError,"none")==0)strcpy(rebootError,"previousRebootFailed");
  if(rebootRecord.discard){
    if(discardRebootOperationJournals()) {
      const uint32_t pending=rebootRecord.discard;rebootRecord.discard=0;
      if(!saveRebootStage(rebootRecord.stage,rebootRecord.token,rebootRecord.blackBoot))rebootRecord.discard=pending;
    }
    if(rebootRecord.discard)strcpy(rebootError,"contextEraseFailed");
  }
  rebootConfirmationAllowed=rebootRecord.stage==4&&!rebootRecord.discard;
  rebootPhaseSince=millis();rebootLastTx=millis()-1000U;rebootSaveRetry=millis()-1000U;recordRebootCheckpoint("journalLoaded",rebootError);publishRebootSnapshot();
}
static bool acceptUniverReboot() {
  ++rebootRequests;recordRebootCheckpoint("requestReceived");
  if(rebootRecord.stage>=1&&rebootRecord.stage<=3)return true;
  uint32_t token=rebootRecord.token ? rebootRecord.token+1U : esp_random();
  if(!token)return false; // Never reuse a completed/failed maintenance token.
  strcpy(rebootError,"none");
  const uint32_t oldDiscard=rebootRecord.discard;rebootRecord.discard=1;
  const bool saved=saveRebootStage(1,token,0);
  if(!saved){rebootRecord.discard=oldDiscard;publishRebootSnapshot();}
  if(saved){recordRebootCheckpoint("workerSuspendBefore");httpsTransport.suspendForReboot();recordRebootCheckpoint("workerSuspended");rebootSaveRetry=millis()-1000U;}
  return saved;
}
static void failUniverReboot(const char* reason) {
  recordRebootCheckpoint("rebootFailed",reason);
  strncpy(rebootError,reason,sizeof(rebootError)-1);
  if(!saveRebootStage(5,rebootRecord.token,rebootRecord.blackBoot)){rebootRecord.stage=5;publishRebootSnapshot();}
}
static bool receiveUniverReboot(const String& line) {
  if(line.startsWith("REBOOT_")){++rebootRxLines;recordRebootCheckpoint("uartRx",line.c_str());}
  const String token=String(rebootRecord.token);
  const String prefix="REBOOT_FORCE_CONTROLLER_READY,token="+token+",boot=";
  if(line.startsWith(prefix)&&(rebootRecord.stage==2||rebootRecord.stage==3)){
    String value=line.substring(prefix.length());uint32_t senderBoot=0;
    if(!value.length()||value.length()>10)return true;
    for(size_t i=0;i<value.length();++i){char c=value[i];if(c<'0'||c>'9'||senderBoot>(UINT32_MAX-(c-'0'))/10U)return true;senderBoot=senderBoot*10U+(c-'0');}
    if(!senderBoot)return true;
    ++rebootRxMatched;recordRebootCheckpoint("f411ReadyMatched","",senderBoot);
    if(rebootRecord.stage==2){
      if(saveRebootStage(3,rebootRecord.token,senderBoot))sendRebootLine("REBOOT_FORCE_CONTROLLER_ACK,token="+token);
    }else if(senderBoot==rebootRecord.blackBoot)sendRebootLine("REBOOT_FORCE_CONTROLLER_ACK,token="+token);
    return true;
  }
  if(line.startsWith("REBOOT_FAILED,token="+token+",")&&univerRebootActive()){
    failUniverReboot("stmRebootFailed");restartForReboot("stmRebootFailed");return true;
  }
  return line.startsWith("REBOOT_FORCE_CONTROLLER_READY,")||line.startsWith("REBOOT_FAILED,");
}
static void serviceUniverReboot() {
  uint32_t now=millis();
  const auto& black=controllerLink.state();
  if(rebootRecord.stage==1){
    if(rebootHttpReplyOutstanding())return; // Finish the local HTTP response attempt first.
    // No idle/payment/outbox gate. The accepted intent discards operation state on boot.
    if((uint32_t)(now-rebootSaveRetry)>=1000U){
      rebootSaveRetry=now;
      if(!saveRebootStage(2,rebootRecord.token,black.boot))failUniverReboot("rebootSaveFailed");
      // NVS updates phaseSince using a later tick; never compare it to the pre-write sample.
      now=millis();
    }
  }
  if(rebootRecord.stage==2){
    if((uint32_t)(now-rebootPhaseSince)>=30000U){failUniverReboot("controllerReadyTimeout");restartForReboot("controllerReadyTimeout");return;}
    if((uint32_t)(now-rebootLastTx)>=1000U){rebootLastTx=now;sendRebootLine("REBOOT_FORCE_CONTROLLER_BEGIN,token="+String(rebootRecord.token));}
  }else if(rebootRecord.stage==3){
    if(black.seen&&black.boot&&black.boot>rebootRecord.blackBoot&&
       (uint32_t)(now-black.receivedAtMs)<5000U){
      if(saveRebootStage(4,rebootRecord.token,rebootRecord.blackBoot)){
        // stage4 is eligible for confirmation only after setup runs again.
        rebootConfirmationAllowed=false;restartForReboot("chainComplete");
      }else failUniverReboot("rebootSaveFailed");
      return;
    }
    if((uint32_t)(now-rebootPhaseSince)>=30000U){failUniverReboot("blackConfirmationTimeout");restartForReboot("blackConfirmationTimeout");return;}
    if((uint32_t)(now-rebootLastTx)>=1000U){rebootLastTx=now;sendRebootLine("REBOOT_FORCE_CONTROLLER_ACK,token="+String(rebootRecord.token));}
  }else if(rebootRecord.stage==4&&rebootConfirmationAllowed){
    if(!rebootConfirmationHandle.valid()&&canSendWebSocketNow()) {
      rebootConfirmationHandle=httpsTransport.enqueue("{\"type\":\"rebootConfirmationRequest\",\"status\":\"done\"}",HttpsCommandKind::DurableFinal,0);
      if(rebootConfirmationHandle.valid()){++rebootConfirmationEnqueues;recordRebootCheckpoint("confirmationQueued");}
    }
    if(httpsTransport.stateFor(rebootConfirmationHandle)==HttpsCommandState::Confirmed && (uint32_t)(now-rebootSaveRetry)>=1000U){
      rebootSaveRetry=now;recordRebootCheckpoint("confirmationAck");
      // Failed NVS cleanup must not turn an acknowledged confirmation into another POST.
      if(saveRebootStage(0,rebootRecord.token,0)){httpsTransport.release(rebootConfirmationHandle);rebootConfirmationHandle={};}
    }
  }
}
static void appendRebootTraceEntry(JsonObject obj,const RebootTraceEntry*e){
  obj["valid"]=e!=nullptr;if(!e)return;
  obj["bootCounter"]=e->boot;obj["uptimeMs"]=e->uptimeMs;obj["sequence"]=e->sequence;
  obj["stage"]=e->stage;obj["token"]=e->token;obj["checkpoint"]=e->checkpoint;
  obj["detail"]=e->detail;obj["value"]=e->value;
}
static void appendUniverRebootStatus(JsonDocument& doc){
  RebootRecord r;char error[40];portENTER_CRITICAL(&rebootSnapshotMux);
  r=rebootSnapshot;memcpy(error,rebootSnapshotError,sizeof(error));portEXIT_CRITICAL(&rebootSnapshotMux);
  JsonObject o=doc["reboot"].to<JsonObject>();o["stage"]=r.stage;o["token"]=r.token;o["error"]=error;o["mode"]="hard";o["profile"]="esp-f411";o["contextDiscardPending"]=r.discard!=0;
  o["journalReady"]=rebootJournalReady;o["phaseAgeMs"]=(uint32_t)(millis()-rebootPhaseSince);
  o["f411BootBefore"]=r.blackBoot;o["lastTxMs"]=rebootLastTx;
  o["httpReplyPending"]=rebootHttpReplyOutstanding();
  o["confirmationAllowed"]=rebootConfirmationAllowed;o["confirmationQueued"]=rebootConfirmationHandle.valid();
  o["confirmationStateRaw"]=static_cast<unsigned>(httpsTransport.stateFor(rebootConfirmationHandle));
  JsonObject c=o["counters"].to<JsonObject>();c["requests"]=rebootRequests;
  c["uartTxAttempts"]=rebootTxAttempts;c["uartTxSent"]=rebootTxSent;
  c["uartRxLines"]=rebootRxLines;c["readyMatched"]=rebootRxMatched;
  c["nvsSaveFailures"]=rebootSaveFailures;c["contextEraseFailures"]=rebootEraseFailures;
  c["restartCalls"]=rebootRestartCalls;c["confirmationEnqueues"]=rebootConfirmationEnqueues;
  const auto& previous=rebootTrace.previous();
  appendRebootTraceEntry(o["previousBootLast"].to<JsonObject>(),previous.magic?&previous:nullptr);
  JsonArray trace=o["trace"].to<JsonArray>();
  for(int i=3;i>=0;--i){const auto*e=rebootTrace.recent(i);if(e)appendRebootTraceEntry(trace.add<JsonObject>(),e);}
  o["traceStored"]=rebootTrace.count();
  RebootTraceEntry httpLast={},httpPrevious={};
  portENTER_CRITICAL(&rebootHttpTraceMux);
  const auto* http=rebootHttpTrace.latest();if(http)httpLast=*http;
  httpPrevious=rebootHttpTrace.previous();
  portEXIT_CRITICAL(&rebootHttpTraceMux);
  appendRebootTraceEntry(o["httpLast"].to<JsonObject>(),httpLast.magic?&httpLast:nullptr);
  appendRebootTraceEntry(o["previousBootHttpLast"].to<JsonObject>(),httpPrevious.magic?&httpPrevious:nullptr);
}
bool waitForHttpResponse(const char*,const String&,uint16_t&,char*,size_t);
static void handleUniverReboot(){
  setRebootHttpReplyPending(true);
  recordHttpRebootCheckpoint("rebootHttpEntered",0);
  uint16_t code;char response[HTTP_JSON_RESPONSE_LIMIT+1]={};
  if(waitForHttpResponse("reboot","{}",code,response,sizeof(response))){
    recordHttpRebootCheckpoint("rebootBridgeReturned",code);
    httpServer.send(code,"application/json",response);
    recordHttpRebootCheckpoint("rebootHttpSent",code);
  }else recordHttpRebootCheckpoint("rebootBridgeTimeout",0);
  setRebootHttpReplyPending(false);
}
