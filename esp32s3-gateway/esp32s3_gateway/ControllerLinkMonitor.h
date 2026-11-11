#pragma once
#include <stdint.h>
#include <stddef.h>

// Main-loop-owned, fixed RAM. No networking, NVS, locks, heap or motor commands.
struct LinkEvent {
  uint32_t id=0, uptimeMs=0, boot=0, tx=0;
  char event[25]={}, detail[113]={};
};
struct F411LinkState {
  bool seen=false;
  uint32_t boot=0, receivedAtMs=0, tx=0, reasonTx=0, recoveries=0;
  uint8_t manager=0,mdb=0;
  bool reportPending=false;
  char reason[25]={};
};
class ControllerLinkMonitor {
 public:
  static constexpr size_t Capacity=8;
  // A concurrent core may publish a timestamp a tick newer than the sample.
  // Signed short-interval age rejects that future sample and handles millis wrap.
  static bool workerRestartDue(uint32_t now,uint32_t /*loops*/,uint32_t lastLoopMs){return (int32_t)(now-lastLoopMs)>=55000;}
  void noteByte(uint32_t now){++rxBytes_;lastByteMs_=now;byteSeen_=true;}
  // True: private telemetry was consumed, including a malformed telemetry frame.
  bool receive(const char* line,uint32_t now);
  void record(const char* event,const char* detail,uint32_t now,uint32_t tx=0,uint32_t boot=0);
  void markUartRestart(uint32_t now);
  bool uartRestartDue(uint32_t now) const;
  const F411LinkState& state()const{return state_;}
  uint32_t rxBytes()const{return rxBytes_;}
  uint32_t rxLines()const{return rxLines_;}
  uint32_t lastByteMs()const{return lastByteMs_;}
  uint32_t malformed()const{return malformed_;}
  uint32_t uartRestarts()const{return uartRestarts_;}
  uint32_t overwritten()const{return overwritten_;}
  size_t count()const{return count_;}
  const LinkEvent& event(size_t oldestIndex)const{return events_[(head_+oldestIndex)%Capacity];}
  const LinkEvent& lastRejection()const{return lastRejection_;}
  const LinkEvent& lastRecovery()const{return lastRecovery_;}
 private:
  F411LinkState state_={};
  LinkEvent events_[Capacity],lastRejection_,lastRecovery_;
  size_t head_=0,count_=0;
  uint32_t nextId_=0,overwritten_=0,rxBytes_=0,rxLines_=0,lastByteMs_=0;
  uint32_t malformed_=0,uartRestarts_=0,lastUartRestartMs_=0;
  bool byteSeen_=false;
};
