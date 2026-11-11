#pragma once
static bool applyNetworkSettings(const String& body,uint16_t& code,String& response) {
  JsonDocument d;if(deserializeJson(d,body))return false;
  if(d["logsOnly"]==true) {
    String endpoint=d["logsEndpoint"].as<String>();
    if(endpoint.length()) {
      if(!validBackendUrl(endpoint)||!endpoint.startsWith("https://")){code=400;response="{\"error\":\"invalidLogsEndpoint\"}";return false;}
      Preferences prefs;bool ok=prefs.begin("univerNet",false);
      if(ok){ok=prefs.putString("logs",endpoint)==endpoint.length();prefs.end();}
      if(!ok){code=500;response="{\"error\":\"logsConfigSaveFailed\"}";return false;}
      diagnosticSetEndpoint(endpoint);
    }
    publishNetworkSnapshot();code=200;response="{\"status\":\"updated\"}";return true;
  }
  String u=d["backendUrl"].as<String>();
  if(d["changeWifi"]!=true && !validBackendUrl(u)){code=400;response="{\"error\":\"invalidBackendUrl\"}";return false;}
  // Never move pending payments/results to another backend or reconfigure during a vend.
  if(activePayment.active || pendingDirectDelivery.active || (d["changeWifi"]!=true &&
      (pendingOrderCancellationRequest.active || pendingVendReportSend.active || pendingDirectDeliveryResponse.active || !HttpsCommandTransport::idle()))) {
    code=500;response="{\"error\":\"operationPending\"}";return false;
  }
  bool changeWifi=d["changeWifi"]==true;
  const char* error=nullptr;
  if(changeWifi) {
    bool ok=d["encoded"]==true ? univerWifi.updateLocalConfig(d["ssid"].as<String>(),d["password"].as<String>(),error)
                               : univerWifi.updateLocalCredentials(d["ssid"].as<String>(),d["password"].as<String>(),error);
    if(!ok){code=500;response="{\"error\":\"wifiConfigSaveFailed\"}";return false;}
  }
  if(changeWifi){univerWifi.reconnectUsingLocalConfig(millis());publishNetworkSnapshot();code=200;response="{\"status\":\"updated\"}";return true;}
  Preferences p;bool saved=p.begin("univerNet",false);
  if(saved){saved=p.putString("backend",u)==u.length();p.end();}
  if(!saved){code=500;response="{\"error\":\"backendConfigSaveFailed\"}";return false;}
  HttpsCommandTransport::setEndpoint(u);
  publishNetworkSnapshot();code=200;response="{\"status\":\"updated\",\"statusMessage\":null}";return true;
}
static void handleNetworkSnapshot() {
  recordHttpRebootCheckpoint("networkHttpEntered",0);
  std::unique_ptr<char[]> json(new(std::nothrow) char[sizeof(networkSnapshot)]);uint32_t at;
  if(!json){httpServer.send(503,"application/json","{}");return;}
  portENTER_CRITICAL(&networkSnapshotMux);memcpy(json.get(),networkSnapshot,sizeof(networkSnapshot));at=networkSnapshotAtMs;portEXIT_CRITICAL(&networkSnapshotMux);
  // Compute age in the HTTP task, so a stale main-loop snapshot is visible.
  JsonDocument d;
  if(deserializeJson(d,json.get())){httpServer.send(500,"application/json","{}");return;}
  d["snapshotAgeMs"]=(uint32_t)(millis()-at);
  d["httpListening"]=httpServer.isListening();
  String body;serializeJson(d,body);httpServer.send(200,"application/json",body);
  recordHttpRebootCheckpoint("networkHttpSent",body.length());
}
static void handleNetworkForm(bool encoded=false) {
  bool portal;portENTER_CRITICAL(&networkSnapshotMux);portal=networkPortal;portEXIT_CRITICAL(&networkSnapshotMux);
  if(!portal || httpServer.client().localIP()!=WifiConfigService::portalIp()) {httpServer.send(403,"application/json","{}");return;}
  String origin=httpServer.header("Origin");
  if(origin.length()&&origin!="http://192.168.4.1:8000"){httpServer.send(403,"application/json","{}");return;}
  if((httpServer.arg("logsOnly")!="1" && httpServer.arg("changeWifi")!="0" && (!httpServer.hasArg("password")||!httpServer.hasArg("ssid")))){httpServer.send(400,"application/json","{}");return;}
  JsonDocument d;d["logsOnly"]=httpServer.arg("logsOnly")=="1";d["logsEndpoint"]=httpServer.arg("logsEndpoint");d["ssid"]=httpServer.arg("ssid");d["password"]=httpServer.arg("password");d["backendUrl"]=httpServer.arg("backendUrl");
  d["changeWifi"]=httpServer.arg("changeWifi")!="0";d["encoded"]=encoded;
  String body;serializeJson(d,body);uint16_t code=500;char response[HTTP_JSON_RESPONSE_LIMIT+1]="{}";
  if(waitForHttpResponse("network",body,code,response,sizeof(response)))httpServer.send(code,"application/json",response);
}
static void handleNetworkPage() {
  httpServer.send(200,"text/html; charset=utf-8",R"HTML(<!doctype html><meta charset="utf-8"><meta name="viewport" content="width=device-width"><title>Универ v2</title><h1>Универ — настройки сети</h1><p>Текущие настройки (пароли не отображаются):</p><pre id="current"></pre><p>Изменение доступно в сети Univer-Setup, адрес 192.168.4.1:8000. После сохранения Wi-Fi устройство переподключится. Рабочий адрес: 192.168.x.10:8000.</p><form method="post" action="/wifi-config"><label>SSID <input name="ssid" maxlength="32" required></label><br><label>Пароль Wi-Fi <input type="password" name="password" maxlength="64"></label><br><button>Сохранить Wi-Fi</button></form><form method="post" action="/wifi-config"><input type="hidden" name="changeWifi" value="0"><label>Backend URL <input id="url" name="backendUrl" maxlength="240" required size="50"></label><button>Сохранить backend</button></form><form method="post" action="/wifi-config"><input type="hidden" name="logsOnly" value="1"><label>HTTPS endpoint логов <input id="logs" name="logsEndpoint" maxlength="240" size="50" placeholder="Не задан: отправка выключена"></label><p>Пустое поле сохраняет прежний адрес.</p><button>Сохранить endpoint логов</button></form><script>fetch('/network-settings').then(r=>r.json()).then(d=>{document.querySelector('#current').textContent=JSON.stringify(d,null,2);document.querySelector('#url').value=d.backendUrl;document.querySelector('#logs').value=d.logsEndpoint;document.querySelector('[name=ssid]').value=d.configuredSsid;});</script>)HTML");
}
static WebServer networkRedirect(80);
static bool redirectRunning=false;
static void serviceNetworkPortalHttp() {
  bool portal;portENTER_CRITICAL(&networkSnapshotMux);portal=networkPortal;portEXIT_CRITICAL(&networkSnapshotMux);
  if(portal&&!setupDnsRunning){setupDnsRunning=setupDns.start(53,"*",WifiConfigService::portalIp());}
  if(!portal&&setupDnsRunning){setupDns.stop();setupDnsRunning=false;}
  if(portal&&!redirectRunning){
    networkRedirect.setRequestLimits(2000,1000,4096,1024,32,0);
    networkRedirect.onNotFound([](){networkRedirect.sendHeader("Location","http://192.168.4.1:8000/");networkRedirect.send(302,"text/plain","");});
    networkRedirect.begin();redirectRunning=true;
  }
  if(!portal&&redirectRunning){networkRedirect.stop();redirectRunning=false;}
  if(redirectRunning)networkRedirect.handleClient();
  if(setupDnsRunning)setupDns.processNextRequest();
}
