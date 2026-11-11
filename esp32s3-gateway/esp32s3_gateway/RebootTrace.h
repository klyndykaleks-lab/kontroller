#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
// One main-loop writer. Fixed RTC storage, no flash, locks, heap or networking.
struct RebootTraceEntry {
 uint32_t magic,sequence,boot,uptimeMs,stage,token,value;
 char checkpoint[32],detail[48];
 uint32_t checksum;
};
struct RebootTraceStore {RebootTraceEntry entries[12];};
class RebootTrace {
 public:
 void begin(RebootTraceStore& store,bool retained){
  store_=&store;previous_={};next_=1;count_=0;
  if(!retained)memset(&store,0,sizeof(store));
  for(const auto&e:store.entries)if(valid(e)){
   ++count_;if(!previous_.magic||(int32_t)(e.sequence-previous_.sequence)>0)previous_=e;
  }
  if(previous_.magic){next_=previous_.sequence+1U;if(!next_)next_=1;}
 }
 static bool valid(const RebootTraceEntry&e){
  return e.magic==Magic&&e.sequence&&e.checkpoint[31]==0&&e.detail[47]==0&&e.checksum==sum(e);
 }
 void mark(uint32_t boot,uint32_t now,uint32_t stage,uint32_t token,const char* checkpoint,const char*detail="",uint32_t value=0){
  if(!store_)return;
  RebootTraceEntry e={};e.magic=Magic;e.sequence=next_++;if(!next_)next_=1;
  e.boot=boot;e.uptimeMs=now;e.stage=stage;e.token=token;e.value=value;
  copy(e.checkpoint,sizeof(e.checkpoint),checkpoint);copy(e.detail,sizeof(e.detail),detail);e.checksum=sum(e);
  auto& slot=store_->entries[(e.sequence-1U)%12U];
  slot.magic=0;__sync_synchronize();
  memcpy(reinterpret_cast<uint8_t*>(&slot)+4,reinterpret_cast<const uint8_t*>(&e)+4,sizeof(e)-4);
  __sync_synchronize();slot.magic=Magic;
  if(count_<12)++count_;
 }
 const RebootTraceEntry& previous()const{return previous_;}
 const RebootTraceEntry* latest()const{
  const RebootTraceEntry* best=nullptr;if(!store_)return best;
  for(const auto&e:store_->entries)if(valid(e)&&(!best||(int32_t)(e.sequence-best->sequence)>0))best=&e;
  return best;
 }
 const RebootTraceEntry* recent(unsigned age)const{
  const auto*last=latest();if(!last||age>=12)return nullptr;
  uint32_t n=last->sequence-age;if(!n)return nullptr;
  const auto&e=store_->entries[(n-1U)%12U];return valid(e)&&e.sequence==n?&e:nullptr;
 }
 unsigned count()const{return count_;}
 private:
 static constexpr uint32_t Magic=0x52544333U;
 static void copy(char*out,size_t n,const char*s){if(!s)s="";size_t len=strlen(s);if(len>=n)len=n-1;memcpy(out,s,len);out[len]=0;}
 static uint32_t sum(const RebootTraceEntry&e){
  const auto*p=reinterpret_cast<const uint8_t*>(&e);uint32_t h=2166136261U;
  for(size_t i=4;i<offsetof(RebootTraceEntry,checksum);++i)h=(h^p[i])*16777619U;
  return h;
 }
 RebootTraceStore*store_=nullptr;RebootTraceEntry previous_={};uint32_t next_=1;unsigned count_=0;
};
