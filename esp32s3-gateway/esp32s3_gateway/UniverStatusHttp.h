#pragma once
// HTTP task owns this RAM flag. No UART, GPIO, NVS or physical door inference.
static bool univerInitialInteraction = true;
static void handleUniverStatus() {
  recordHttpRebootCheckpoint("statusHttpEntered",0);
  JsonDocument doc;
  doc["latchStatus"] = "unknown";
  doc["doorStatus"] = "unknown";
  doc["temperature"] = "none";
  doc["status"] = nullptr;
  doc["latchMode"] = "openingHighPulse";
  doc["time"] = nullptr;
  doc["initialInteraction"] = univerInitialInteraction;
  doc["version"] = FIRMWARE_BUILD_ID;
  // Main owns hardware/FSM/NVS. Read its bounded published snapshot only.
  std::unique_ptr<char[]> snapshot(new(std::nothrow) char[sizeof(networkSnapshot)]);
  if (!snapshot) { httpServer.send(503, "application/json", "{}"); return; }
  uint32_t snapshotAt;
  portENTER_CRITICAL(&networkSnapshotMux);
  memcpy(snapshot.get(), networkSnapshot, sizeof(networkSnapshot));
  snapshotAt = networkSnapshotAtMs;
  portEXIT_CRITICAL(&networkSnapshotMux);
  JsonDocument published;
  if (deserializeJson(published, snapshot.get())) {
    httpServer.send(500, "application/json", "{}"); return;
  }
  JsonObject network = doc["network"].to<JsonObject>();
  for (const char* key : {"connected", "portalOpen", "configuredSsid", "connectedSsid",
                          "ip", "gateway", "dns", "mask", "port", "backendUrl", "logsEndpoint", "rssiDbm", "lastDisconnectReason"}) {
    network[key].set(published[key]);
  }
  for (const char* key : {"logs", "health", "lastRejection", "lastRecovery",
                          "recentEvents", "recentEventsOverwritten", "reboot"}) {
    doc[key].set(published[key]);
  }
  doc["snapshotAgeMs"] = (uint32_t)(millis() - snapshotAt);
  doc["httpListening"] = httpServer.isListening();
  String body;
  serializeJson(doc, body);
  httpServer.send(200, "application/json", body);
  recordHttpRebootCheckpoint("statusHttpSent",body.length());
  if (httpServer.lastResponseSucceeded()) univerInitialInteraction = false;
}
