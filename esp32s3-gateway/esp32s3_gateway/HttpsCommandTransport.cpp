#include "HttpsCommandTransport.h"
#include "DiagnosticLog.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <NetworkClientSecure.h>
#include <WiFi.h>
#include <time.h>

#include "device_api_config.h"

extern const uint8_t x509CrtBundleStart[] asm("_binary_x509_crt_bundle_start");
extern const uint8_t x509CrtBundleEnd[] asm("_binary_x509_crt_bundle_end");

namespace {
constexpr size_t kCapacity = 12;
constexpr size_t kPayloadCapacity = 513;
constexpr uint32_t kRetryDelayMs = 2000U;
constexpr uint32_t kMaximumRetryDelayMs = 60000U;
constexpr uint32_t kConnectTimeoutMs = 5000U;
constexpr uint32_t kApplicationIoTimeoutMs = 8000U;
constexpr uint32_t kResponseHeaderTimeoutMs = 5000U;
constexpr size_t kResponseHeaderCapacity = 4096;
constexpr uint32_t kResponseBodyTimeoutMs = 2000U;
constexpr size_t kResponseBodyCapacity = 256;
constexpr time_t kMinimumTlsTime = 1704067200;

struct Slot {
  bool used;
  bool cancelled;
  uint32_t generation;
  uint32_t sequence;
  uint32_t createdMs;
  uint32_t lifetimeMs;
  uint32_t nextAttemptMs;
  uint8_t retryCount;
  HttpsCommandKind kind;
  HttpsCommandState state;
  char payload[kPayloadCapacity];
};

Slot slots[kCapacity] = {};
// Payment-only generation tombstones survive clearSlot and non-payment slot reuse.
// The sole active payment owner releases its context before another payment starts.
uint32_t lastPostedPaymentGeneration[kCapacity] = {};
portMUX_TYPE slotsMux = portMUX_INITIALIZER_UNLOCKED;
HttpsWorkerHealth workerHealth;
// 0 idle; 1 waiting for Wi-Fi/time; 2 business HTTP/TLS; 3 diagnostic HTTP/TLS.
void noteWorkerPhase(uint8_t phase,bool newLoop=false) {
  const uint32_t now=millis();
  portENTER_CRITICAL(&slotsMux);
  if(newLoop){++workerHealth.loops;workerHealth.lastLoopMs=now;}
  if(workerHealth.phase!=phase){workerHealth.phase=phase;workerHealth.phaseSinceMs=now;}
  portEXIT_CRITICAL(&slotsMux);
}
TaskHandle_t workerTask = nullptr;
bool workerAvailable = false;
bool wifiAvailable = false;
bool rebootSuspended = false;
uint32_t nextSequence = 1;
#if DEVICE_API_USE_TEST_HOST
char configuredEndpoint[241] = "https://device-api-test.kas-pay.by/controllers/" DEVICE_API_BOARD_ID "/commands";
#else
char configuredEndpoint[241] = "https://device-api.kas-pay.by/controllers/" DEVICE_API_BOARD_ID "/commands";
#endif

bool validBoardId() {
  const char* value = DEVICE_API_BOARD_ID;
  if (value == nullptr || *value == '\0') return false;
  for (; *value; ++value) {
    if (!((*value >= 'a' && *value <= 'z') || (*value >= '0' && *value <= '9'))) return false;
  }
  return true;
}

bool parseType(const char* payload, String& type) {
  JsonDocument doc;
  if (deserializeJson(doc, payload)) return false;
  if (!doc["type"].is<const char*>()) return false;
  type = doc["type"].as<String>();
  return true;
}

String endpointFor(const char* payload) {
  String type;
  if (!parseType(payload, type)) return String();
  if (type == "paymentRequest" || type == "orderCancellationRequest" ||
      type == "productDeliveryResponse" || type == "rebootConfirmationRequest") {
    return HttpsCommandTransport::endpoint();
  }
  return String();
}

bool handleMatches(const HttpsCommandHandle& handle) {
  return handle.valid() && handle.slot < kCapacity && slots[handle.slot].used &&
         slots[handle.slot].generation == handle.generation;
}

bool expired(const Slot& slot, uint32_t nowMs) {
  return slot.kind != HttpsCommandKind::DurableFinal && slot.lifetimeMs != 0 &&
         static_cast<uint32_t>(nowMs - slot.createdMs) >= slot.lifetimeMs;
}

void clearSlot(size_t index) {
  const uint32_t generation = slots[index].generation;
  slots[index] = {};
  slots[index].generation = generation;
}

uint32_t retryDelayMs(uint8_t retryCount) {
  const uint8_t shift = retryCount > 5 ? 5 : retryCount;
  const uint32_t delayMs = kRetryDelayMs << shift;
  return delayMs < kMaximumRetryDelayMs ? delayMs : kMaximumRetryDelayMs;
}

HttpsCommandHandle allocateSlot(HttpsCommandKind kind, uint32_t lifetimeMs) {
  HttpsCommandHandle handle;
  if(rebootSuspended)return handle;
  const uint32_t nowMs = millis();
  for (size_t i = 0; i < kCapacity; ++i) {
    if (slots[i].used) continue;
    Slot& slot = slots[i];
    uint32_t generation = slot.generation + 1U;
    if (generation == 0) generation = 1U;
    slot = {};
    slot.used = true;
    slot.generation = generation;
    slot.sequence = nextSequence++;
    slot.createdMs = nowMs;
    slot.lifetimeMs = lifetimeMs;
    slot.nextAttemptMs = nowMs;
    slot.kind = kind;
    slot.state = HttpsCommandState::Reserved;
    handle.slot = static_cast<uint8_t>(i);
    handle.generation = generation;
    break;
  }
  return handle;
}

int nextQueued(uint32_t nowMs) {
  int selected = -1;
  uint32_t selectedSequence = 0;
  for (size_t i = 0; i < kCapacity; ++i) {
    Slot& slot = slots[i];
    if (!slot.used) continue;
    if ((slot.state == HttpsCommandState::Reserved ||
         slot.state == HttpsCommandState::Queued) && expired(slot, nowMs)) {
      clearSlot(i);
      continue;
    }
    if (slot.state != HttpsCommandState::Queued ||
        static_cast<int32_t>(nowMs - slot.nextAttemptMs) < 0) {
      continue;
    }
    if (selected < 0 ||
        static_cast<int32_t>(slot.sequence - selectedSequence) < 0) {
      selected = static_cast<int>(i);
      selectedSequence = slot.sequence;
    }
  }
  return selected;
}

bool ackReceived(int code, const String& body) {
  JsonDocument doc;
  if (deserializeJson(doc, body)) return false;
  if (code == 201) {
    return doc["id"].is<const char*>() && doc["id"].as<String>().length() > 0;
  }
  return code == HTTP_CODE_OK && doc["status"].is<const char*>() &&
         doc["status"].as<String>() == "received";
}

HttpsCommandState classifyResponse(int code, bool bodyRead, bool acknowledged) {
  if (acknowledged) return HttpsCommandState::Confirmed;
  if (code < 0 || code >= 500 || !bodyRead) {
    // Once a request has been written, a transport/read failure cannot prove
    // that the backend did not accept it.
    return HttpsCommandState::Ambiguous;
  }
  return HttpsCommandState::Failed;
}
}  // namespace

