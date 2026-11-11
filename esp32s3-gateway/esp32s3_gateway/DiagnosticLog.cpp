#include "DiagnosticLog.h"
#include "HttpsCommandTransport.h"
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <NetworkClientSecure.h>
#include <WiFi.h>
#include <time.h>
extern const uint8_t x509CrtBundleStart[] asm("_binary_x509_crt_bundle_start");
extern const uint8_t x509CrtBundleEnd[] asm("_binary_x509_crt_bundle_end");
namespace {
constexpr unsigned Capacity=16;
struct Record {
  bool used, important, truncated;
  uint8_t event;
  int code;
  uint32_t sequence, uptime;
  char context[241], message[113], attributes[225];
};
Record records[Capacity]={};
portMUX_TYPE mux=portMUX_INITIALIZER_UNLOCKED;
char endpoint[241]="https://device-api-test.kas-pay.by/controllers/01m1f161e1zzxq8985yh9xbchy/logs", logVersion[64]={};
uint32_t logBootCounter=0, sequence=0, dropped=0, sent=0, attempts=0, lastAttempt=0;
int lastHttp=0;
bool busy=true, workerReady=false;

// Producer calls are bounded RAM copies only. No NVS, JSON or networking under lock.
const char* eventName(uint8_t event) {
  static const char* names[]={"boot","terminalRequest","terminalResponse","backendAttempt","backendResponse","flow","network"};
  return event<7?names[event]:"unknown";
}
// Copy a bounded UTF-8 prefix without cutting a multibyte character.
// Producers only copy RAM; JSON parsing/escaping stays in the existing network worker.
bool copyUtf8(char* dest,size_t capacity,const char* source) {
  const size_t length=strlen(source);
  size_t count=length<capacity?length:capacity-1;
  if(count<length)while(count && (static_cast<unsigned char>(source[count])&0xC0)==0x80)--count;
  memcpy(dest,source,count);dest[count]=0;
  return count<length;
}
String encodeAttributes(const Record& record,bool& shortened) {
  JsonDocument parsed;
  String encoded;
  if(!deserializeJson(parsed,record.attributes) && parsed.is<JsonObject>()) {
    serializeJson(parsed,encoded);
    if(encoded.length()<=224)return encoded;
  }
  // UART text, invalid JSON, or an oversized JSON prefix: preserve a bounded
  // preview inside a valid JSON object. Never export broken JSON as attributes.
  char preview[225];copyUtf8(preview,sizeof(preview),record.attributes);
  JsonDocument wrapper;
  for(;;) {
    wrapper["raw"]=preview;encoded="";serializeJson(wrapper,encoded);
    if(encoded.length()<=224)return encoded;
    size_t count=strlen(preview);
    if(count) {--count;while(count && (static_cast<unsigned char>(preview[count])&0xC0)==0x80)--count;}
    preview[count]=0;shortened=true;
  }
}
void serviceIdle() {
  static uint32_t nextDelay=5000, nextAt=millis();
  do {
    char url[241];bool active;Record batch[2]={};unsigned indexes[2]={};unsigned count=0;
    portENTER_CRITICAL(&mux);
    active=!busy&&endpoint[0];memcpy(url,endpoint,sizeof(url));
    if(active && (int32_t)(millis()-nextAt)>=0) {
      // Copy at most two events. Do not remove until successful ACK; producer may evict routine records meanwhile.
      for(unsigned j=0;j<2;j++) {
        int selected=-1;
        for(unsigned i=0;i<Capacity;i++) {
          if(!records[i].used || (j && i==indexes[0]))continue;
          if(selected<0 || (int32_t)(records[i].sequence-records[selected].sequence)<0)selected=i;
        }
        if(selected<0)break;
        indexes[count]=selected;batch[count++]=records[selected];
      }
    }
    portEXIT_CRITICAL(&mux);
    if(!count || WiFi.status()!=WL_CONNECTED || time(nullptr)<1704067200 || ESP.getFreeHeap()<70000)continue;
    // At most one small batch per 5s. Logger never waits for business queue capacity.
    JsonDocument doc;
    doc["type"]="controllerDiagnostics";doc["schemaVersion"]=2;doc["bootCounter"]=logBootCounter;doc["version"]=logVersion;
    doc["unixTime"]=static_cast<uint32_t>(time(nullptr));doc["uptimeMs"]=millis();
    doc["controllerEndpoint"]=HttpsCommandTransport::endpoint();
    portENTER_CRITICAL(&mux);uint32_t lost=dropped;portEXIT_CRITICAL(&mux);doc["dropped"]=lost;
    for(unsigned i=0;i<count;i++) {
      JsonObject e=doc["events"].add<JsonObject>();e["logId"]=batch[i].sequence;e["uptimeMs"]=batch[i].uptime;
      e["eventType"]=eventName(batch[i].event);e["context"]=batch[i].context;e["message"]=batch[i].message;
      bool shortened=batch[i].truncated;
      e["attributes"]=encodeAttributes(batch[i],shortened);
      e["code"]=batch[i].code;e["truncated"]=shortened;
    }
    String body;serializeJson(doc,body);
    portENTER_CRITICAL(&mux);active=!busy&&!strcmp(endpoint,url);portEXIT_CRITICAL(&mux);
    if(!active)continue;
    NetworkClientSecure client;client.setCACertBundle(x509CrtBundleStart,x509CrtBundleEnd-x509CrtBundleStart);
    client.setHandshakeTimeout(3);client.setTimeout(1500);
    HTTPClient http;http.setConnectTimeout(1500);http.setTimeout(1500);
    http.setOperationLimits(3000,1500,1024,500,128);http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
    int code=0;
    if(http.begin(client,url)) {
      http.addHeader("Content-Type","application/json");http.addHeader("Accept","application/json");http.addHeader("X-API-Version","0.1");
      code=http.POST(reinterpret_cast<uint8_t*>(const_cast<char*>(body.c_str())),body.length());
      http.end();
    }
    portENTER_CRITICAL(&mux);
    attempts++;lastHttp=code;lastAttempt=millis();
    if(code>=200 && code<300) {
      sent+=count;
      for(unsigned i=0;i<count;i++)if(records[indexes[i]].used&&records[indexes[i]].sequence==batch[i].sequence){records[indexes[i]].used=false;}
    }
    portEXIT_CRITICAL(&mux);
    nextDelay=(code>=200 && code<300)?5000:min<uint32_t>(nextDelay*2,60000U);nextAt=millis()+nextDelay;
  } while(false);
}
}
void diagnosticBegin(uint32_t boot,const char* version,const char* resetReason) {
  logBootCounter=boot;strlcpy(logVersion,version,sizeof(logVersion));
  workerReady=true;
  diagnosticRecord(DiagnosticEvent::Boot,"esp",resetReason,0,true);
}
void diagnosticRecord(DiagnosticEvent event,const char* context,const char* message,int code,bool important,const char* attributes) {
  if(!context)context="";if(!message)message="";if(!attributes)attributes="{}";
  Record incoming={};incoming.important=important;incoming.event=static_cast<uint8_t>(event);incoming.code=code;incoming.uptime=millis();
  incoming.truncated=copyUtf8(incoming.context,sizeof(incoming.context),context);
  incoming.truncated=copyUtf8(incoming.message,sizeof(incoming.message),message)||incoming.truncated;
  incoming.truncated=copyUtf8(incoming.attributes,sizeof(incoming.attributes),attributes)||incoming.truncated;
  portENTER_CRITICAL(&mux);
  int index=-1;
  for(unsigned i=0;i<Capacity;i++)if(!records[i].used){index=i;break;}
  if(index<0) {
    // Preserve failure context, evict oldest routine entry first; count every loss.
    for(unsigned i=0;i<Capacity;i++)if(!records[i].important && (index<0 || (int32_t)(records[i].sequence-records[index].sequence)<0))index=i;
    if(index<0 && important)for(unsigned i=0;i<Capacity;i++)if(index<0 || (int32_t)(records[i].sequence-records[index].sequence)<0)index=i;
    dropped++;
  }
  if(index>=0){incoming.used=true;incoming.sequence=++sequence;records[index]=incoming;}
  portEXIT_CRITICAL(&mux);
}
void diagnosticBusy(bool value){portENTER_CRITICAL(&mux);busy=value;portEXIT_CRITICAL(&mux);}
bool diagnosticSetEndpoint(const String& value) {
  // Empty submission is explicitly a no-op, including at factory default.
  if(!value.length())return true;
  if(!value.startsWith("https://")||value.length()>240)return false;
  portENTER_CRITICAL(&mux);value.toCharArray(endpoint,sizeof(endpoint));portEXIT_CRITICAL(&mux);return true;
}
String diagnosticEndpoint(){char value[241];portENTER_CRITICAL(&mux);memcpy(value,endpoint,sizeof(value));portEXIT_CRITICAL(&mux);return String(value);}
String diagnosticStats(){
  uint32_t lost,done,tries,at;int code;unsigned pending=0;
  portENTER_CRITICAL(&mux);lost=dropped;done=sent;tries=attempts;at=lastAttempt;code=lastHttp;for(const auto&r:records)pending+=r.used;portEXIT_CRITICAL(&mux);
  JsonDocument doc;doc["initialized"]=workerReady;doc["pending"]=pending;doc["dropped"]=lost;doc["sent"]=done;doc["attempts"]=tries;doc["lastHttpCode"]=code;doc["lastAttemptMs"]=at;
  String json;serializeJson(doc,json);return json;
}

void diagnosticServiceIdle(){serviceIdle();}
