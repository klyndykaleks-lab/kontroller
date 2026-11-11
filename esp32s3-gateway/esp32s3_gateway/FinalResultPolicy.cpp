#include "FinalResultPolicy.h"
#include <Arduino.h>
#include <Preferences.h>
#include <time.h>
#include <string.h>
namespace {
constexpr uint32_t Magic=0x46523731U;
struct Entry {FinalResultBudget record={};uint32_t lastMs=0,lastSaveMs=0;};
Entry entries[2];
Preferences store;
bool ready=false;
const char* key(unsigned index){return index?"direct":"vend";}
bool same(const FinalResultBudget&r,uint32_t first,uint32_t second){return r.first==first&&r.second==second;}
bool olderOrSame(const FinalResultBudget&r,uint32_t first,uint32_t second){return first<r.first||(first==r.first&&second<=r.second);}
void save(unsigned i){if(ready)store.putBytes(key(i),&entries[i].record,sizeof(FinalResultBudget));entries[i].lastSaveMs=millis();}
uint64_t epochMs(){const time_t now=time(nullptr);return now>=1704067200?static_cast<uint64_t>(now)*1000ULL:0;}
}
void finalResultPolicyBegin(){
 ready=store.begin("final70",false);
 for(unsigned i=0;i<2;i++){
  entries[i]={};
  const size_t loaded=ready?store.getBytes(key(i),&entries[i].record,sizeof(FinalResultBudget)):0;
  auto&r=entries[i].record;
  if(loaded!=sizeof(FinalResultBudget)||r.magic!=Magic||r.phase<1||r.phase>3||r.remainingMs>FINAL_RESULT_BUDGET_MS)r={};
  entries[i].lastMs=entries[i].lastSaveMs=millis();
 }
}
bool finalResultTerminal(bool direct,uint32_t first,uint32_t second){
 const auto&r=entries[direct?1:0].record;
 return r.magic==Magic&&r.phase>=2&&olderOrSame(r,first,second);
}
uint32_t finalResultRemaining(bool direct,uint32_t first,uint32_t second){
 const unsigned i=direct?1:0;auto&e=entries[i];auto&r=e.record;const uint32_t now=millis();
 if(finalResultTerminal(direct,first,second))return 0;
 if(r.magic!=Magic||!same(r,first,second)){
  r={};r.magic=Magic;r.first=first;r.second=second;r.phase=1;r.remainingMs=FINAL_RESULT_BUDGET_MS;
  e.lastMs=now;
  const uint64_t epoch=epochMs();if(epoch)r.expiresUnixMs=epoch+71000ULL; // second-resolution clock: no early expiry from rounding
  save(i);return r.remainingMs;
 }
 if(r.phase!=1)return 0;
 const uint32_t elapsed=now-e.lastMs;e.lastMs=now;
 r.remainingMs=elapsed>=r.remainingMs?0:r.remainingMs-elapsed;
 const uint64_t epoch=epochMs();
 if(epoch){
  if(!r.expiresUnixMs)r.expiresUnixMs=epoch+r.remainingMs+1000ULL;
  if(epoch>=r.expiresUnixMs)r.remainingMs=0;
  else if(r.expiresUnixMs-epoch<r.remainingMs)r.remainingMs=static_cast<uint32_t>(r.expiresUnixMs-epoch);
 }
 // Checkpoint includes offline waits; restart never blindly grants a new70s.
 if(now-e.lastSaveMs>=FINAL_RESULT_RETRY_MS)save(i);
 return r.remainingMs;
}
void finalResultComplete(bool direct,uint32_t first,uint32_t second,bool confirmed){
 const unsigned i=direct?1:0;auto&r=entries[i].record;
 if(r.magic==Magic&&same(r,first,second)&&r.phase>=2)return;
 // Never let a late old completion tombstone a newer result.
 if(r.magic==Magic&&!same(r,first,second)&&olderOrSame(r,first,second))return;
 if(r.magic!=Magic||!same(r,first,second)){r={};r.magic=Magic;r.first=first;r.second=second;}
 r.remainingMs=0;r.phase=confirmed?2:3;save(i);
}