bool HttpsCommandTransport::begin() {
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  if (workerTask != nullptr) return workerAvailable;
  const BaseType_t created =
      xTaskCreatePinnedToCore(workerEntry, "https-command", 9216, this, 1,
                              &workerTask, 0);
  portENTER_CRITICAL(&slotsMux);
  workerAvailable = created == pdPASS;
  if (!workerAvailable) workerTask = nullptr;
  portEXIT_CRITICAL(&slotsMux);
  return workerAvailable;
}

void HttpsCommandTransport::notifyWifiReady(bool ready) {
  portENTER_CRITICAL(&slotsMux);
  wifiAvailable = ready;
  portEXIT_CRITICAL(&slotsMux);
}

HttpsWorkerHealth HttpsCommandTransport::health() {
  portENTER_CRITICAL(&slotsMux);
  HttpsWorkerHealth value=workerHealth;
  for(const auto& slot:slots) {
    if(!slot.used)continue;
    ++value.used;
    if(slot.state==HttpsCommandState::Queued)++value.queued;
    if(slot.state==HttpsCommandState::InFlight)++value.inFlight;
  }
  portEXIT_CRITICAL(&slotsMux);
  return value;
}

String HttpsCommandTransport::endpoint() {
  char value[241]; portENTER_CRITICAL(&slotsMux);
  memcpy(value, configuredEndpoint, sizeof(value)); portEXIT_CRITICAL(&slotsMux);
  return String(value);
}
bool HttpsCommandTransport::idle() {
  bool idle=true; portENTER_CRITICAL(&slotsMux);
  for (const auto& s:slots) if(s.used) idle=false;
  portEXIT_CRITICAL(&slotsMux); return idle;
}
bool HttpsCommandTransport::setEndpoint(const String& url) {
  bool idle=true; portENTER_CRITICAL(&slotsMux);
  for(const auto& s:slots) if(s.used) idle=false;
  if(idle && url.length()<sizeof(configuredEndpoint)) url.toCharArray(configuredEndpoint,sizeof(configuredEndpoint));
  else idle=false;
  portEXIT_CRITICAL(&slotsMux); return idle;
}
bool HttpsCommandTransport::configured() const { return validBoardId(); }

