#pragma once
#include "WifiConfigService.h"
#include <DNSServer.h>
static DeviceConfig univerWifiDefaults() {
  DeviceConfig c={};strncpy(c.wifiSsid,WIFI_SSID,32);strncpy(c.wifiPassword,WIFI_PASSWORD,64);return c;
}
static WifiConfigService univerWifi(univerWifiDefaults());
static portMUX_TYPE networkSnapshotMux=portMUX_INITIALIZER_UNLOCKED;
static char networkSnapshot[8192]="{}";
static bool networkPortal=false;
static uint32_t networkSnapshotAtMs=0;
static DNSServer setupDns;
static bool setupDnsRunning=false;
static bool validBackendUrl(const String& url) {
  if(url.length()>240 || (!url.startsWith("http://")&&!url.startsWith("https://")))return false;
  for(size_t i=0;i<url.length();i++)if(((unsigned char)url[i]<=32 || (unsigned char)url[i]>126) || url[i]=='"'||url[i]=='<'||url[i]=='>'||url[i]=='@'||url[i]=='#'||url[i]=='?'||url[i]=='\\')return false;
  const unsigned begin=url.startsWith("https://")?8:7;
  int slash=url.indexOf('/',begin);
  if(slash<=(int)begin || url[begin]==':' || url[begin]=='.') return false;
  for(unsigned i=begin;i<(unsigned)slash;i++) {
    const char c=url[i];
    if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='.'||c=='-'||c==':'))return false;
  }
  return true;
}
static void appendUniverRebootStatus(JsonDocument& doc);
static void recordHttpRebootCheckpoint(const char* step,uint32_t value);
static void publishNetworkSnapshot() {
  JsonDocument d;d["connected"]=univerWifi.isConnected();d["portalOpen"]=univerWifi.isPortalOpen();
  d["rssiDbm"]=wifiRssiLastDbm;d["lastDisconnectReason"]=lastWifiDisconnectReason;
  d["configuredSsid"]=univerWifi.localSsid();d["connectedSsid"]=WiFi.SSID();
  d["ip"]=WiFi.localIP().toString();d["gateway"]=WiFi.gatewayIP().toString();
  d["dns"]=WiFi.dnsIP().toString();d["mask"]=WiFi.subnetMask().toString();
  d["port"]=8000;d["backendUrl"]=HttpsCommandTransport::endpoint();d["version"]=FIRMWARE_BUILD_ID;
  d["logsEndpoint"]=diagnosticEndpoint();
  JsonDocument stats;deserializeJson(stats,diagnosticStats());d["logs"]=stats.as<JsonObject>();
  JsonObject h=d["health"].to<JsonObject>();
  h["lastPlannedRestart"]=plannedRestartReasonToString(previousPlannedRestartReason);
  h["resetReason"]=resetReasonToString(bootResetReason);
  h["resetReasonCode"]=static_cast<int>(bootResetReason);
  h["loopStackFree"]=uxTaskGetStackHighWaterMark(nullptr);
  appendPreviousBreadcrumb(h["previousBoot"].to<JsonObject>());
  const auto http=getHttpTaskHealth();
  h["httpTaskLoops"]=http.loops;h["httpTaskLastLoopMs"]=http.lastLoopMs;
  h["bootCounter"]=bootCounter;h["uptimeMs"]=millis();h["loopCount"]=loopCount;
  h["phase"]=diagnosticPhaseToString(currentDiagnosticPhase);
  h["lastLoopMs"]=lastLoopCompletedMs;h["lastLoopDurationMs"]=lastLoopDurationMs;
  h["maxLoopGapMs"]=maxLoopGapMs;
  h["freeHeap"]=ESP.getFreeHeap();h["minFreeHeap"]=ESP.getMinFreeHeap();
  h["largestFreeBlock"]=ESP.getMaxAllocHeap();
  h["appState"]=stateToString(appState);h["paymentActive"]=activePayment.active;
  h["paymentTx"]=activePayment.transactionId;h["paymentBoot"]=activePayment.bootIdentity;
  h["vendReportPending"]=pendingVendReportSend.active;
  h["vendJournalReady"]=vendJournalReady;h["paymentJournalReady"]=paymentJournalReady;
  h["httpStackFree"]=httpServerTask?uxTaskGetStackHighWaterMark(httpServerTask):0;
  const HttpsWorkerHealth worker=HttpsCommandTransport::health();
  JsonObject w=h["worker"].to<JsonObject>();
  w["loops"]=worker.loops;w["lastLoopMs"]=worker.lastLoopMs;
  w["phase"]=worker.phase;w["phaseSinceMs"]=worker.phaseSinceMs;
  w["used"]=worker.used;w["queued"]=worker.queued;w["inFlight"]=worker.inFlight;
  w["lastHttpCode"]=worker.lastHttpCode;w["lastCompletedMs"]=worker.lastCompletedMs;
  w["lastOutcome"]=worker.lastOutcome;w["lastKind"]=worker.lastKind;
  w["transportAvailable"]=httpsTransport.available();w["tlsClockReady"]=httpsTransport.tlsClockReady();
  w["wifiReady"]=wifiReady;
  const auto& f411=controllerLink.state();
  JsonObject f=h["f411"].to<JsonObject>();
  f["seen"]=f411.seen;f["boot"]=f411.boot;f["receivedAtMs"]=f411.receivedAtMs;
  f["ageMs"]=f411.seen?(uint32_t)(millis()-f411.receivedAtMs):0;
  f["managerState"]=f411.manager;f["mdbState"]=f411.mdb;f["tx"]=f411.tx;
  f["reportPending"]=f411.reportPending;f["lastReason"]=f411.reason;f["lastReasonTx"]=f411.reasonTx;f["recoveryAttempts"]=f411.recoveries;
  f["uartRxBytes"]=controllerLink.rxBytes();f["uartRxLines"]=controllerLink.rxLines();f["lastRxByteMs"]=controllerLink.lastByteMs();
  f["malformedFrames"]=controllerLink.malformed();f["uartRestarts"]=controllerLink.uartRestarts();
  auto addEvent=[](JsonObject obj,const LinkEvent& event){obj["id"]=event.id;obj["uptimeMs"]=event.uptimeMs;obj["boot"]=event.boot;obj["tx"]=event.tx;obj["eventType"]=event.event;obj["detail"]=event.detail;};
  addEvent(d["lastRejection"].to<JsonObject>(),controllerLink.lastRejection());
  addEvent(d["lastRecovery"].to<JsonObject>(),controllerLink.lastRecovery());
  JsonArray recent=d["recentEvents"].to<JsonArray>();
  for(size_t i=0;i<controllerLink.count();++i)addEvent(recent.add<JsonObject>(),controllerLink.event(i));
  d["recentEventsOverwritten"]=controllerLink.overwritten();
  appendUniverRebootStatus(d);
  std::unique_ptr<char[]> json(new(std::nothrow) char[sizeof(networkSnapshot)]);
  if(!json)return;
  if(measureJson(d)>=sizeof(networkSnapshot)){strcpy(json.get(),"{\"error\":\"networkSnapshotTooLarge\"}");}
  else serializeJson(d,json.get(),sizeof(networkSnapshot));
  portENTER_CRITICAL(&networkSnapshotMux);memcpy(networkSnapshot,json.get(),sizeof(networkSnapshot));networkSnapshotAtMs=millis();networkPortal=univerWifi.isPortalOpen();portEXIT_CRITICAL(&networkSnapshotMux);
}
static void beginUniverNetwork() {
  Preferences p;if(p.begin("univerNet",true)){String u=p.getString("backend","");String logs=p.getString("logs","");p.end();if(validBackendUrl(u))HttpsCommandTransport::setEndpoint(u);if(validBackendUrl(logs)&&logs.startsWith("https://"))diagnosticSetEndpoint(logs);}
  univerWifi.begin(millis());publishNetworkSnapshot();
}
static void serviceUniverNetwork() {
  univerWifi.service(millis());
  const bool wasReady=wifiReady;wifiReady=univerWifi.isConnected();
  if(wasReady!=wifiReady)diagnosticRecord(DiagnosticEvent::Network,wifiReady?"connected":"disconnected",wifiReady?WiFi.localIP().toString().c_str():"offline",0,!wifiReady);
  if(wasReady&&!wifiReady)handleBackendOfflineDuringActiveFlow("Wi-Fi offline во время оплаты");
  if(!wasReady&&wifiReady){configTime(0,0,"pool.ntp.org","time.google.com");}
  static uint32_t published=0;
  if((uint32_t)(millis()-published)>=500){published=millis();publishNetworkSnapshot();}
}