bool HttpsCommandTransport::available() const {
  portENTER_CRITICAL(&slotsMux);
  const bool ready = workerAvailable;
  portEXIT_CRITICAL(&slotsMux);
  return configured() && ready;
}

bool HttpsCommandTransport::tlsClockReady() const {
  return time(nullptr) >= kMinimumTlsTime;
}

HttpsCommandHandle HttpsCommandTransport::reserve(HttpsCommandKind kind,
                                                  uint32_t lifetimeMs) {
  if (!available()) return {};
  portENTER_CRITICAL(&slotsMux);
  HttpsCommandHandle handle = allocateSlot(kind, lifetimeMs);
  portEXIT_CRITICAL(&slotsMux);
  return handle;
}

bool HttpsCommandTransport::commit(const HttpsCommandHandle& handle,
                                   const String& payload) {
  if (payload.length() == 0 || payload.length() >= kPayloadCapacity) return false;
  String type;
  if (!parseType(payload.c_str(), type) || endpointFor(payload.c_str()).length() == 0) {
    return false;
  }
  portENTER_CRITICAL(&slotsMux);
  if (!handleMatches(handle) ||
      slots[handle.slot].state != HttpsCommandState::Reserved) {
    portEXIT_CRITICAL(&slotsMux);
    return false;
  }
  Slot& slot = slots[handle.slot];
  memcpy(slot.payload, payload.c_str(), payload.length() + 1);
  slot.state = HttpsCommandState::Queued;
  slot.nextAttemptMs = millis();
  portEXIT_CRITICAL(&slotsMux);
  return true;
}

HttpsCommandHandle HttpsCommandTransport::enqueue(const String& payload,
                                                  HttpsCommandKind kind,
                                                  uint32_t lifetimeMs) {
  HttpsCommandHandle handle = reserve(kind, lifetimeMs);
  if (!handle.valid()) return {};
  if (!commit(handle, payload)) {
    release(handle);
    return {};
  }
  return handle;
}

HttpsCommandState HttpsCommandTransport::stateFor(
    const HttpsCommandHandle& handle) const {
  portENTER_CRITICAL(&slotsMux);
  const HttpsCommandState state =
      handleMatches(handle) ? slots[handle.slot].state : HttpsCommandState::Unknown;
  portEXIT_CRITICAL(&slotsMux);
  return state;
}

void HttpsCommandTransport::suspendForReboot() {
  portENTER_CRITICAL(&slotsMux);
  rebootSuspended=true;
  for(size_t i=0;i<kCapacity;++i) {
    if(slots[i].state==HttpsCommandState::InFlight)slots[i].cancelled=true;
    else clearSlot(i);
  }
  portEXIT_CRITICAL(&slotsMux);
}

void HttpsCommandTransport::cancel(const HttpsCommandHandle& handle) {
  portENTER_CRITICAL(&slotsMux);
  if (handleMatches(handle)) {
    if (slots[handle.slot].state == HttpsCommandState::InFlight) {
      slots[handle.slot].cancelled = true;
    } else {
      clearSlot(handle.slot);
    }
  }
  portEXIT_CRITICAL(&slotsMux);
}

bool HttpsCommandTransport::cancelPaymentAndWasAttempted(const HttpsCommandHandle& handle) {
  if (!handle.valid() || handle.slot >= kCapacity) return false;
  portENTER_CRITICAL(&slotsMux);
  const bool attempted = lastPostedPaymentGeneration[handle.slot] == handle.generation;
  if (handleMatches(handle) && slots[handle.slot].kind == HttpsCommandKind::Payment) {
    if (slots[handle.slot].state == HttpsCommandState::InFlight) slots[handle.slot].cancelled = true;
    else clearSlot(handle.slot);
  }
  portEXIT_CRITICAL(&slotsMux);
  return attempted;
}

void HttpsCommandTransport::release(const HttpsCommandHandle& handle) {
  cancel(handle);
}

void HttpsCommandTransport::workerEntry(void* context) {
  static_cast<HttpsCommandTransport*>(context)->worker();
}

void HttpsCommandTransport::worker() {
  for (;;) {
    noteWorkerPhase(0,true);
    bool ready = false;
    portENTER_CRITICAL(&slotsMux);
    ready = wifiAvailable && workerAvailable && !rebootSuspended;
    portEXIT_CRITICAL(&slotsMux);
    if (!ready || !configured() || (endpoint().startsWith("https://") && !tlsClockReady())) {
      noteWorkerPhase(1);
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }

    char payload[kPayloadCapacity] = {};
    HttpsCommandHandle handle;
    HttpsCommandKind kind = HttpsCommandKind::Transient;
    portENTER_CRITICAL(&slotsMux);
    const int index = nextQueued(millis());
    if (index >= 0) {
      Slot& slot = slots[index];
      slot.state = HttpsCommandState::InFlight;
      handle.slot = static_cast<uint8_t>(index);
      handle.generation = slot.generation;
      kind = slot.kind;
      memcpy(payload, slot.payload, sizeof(payload));
    }
    portEXIT_CRITICAL(&slotsMux);
    if (!handle.valid()) {
      noteWorkerPhase(3);
      diagnosticServiceIdle();
      noteWorkerPhase(0);
      vTaskDelay(pdMS_TO_TICKS(25));
      continue;
    }

    noteWorkerPhase(2);
    String url = endpointFor(payload);
    diagnosticRecord(DiagnosticEvent::BackendAttempt,url.c_str(),"Sending backend command",0,false,payload);
    int transportCode=0;
    String transportDetail="connectOrBeginFailed";
    HttpsCommandState outcome = HttpsCommandState::Failed;
    if (url.length()) {
      NetworkClient plainClient;
      NetworkClientSecure client;
      client.setCACertBundle(x509CrtBundleStart,
          static_cast<size_t>(x509CrtBundleEnd - x509CrtBundleStart));
      client.setHandshakeTimeout(10);
      client.setTimeout(kConnectTimeoutMs);
      HTTPClient http;
      http.setConnectTimeout(kConnectTimeoutMs);
      http.setTimeout(kResponseHeaderTimeoutMs);
      http.setOperationLimits(kApplicationIoTimeoutMs,
                              kResponseHeaderTimeoutMs,
                              kResponseHeaderCapacity,
                              kResponseBodyTimeoutMs,
                              kind == HttpsCommandKind::DurableFinal ? SIZE_MAX : kResponseBodyCapacity);
      http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
      if (http.begin(url.startsWith("https://") ? static_cast<NetworkClient&>(client) : plainClient, url)) {
        http.addHeader("X-API-Version", "0.1");
        http.addHeader("Accept", "application/json");
        http.addHeader("Content-Type", "application/json");
        // Linearize cancellation against POST. InFlight may only mean preparing
        // a connection: a cancelled/expired request must not reach the backend.
        portENTER_CRITICAL(&slotsMux);
        const bool postAllowed = handleMatches(handle) && !slots[handle.slot].cancelled &&
                                 !expired(slots[handle.slot], millis());
        if (postAllowed && kind == HttpsCommandKind::Payment)
          lastPostedPaymentGeneration[handle.slot] = handle.generation;
        portEXIT_CRITICAL(&slotsMux);
        const int code = postAllowed
            ? http.POST(reinterpret_cast<uint8_t*>(payload), strlen(payload)) : 0;
        transportCode=code;
        String body;
        const bool finalAck = kind == HttpsCommandKind::DurableFinal && code >= 200 && code < 300;
        const bool finalResponse = kind == HttpsCommandKind::DurableFinal;
        const bool bodyRead = !finalResponse && code > 0 && http.getJsonBodyBounded(body);
        outcome = classifyResponse(
            code, bodyRead, finalAck || (bodyRead && ackReceived(code, body)));
        transportDetail=String("ack=")+String(static_cast<unsigned>(outcome))+",bodyRead="+String(bodyRead?1:0)+",body="+body.substring(0,128);
        // A final ACK requires only status+headers; never drain an unbounded response body.
        if(finalResponse){if(url.startsWith("https://"))client.stop();else plainClient.stop();}
        http.end();
      }
    }

    diagnosticRecord(DiagnosticEvent::BackendResponse,url.c_str(),"Backend response",transportCode,outcome!=HttpsCommandState::Confirmed,transportDetail.c_str());
    portENTER_CRITICAL(&slotsMux);
    workerHealth.lastHttpCode=transportCode;workerHealth.lastCompletedMs=millis();
    workerHealth.lastOutcome=static_cast<uint8_t>(outcome);workerHealth.lastKind=static_cast<uint8_t>(kind);
    if (!handleMatches(handle)) {
      portEXIT_CRITICAL(&slotsMux);
      continue;
    }
    Slot& slot = slots[handle.slot];
    const uint32_t nowMs = millis();
    if (slot.cancelled || expired(slot, nowMs)) {
      clearSlot(handle.slot);
    } else if (outcome == HttpsCommandState::Confirmed) {
      if (kind == HttpsCommandKind::DurableFinal || kind == HttpsCommandKind::Cancellation) slot.state = outcome;
      else clearSlot(handle.slot);
    } else if (kind == HttpsCommandKind::Payment) {
      // A POST that may have reached the backend is never repeated automatically.
      clearSlot(handle.slot);
    } else if (outcome == HttpsCommandState::Failed) {
      // Permanent 4xx is terminal for transient messages. Durable finals remain
      // observable so main can retain their NVS record without hammering backend.
      if (kind == HttpsCommandKind::DurableFinal) {slot.state=HttpsCommandState::Queued;slot.nextAttemptMs=nowMs+5000U;}
      else if (kind == HttpsCommandKind::Cancellation) slot.state = HttpsCommandState::Failed;
      else clearSlot(handle.slot);
    } else {
      slot.state = HttpsCommandState::Queued;
      slot.nextAttemptMs = nowMs + (kind == HttpsCommandKind::DurableFinal ? 5000U : retryDelayMs(slot.retryCount));
      if (slot.retryCount < UINT8_MAX) ++slot.retryCount;
    }
    portEXIT_CRITICAL(&slotsMux);
  }
}
