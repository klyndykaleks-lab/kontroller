#include <WiFi.h>
#include <WebSocketsServer.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <errno.h>
#include <esp_system.h>
#include <esp_wifi_types.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include "wifi_secrets.h"
#include "HttpsCommandTransport.h"
#include "DiagnosticLog.h"
#include "FinalResultPolicy.h"
#include "ControllerLinkMonitor.h"
#include <memory>
#include <new>
static ControllerLinkMonitor controllerLink;

struct DeliveryLineCheckRequestData;
struct DeliveryLineCheckResultData;
enum class HttpAdmission { Complete, Waiting };

const size_t HTTP_JSON_BODY_LIMIT = 512;
const size_t HTTP_JSON_RESPONSE_LIMIT = 384;
const uint32_t HTTP_REQUEST_RECEIVE_TIMEOUT_MS = 2000;
const uint32_t HTTP_ADMISSION_TIMEOUT_MS = 500;
const uint32_t HTTP_PROCESSING_TIMEOUT_MS = 2500;
const uint32_t HTTP_RESPONSE_WRITE_TIMEOUT_MS = 1000;

enum class HttpBridgeState : uint8_t {
  Free,
  Pending,
  Claimed,
  Complete
};

struct HttpBridgeSlot {
  HttpBridgeState state = HttpBridgeState::Free;
  uint32_t token = 0;
  uint16_t responseCode = 500;
  char route[16] = {};
  char body[HTTP_JSON_BODY_LIMIT + 1] = {};
  char responseBody[HTTP_JSON_RESPONSE_LIMIT + 1] = "{}";
  char timeoutBody[HTTP_JSON_RESPONSE_LIMIT + 1] = "{}";
};

// 1 = local test mode, 0 = normal bridge mode.
#define APP_TEST_MODE 0

// UART transport is used between ESP32-S3 and STM32.
#define STM_USB_HOST_MODE 0

// -------- Wi-Fi --------

// -------- Local WebSocket server --------
IPAddress LOCAL_IP(192, 168, 0, 10);
IPAddress LOCAL_GATEWAY(192, 168, 0, 1);
IPAddress LOCAL_SUBNET(255, 255, 255, 0);
IPAddress LOCAL_DNS(192, 168, 0, 1);
const uint16_t WS_PORT = 8000;
const char* WS_PATH = "/mdb/ws";

// Если backend потребует Sec-WebSocket-Protocol, вписать значение сюда.
const char* WS_SUBPROTOCOL = "";

// Если backend потребует headers, пример. В server-mode ESP принимает клиентские
// заголовки handshake, а JSON API остаётся тем же.
// const char* WS_EXTRA_HEADERS = "Authorization: Bearer xxx\r\nX-Device-Id: esp32-test";
const char* WS_EXTRA_HEADERS = "";

// -------- LED --------
#if defined(D2)
const uint8_t LED_PIN = D2;
#else
const uint8_t LED_PIN = 2;
#endif

const bool LED_ACTIVE_HIGH = true;

// -------- Ports --------
#if !STM_USB_HOST_MODE
HardwareSerial& STM_PORT = Serial1;
const uint32_t STM_UART_BAUD = 115200;
const int STM_UART_RX_PIN = 18; // ESP RX, connect to STM32 PA15 TX
const int STM_UART_TX_PIN = 17; // ESP TX, connect to STM32 PA10 RX
#endif
#if APP_TEST_MODE && !STM_USB_HOST_MODE
Stream& LOG_PORT = Serial;
#else
Stream& LOG_PORT = Serial0;
#endif

#ifndef SERIAL_DEBUG_ENABLED
#define SERIAL_DEBUG_ENABLED 0
#endif
#if SERIAL_DEBUG_ENABLED
const char* FIRMWARE_BUILD_ID = "esp-http-varka-v10.0.5-reboot-fix-esp-f411-20261110-serial-debug";
#else
const char* FIRMWARE_BUILD_ID = "esp-http-varka-v10.0.5-reboot-fix-esp-f411-20261110";
#endif
const uint8_t GATEWAY_DIAGNOSTIC_VERSION = 2;

// -------- Timeouts and intervals --------
const uint32_t WIFI_DRIVER_RESTART_AFTER_MS = 120000;
const uint32_t WIFI_DEVICE_RESTART_AFTER_MS = 300000;
const uint32_t WS_RECONNECT_INTERVAL_MS = 5000;
const uint32_t WS_HEARTBEAT_INTERVAL_MS = 15000;
const uint32_t WS_HEARTBEAT_PONG_TIMEOUT_MS = 3000;
const uint8_t WS_HEARTBEAT_DISCONNECT_COUNT = 2;
const uint32_t DEVICE_STATUS_INTERVAL_MS = 120000;
const uint32_t CONTROLLER_DIAGNOSTIC_INTERVAL_MS = 30000;
const uint32_t RADIO_DIAGNOSTIC_SAMPLE_INTERVAL_MS = 1000;
const uint32_t SLOW_LIBRARY_CALL_THRESHOLD_MS = 250;
const uint32_t SLOW_LIBRARY_EVENT_INTERVAL_MS = 5000;
const uint32_t STATE_TIMEOUT_PAYMENT_MS = 100000;
const uint32_t STATE_TIMEOUT_DELIVERY_MS = 60000;
const uint32_t STATE_TIMEOUT_DELIVERY_START_MS = 1500;
const uint32_t STATE_TIMEOUT_CONFIG_ACK_MS = 10000;
const uint32_t STATE_TIMEOUT_LINE_CHECK_MS = 1500;
const uint32_t STATE_TIMEOUT_WS_SEND_MS = 60000;
const uint32_t OFFLINE_FLOW_LOG_INTERVAL_MS = 3000;
const uint32_t DELIVERY_JOURNAL_MAGIC = 0x564D4344U;
const uint16_t DELIVERY_JOURNAL_VERSION = 1U;
const char* DELIVERY_JOURNAL_NAMESPACE = "direct";
const char* DELIVERY_JOURNAL_KEY = "record";
const char* HEALTH_NAMESPACE = "health";
const char* HEALTH_BOOT_COUNTER_KEY = "boots";
const char* HEALTH_WIFI_RESTARTS_KEY = "wifi_rst";
const char* HEALTH_PLANNED_REASON_KEY = "planned";
const uint32_t PLANNED_RESTART_NONE = 0;
const uint32_t PLANNED_RESTART_WIFI_RECOVERY = 1;
const uint32_t PLANNED_RESTART_WORKER_STALL = 2;
const uint32_t PLANNED_RESTART_HTTP_STALL = 3;
const size_t GATEWAY_EVENT_RING_CAPACITY = 64;
const uint32_t GATEWAY_RTC_BREADCRUMB_MAGIC = 0x564D4352U;
const uint16_t GATEWAY_RTC_BREADCRUMB_VERSION = 2U;

// -------- Test mode --------
const uint32_t TEST_PAYMENT_INTERVAL_MS = 30000;
const uint16_t TEST_PRODUCT_POSITION_ID = 101;
const uint32_t TEST_PRICE_MINOR = 100;
const uint8_t TEST_DELIVERY_LINE = 1;
const bool TEST_SEND_DELIVERY_SUCCESS_AFTER_PAID = true;

// -------- Protocol constants --------
const uint16_t CONTROLLER_ID = 1;
const size_t STM_RX_LINE_LIMIT = 256;
const double PRICE_MINOR_DIVISOR = 100.0;
const uint16_t MIN_VALID_PRODUCT_POSITION_ID = 0;
const uint16_t MAX_VALID_PRODUCT_POSITION_ID = 999;
const uint16_t MAX_VALID_MDB_SELECTION_ID = UINT16_MAX;
const uint16_t MIN_VALID_DELIVERY_LINE = 0;
const uint16_t MAX_VALID_DELIVERY_LINE = 255;
const size_t SELECTION_MAP_CAPACITY = 32;

#if STM_USB_HOST_MODE
const size_t STM_USB_TX_BUFFER_SIZE = 256;
const uint32_t STM_USB_MOUNT_WAIT_MS = 1000;
const uint32_t STM_USB_CONNECT_LOG_INTERVAL_MS = 3000;
#endif

enum class UsbIncomingLineType {
  UNKNOWN,
  PAYMENT_REQUEST,
  VEND_REPORT,
  SET_MAP_RESULT,
  DELIVERY_START_RESULT,
  DELIVERY_RESULT,
  CONTROLLER_DIAGNOSTIC,
  ORDER_CANCELLED,
  DELIVERY_LINE_CHECK_RESULT
};

enum class PaymentReplyType {
  OK,
  FAIL,
  TIMEOUT
};

struct PaymentRequestData {
  uint32_t bootIdentity = 0;
  uint32_t transactionId = 0;
  uint16_t productPositionId;
  uint32_t priceMinor;
  double productPrice;
  uint16_t deliveryLine;
};

struct VendReportData {
  uint32_t bootIdentity = 0;
  uint32_t transactionId = 0;
  uint16_t productPositionId;
  String machineStatusRaw;
  String localStatusRaw;
  String coffeeMachineStatus;
  String snackMachineStatus;
};

struct ConfigCommandData {
  uint16_t productPositionId;
  uint16_t deliveryLine;
};

struct ConfigAckData {
  bool success;
  uint16_t motor;
  uint16_t productPositionId;
  String storageStatus;
};

struct ActivePaymentContext {
  uint32_t bootIdentity = 0;
  uint32_t transactionId = 0;
  bool active;
  uint16_t productPositionId;
  uint32_t priceMinor;
  double productPrice;
  uint16_t deliveryLine;
  bool paymentReplyForwardedToStm;
  bool cancelledByMachine;
};

struct PendingConfigContext {
  bool active;
  uint16_t productPositionId;
  uint16_t deliveryLine;
};

struct DirectDeliveryRequestData {
  uint16_t productPositionId;
  uint16_t deliveryLine;
};

struct PendingDirectDeliveryContext {
  bool active;
  uint16_t productPositionId;
  uint16_t deliveryLine;
  bool startStatusReceived;
  bool startConfirmed;
  bool startTimeoutReported;
  String motorBoardStatus;
  String startReason;
  uint32_t operationId;
  bool recoveredAfterRestart;
  bool recoveryNoticePending;
};

struct PendingDirectHttpResponseContext {
  bool active;
  uint32_t token;
  uint32_t operationId;
  uint16_t deliveryLine;
  bool recoveredAfterRestart;
  uint32_t deadlineMs;
};

struct DirectDeliveryStartResultData {
  uint32_t operationId;
  uint16_t deliveryLine;
  bool started;
  String motorBoardStatus;
  String reason;
};

struct DirectDeliveryResultData {
  uint16_t deliveryLine;
  String statusRaw;
  String snackMachineStatus;
  uint32_t operationId;
  bool recoveredAfterRestart;
};

enum class DeliveryJournalPhase : uint8_t {
  ACTIVE = 1,
  RESULT = 2
};

struct DeliveryJournalRecord {
  uint32_t magic;
  uint16_t version;
  uint16_t size;
  uint32_t operationId;
  uint16_t productPositionId;
  uint16_t deliveryLine;
  uint8_t phase;
  uint8_t resultSuccess;
  uint16_t reserved;
  uint32_t checksum;
};

struct DeliveryLineCheckRequestData {
  uint16_t deliveryLine;
};

struct DeliveryLineCheckResultData {
  uint16_t deliveryLine;
  String status;
};

struct ControllerDiagnosticData {
  String source;
  String event;
  uint16_t deliveryLine;
  uint32_t count;
  String status;
};

enum class GatewayEventType : uint8_t {
  BOOT = 1,
  WIFI_STA_CONNECTED = 2,
  WIFI_GOT_IP = 3,
  WIFI_DISCONNECTED = 4,
  WIFI_RECONNECT_ATTEMPT = 5,
  WIFI_DRIVER_RESTART = 6,
  WIFI_DEVICE_RESTART = 7,
  WS_SERVER_STARTED = 8,
  WS_SERVER_STOPPED = 9,
  WS_CLIENT_CONNECTED = 10,
  WS_CLIENT_DISCONNECTED = 11,
  WS_SEND_FAILED = 12,
  SLOW_LIBRARY_CALL = 13
};

enum class DiagnosticPhase : uint8_t {
  INVALID = 0,
  SETUP = 1,
  LOOP_START = 2,
  STM_SERVICE_1 = 3,
  WIFI_RECOVERY = 4,
  STM_SERVICE_2 = 5,
  WS_SERVER_SETUP = 6,
  WS_LOOP = 7,
  TELEMETRY = 8,
  PENDING_WORK_1 = 9,
  STM_SERVICE_3 = 10,
  STM_RX = 11,
  TEST_MODE = 12,
  PENDING_WORK_2 = 13,
  STATE_TIMEOUTS = 14,
  LOOP_IDLE = 15,
  WS_SEND = 16,
  WS_CLIENT_DISCONNECT = 17,
  WS_SERVER_CLOSE = 18,
  WS_SERVER_BEGIN = 19,
  WIFI_BEGIN = 20,
  WIFI_MODE_OFF = 21,
  WIFI_MODE_STA = 22,
  WIFI_CONFIG = 23,
  DEVICE_RESTART = 24
};

struct CallTimingStats {
  uint32_t calls;
  uint32_t lastMs;
  uint32_t maxMs;
  uint32_t slowCalls;
  uint32_t lastSlowEventMs;
};

struct GatewayRtcBreadcrumb {
  uint32_t magic;
  uint16_t version;
  uint16_t size;
  uint32_t bootId;
  uint32_t bootCounter;
  uint32_t uptimeMs;
  uint32_t phaseEnteredMs;
  uint32_t lastLoopStartedMs;
  uint32_t lastLoopCompletedMs;
  uint32_t loopCount;
  uint32_t heapFree;
  int16_t wifiRssi;
  uint8_t wifiStatus;
  uint8_t phase;
  uint8_t appState;
  uint8_t wsActiveClientId;
  uint8_t flags;
  uint8_t reserved[3];
};

struct GatewayEventRecord {
  uint32_t sequence;
  uint32_t uptimeMs;
  GatewayEventType type;
  int32_t value;
  uint32_t loopAgeMs;
  int16_t wifiRssi;
  uint8_t wifiStatus;
  uint8_t diagnosticPhase;
};

struct PendingLineCheckContext {
  bool active;
  uint16_t deliveryLine;
  uint32_t httpResponseToken;
};

struct PendingVendReportSendContext {
  bool active;
  VendReportData data;
  uint32_t deadlineMs;
};

struct PendingDirectDeliveryResponseContext {
  bool active;
  DirectDeliveryResultData data;
  uint32_t deadlineMs;
};

struct PendingOrderCancellationContext {
  bool active;
  uint32_t deadlineMs;
};

void setPendingLineCheck(const DeliveryLineCheckRequestData& request,
                         uint32_t httpResponseToken);
bool parseDeliveryLineCheckResultLine(const String& line,
                                      DeliveryLineCheckResultData& outData);
String buildUsbDeliveryLineCheckCommandLine(
    const DeliveryLineCheckRequestData& request);

struct SelectionMotorMapEntry {
  bool used;
  uint16_t selectionId;
  uint8_t motor;
};

enum class AppState {
  IDLE,
  WAITING_WS_PAYMENT_REPLY,
  WAITING_STM_DELIVERY_RESULT,
  WAITING_STM_CONFIG_ACK,
  WAITING_STM_DIRECT_DELIVERY_RESULT,
  WAITING_STM_LINE_CHECK_RESULT
};

WebSocketsServer webSocket(WS_PORT);
WebServer httpServer(8000);
HttpsCommandTransport httpsTransport;
bool httpServerStarted = false;
TaskHandle_t httpServerTask = nullptr;
struct HttpTaskHealth { uint32_t loops=0,lastLoopMs=0; };
static HttpTaskHealth httpTaskHealth;
static portMUX_TYPE httpTaskHealthMux=portMUX_INITIALIZER_UNLOCKED;
static HttpTaskHealth getHttpTaskHealth() {
  portENTER_CRITICAL(&httpTaskHealthMux);
  const auto value=httpTaskHealth;
  portEXIT_CRITICAL(&httpTaskHealthMux);
  return value;
}
static void noteHttpTaskProgress() {
  const uint32_t now=millis();
  portENTER_CRITICAL(&httpTaskHealthMux);
  ++httpTaskHealth.loops;httpTaskHealth.lastLoopMs=now;
  portEXIT_CRITICAL(&httpTaskHealthMux);
}
portMUX_TYPE httpAdmissionMux = portMUX_INITIALIZER_UNLOCKED;
HttpBridgeSlot httpBridge;
uint32_t nextHttpToken = 1;
uint32_t operationGeneration = 1;
uint32_t pendingCancellationGeneration = 0;
AppState appState = AppState::IDLE;

bool wifiReady = false;
bool wsReady = false;
bool wsServerStarted = false;
uint8_t wsActiveClientId = 255;
uint32_t bootId = 0;
uint32_t bootCounter = 0;
esp_reset_reason_t bootResetReason = ESP_RST_UNKNOWN;
uint32_t previousPlannedRestartReason = PLANNED_RESTART_NONE;
uint32_t wifiRecoveryRestartCount = 0;
uint32_t wifiOfflineSinceMs = 0;
uint32_t wifiDisconnectCount = 0;
uint32_t wifiGotIpCount = 0;
uint32_t wifiDriverRestartCount = 0;
uint32_t wsServerRestartCount = 0;
uint32_t wsConnectCount = 0;
uint32_t wsDisconnectCount = 0;
uint32_t wsSendFailureCount = 0;
uint32_t lastWifiDisconnectReason = 0;
uint32_t lastWsRxMs = 0;
uint32_t lastWsTxMs = 0;
uint32_t lastDeviceStatusMs = 0;
uint32_t lastLoopStartedMs = 0;
uint32_t lastLoopCompletedMs = 0;
uint32_t lastLoopDurationMs = 0;
uint32_t maxLoopDurationMs = 0;
uint32_t maxLoopGapMs = 0;
uint32_t loopCount = 0;
uint32_t lastRadioDiagnosticSampleMs = 0;
uint32_t wifiRssiSampleCount = 0;
uint32_t wifiRssiBelow80SampleCount = 0;
uint32_t wifiRssiBelow90SampleCount = 0;
int16_t wifiRssiLastDbm = -127;
int16_t wifiRssiMinDbm = 0;
int16_t wifiRssiMaxDbm = 0;
uint8_t lastSampledWifiStatus = static_cast<uint8_t>(WL_NO_SHIELD);
uint32_t lastWifiDisconnectUptimeMs = 0;
uint32_t lastWifiDisconnectLoopAgeMs = 0;
int16_t lastWifiDisconnectRssiDbm = -127;
uint8_t lastWifiDisconnectStatus = static_cast<uint8_t>(WL_NO_SHIELD);
DiagnosticPhase lastWifiDisconnectPhase = DiagnosticPhase::INVALID;
DiagnosticPhase currentDiagnosticPhase = DiagnosticPhase::INVALID;
bool previousGatewayBreadcrumbValid = false;
GatewayRtcBreadcrumb previousGatewayBreadcrumb = {};
RTC_NOINIT_ATTR GatewayRtcBreadcrumb gatewayRtcBreadcrumb;
CallTimingStats wifiBeginTiming = {};
CallTimingStats wifiModeOffTiming = {};
CallTimingStats wifiModeStaTiming = {};
CallTimingStats wifiConfigTiming = {};
CallTimingStats wsServerBeginTiming = {};
CallTimingStats wsServerCloseTiming = {};
CallTimingStats wsLoopTiming = {};
CallTimingStats wsSendTiming = {};
CallTimingStats wsClientDisconnectTiming = {};
String lastF411Event;
String lastF411Status;
uint32_t lastF411Count = 0;
uint16_t lastF411State = 0;
String lastF103Event;
String lastF103Status;
uint32_t lastF103CommandCounter = 0;
uint16_t lastF103Line = 0;
bool wifiDriverRestartedThisOutage = false;
bool gatewayHelloPending = false;
bool gatewayStatusPending = false;
bool controllerDiagnosticPending = false;
bool pendingControllerDiagnosticUrgent = false;
ControllerDiagnosticData pendingControllerDiagnostic;
#define VEND_FLOW_DIAGNOSTIC_QUEUE_CAPACITY 16U
ControllerDiagnosticData vendFlowDiagnosticQueue[VEND_FLOW_DIAGNOSTIC_QUEUE_CAPACITY];
uint8_t vendFlowDiagnosticHead = 0;
uint8_t vendFlowDiagnosticTail = 0;
uint8_t vendFlowDiagnosticCount = 0;
uint32_t vendFlowDiagnosticDropCount = 0;
bool gatewayEventReplayPending = false;
uint32_t gatewayEventReplaySequence = 0;

portMUX_TYPE gatewayEventMux = portMUX_INITIALIZER_UNLOCKED;
GatewayEventRecord gatewayEventRing[GATEWAY_EVENT_RING_CAPACITY] = {};
uint32_t gatewayEventNextSequence = 1;
uint32_t gatewayEventStoredCount = 0;

String stmRxBuffer;
SelectionMotorMapEntry selectionMotorMap[SELECTION_MAP_CAPACITY] = {};

uint32_t lastControllerDiagnosticMs = 0;
uint32_t stateEnterMs = 0;

void queueLocalVendFlowDiagnostic(const String& event, uint16_t productPositionId,
                                  uint16_t deliveryLine, uint32_t transactionId,
                                  const String& reason);
uint32_t lastTestPaymentAttemptMs = 0;
uint32_t lastOfflineFlowLogMs = 0;

ActivePaymentContext activePayment = {};
bool activePaymentRequestSentToWs = false;
PendingConfigContext pendingConfig = {false, 0, 0};
PendingDirectDeliveryContext pendingDirectDelivery;
PendingDirectHttpResponseContext pendingDirectHttpResponse;
uint32_t nextDirectRecoveryMs = 0;
bool directResultNeedsPersistence = false;
DirectDeliveryResultData directResultToPersist;
PendingLineCheckContext pendingLineCheck = {false, 0, 0};
PendingVendReportSendContext pendingVendReportSend;
PendingDirectDeliveryResponseContext pendingDirectDeliveryResponse;
PendingOrderCancellationContext pendingOrderCancellationRequest;
HttpsCommandHandle activePaymentRequestHandle;
HttpsCommandHandle pendingVendTransportHandle;
HttpsCommandHandle pendingDirectResponseTransportHandle;
HttpsCommandHandle pendingCancellationTransportHandle;
uint32_t directResponseRetryMs = 0;
Preferences paymentPreferences;
bool paymentJournalReady = false;
String pendingPaymentDecision;
String paymentJournalRecord;
uint32_t paymentDecisionRetryMs = 0;
Preferences vendPreferences;
bool vendJournalReady = false;
String durableVendRecord;
bool vendReportTransportSent = false;
uint32_t vendJournalRetryMs = 0;
uint32_t vendReportSendRetryMs = 0;
Preferences deliveryPreferences;
bool deliveryJournalReady = false;
Preferences healthPreferences;
bool healthPreferencesReady = false;

#if STM_USB_HOST_MODE
USBHostSerial STM_USB(0x0483, 0x5740);
bool stmUsbHostStarted = false;
bool stmUsbLastConnected = false;
uint32_t lastUsbConnectLogMs = 0;
#endif

#if SERIAL_DEBUG_ENABLED
void logMessage(const String& text) {
  LOG_PORT.println(text);
}
#else
#define logMessage(...) do { } while (0)
#endif

#if STM_USB_HOST_MODE
void usbHostSerialLog(const char* message) {
#if SERIAL_DEBUG_ENABLED
  if (message == nullptr) {
    return;
  }
  LOG_PORT.print("[USB HOST] ");
  LOG_PORT.println(message);
#else
  (void)message;
#endif
}

void initStmUsbHost() {
#if !APP_TEST_MODE
  if (stmUsbHostStarted) {
    return;
  }

  STM_USB.setLogger(usbHostSerialLog);
  if (!STM_USB.begin(115200, 0, 0, 8)) {
    logMessage("[USB HOST] USBHostSerial begin failed.");
    return;
  }
  stmUsbHostStarted = true;
  lastUsbConnectLogMs = millis();
  logMessage("[USB HOST] USBHostSerial started, waiting STM32 CDC 0483:5740...");
#endif
}

bool isStmUsbMounted() {
#if !APP_TEST_MODE
  return stmUsbHostStarted && static_cast<bool>(STM_USB);
#else
  return false;
#endif
}

bool waitForStmUsbMounted(uint32_t timeoutMs) {
  uint32_t startMs = millis();
  do {
    if (isStmUsbMounted()) {
      return true;
    }
    delay(10);
  } while ((millis() - startMs) < timeoutMs);

  return isStmUsbMounted();
}

void serviceStmTransport() {
#if !APP_TEST_MODE
  bool connected = isStmUsbMounted();
  uint32_t nowMs = millis();

  if (connected != stmUsbLastConnected) {
    stmUsbLastConnected = connected;
    lastUsbConnectLogMs = nowMs;
    logMessage(connected ? "[USB HOST] STM32 CDC connected." :
               "[USB HOST] STM32 CDC disconnected.");
    return;
  }

  if (!connected && (nowMs - lastUsbConnectLogMs) >= STM_USB_CONNECT_LOG_INTERVAL_MS) {
    lastUsbConnectLogMs = nowMs;
    logMessage("[USB HOST] waiting STM32 CDC...");
  }
#endif
}
#else
void initStmUsbHost() {}
void pumpStmUsbHost() {}
void serviceStmTransport() {}
#endif

void setLed(bool on) {
  digitalWrite(LED_PIN, LED_ACTIVE_HIGH ? (on ? HIGH : LOW) : (on ? LOW : HIGH));
}

String payloadToString(uint8_t* payload, size_t length) {
  String text;
  text.reserve(length);
  for (size_t i = 0; i < length; ++i) {
    text += static_cast<char>(payload[i]);
  }
  return text;
}

const char* stateToString(AppState state) {
  switch (state) {
    case AppState::IDLE:                        return "IDLE";
    case AppState::WAITING_WS_PAYMENT_REPLY:    return "WAITING_WS_PAYMENT_REPLY";
    case AppState::WAITING_STM_DELIVERY_RESULT: return "WAITING_STM_DELIVERY_RESULT";
    case AppState::WAITING_STM_CONFIG_ACK:      return "WAITING_STM_CONFIG_ACK";
    case AppState::WAITING_STM_DIRECT_DELIVERY_RESULT: return "WAITING_STM_DIRECT_DELIVERY_RESULT";
    case AppState::WAITING_STM_LINE_CHECK_RESULT: return "WAITING_STM_LINE_CHECK_RESULT";
    default:                                    return "UNKNOWN";
  }
}

const char* wifiStatusToString(wl_status_t status) {
  switch (status) {
    case WL_NO_SHIELD:       return "WL_NO_SHIELD";
    case WL_STOPPED:         return "WL_STOPPED";
    case WL_IDLE_STATUS:     return "WL_IDLE_STATUS";
    case WL_NO_SSID_AVAIL:   return "WL_NO_SSID_AVAIL";
    case WL_SCAN_COMPLETED:  return "WL_SCAN_COMPLETED";
    case WL_CONNECTED:       return "WL_CONNECTED";
    case WL_CONNECT_FAILED:  return "WL_CONNECT_FAILED";
    case WL_CONNECTION_LOST: return "WL_CONNECTION_LOST";
    case WL_DISCONNECTED:    return "WL_DISCONNECTED";
    default:                 return "UNKNOWN";
  }
}

const char* diagnosticPhaseToString(DiagnosticPhase phase) {
  switch (phase) {
    case DiagnosticPhase::SETUP:                return "setup";
    case DiagnosticPhase::LOOP_START:           return "loopStart";
    case DiagnosticPhase::STM_SERVICE_1:        return "stmService1";
    case DiagnosticPhase::WIFI_RECOVERY:        return "wifiRecovery";
    case DiagnosticPhase::STM_SERVICE_2:        return "stmService2";
    case DiagnosticPhase::WS_SERVER_SETUP:      return "wsServerSetup";
    case DiagnosticPhase::WS_LOOP:              return "wsLoop";
    case DiagnosticPhase::TELEMETRY:            return "telemetry";
    case DiagnosticPhase::PENDING_WORK_1:       return "pendingWork1";
    case DiagnosticPhase::STM_SERVICE_3:        return "stmService3";
    case DiagnosticPhase::STM_RX:               return "stmRx";
    case DiagnosticPhase::TEST_MODE:            return "testMode";
    case DiagnosticPhase::PENDING_WORK_2:       return "pendingWork2";
    case DiagnosticPhase::STATE_TIMEOUTS:       return "stateTimeouts";
    case DiagnosticPhase::LOOP_IDLE:            return "loopIdle";
    case DiagnosticPhase::WS_SEND:              return "wsSend";
    case DiagnosticPhase::WS_CLIENT_DISCONNECT: return "wsClientDisconnect";
    case DiagnosticPhase::WS_SERVER_CLOSE:      return "wsServerClose";
    case DiagnosticPhase::WS_SERVER_BEGIN:      return "wsServerBegin";
    case DiagnosticPhase::WIFI_BEGIN:           return "wifiBegin";
    case DiagnosticPhase::WIFI_MODE_OFF:        return "wifiModeOff";
    case DiagnosticPhase::WIFI_MODE_STA:        return "wifiModeSta";
    case DiagnosticPhase::WIFI_CONFIG:          return "wifiConfig";
    case DiagnosticPhase::DEVICE_RESTART:       return "deviceRestart";
    case DiagnosticPhase::INVALID:
    default:                                     return "invalid";
  }
}

const char* wifiDisconnectReasonToString(uint32_t reason) {
  switch (reason) {
    case 1:   return "unspecified";
    case 2:   return "authExpire";
    case 3:   return "authLeave";
    case 4:   return "assocExpire";
    case 5:   return "assocTooMany";
    case 6:   return "notAuthenticated";
    case 7:   return "notAssociated";
    case 8:   return "assocLeave";
    case 9:   return "assocNotAuthenticated";
    case 15:  return "fourWayHandshakeTimeout";
    case 23:  return "8021xAuthFailed";
    case 34:  return "missingAcks";
    case 36:  return "stationLeaving";
    case 39:  return "timeout";
    case 200: return "beaconTimeout";
    case 201: return "noApFound";
    case 202: return "authFail";
    case 203: return "assocFail";
    case 204: return "handshakeTimeout";
    case 205: return "connectionFail";
    case 206: return "apTsfReset";
    case 207: return "roaming";
    case 208: return "assocComebackTooLong";
    case 209: return "saQueryTimeout";
    case 210: return "noApCompatibleSecurity";
    case 211: return "noApAuthmodeThreshold";
    case 212: return "noApRssiThreshold";
    default:  return "unknown";
  }
}

bool resetCanRetainRtcBreadcrumb(esp_reset_reason_t reason) {
  return reason != ESP_RST_POWERON && reason != ESP_RST_UNKNOWN;
}

bool isRtcBreadcrumbStructValid(const GatewayRtcBreadcrumb& breadcrumb) {
  return breadcrumb.magic == GATEWAY_RTC_BREADCRUMB_MAGIC &&
         breadcrumb.version == GATEWAY_RTC_BREADCRUMB_VERSION &&
         breadcrumb.size == sizeof(GatewayRtcBreadcrumb) &&
         breadcrumb.phase > static_cast<uint8_t>(DiagnosticPhase::INVALID) &&
         breadcrumb.phase <= static_cast<uint8_t>(DiagnosticPhase::DEVICE_RESTART);
}

void markDiagnosticPhase(DiagnosticPhase phase) {
  const uint32_t nowMs = millis();
  currentDiagnosticPhase = phase;
  gatewayRtcBreadcrumb.uptimeMs = nowMs;
  gatewayRtcBreadcrumb.phaseEnteredMs = nowMs;
  gatewayRtcBreadcrumb.lastLoopStartedMs = lastLoopStartedMs;
  gatewayRtcBreadcrumb.lastLoopCompletedMs = lastLoopCompletedMs;
  gatewayRtcBreadcrumb.loopCount = loopCount;
  gatewayRtcBreadcrumb.wifiRssi = wifiRssiLastDbm;
  gatewayRtcBreadcrumb.wifiStatus = lastSampledWifiStatus;
  gatewayRtcBreadcrumb.phase = static_cast<uint8_t>(phase);
  gatewayRtcBreadcrumb.appState = static_cast<uint8_t>(appState);
  gatewayRtcBreadcrumb.wsActiveClientId = wsActiveClientId;
  gatewayRtcBreadcrumb.flags =
      (wifiReady ? 0x01U : 0U) |
      (wsReady ? 0x02U : 0U) |
      (wsServerStarted ? 0x04U : 0U);
}

void initializeRtcBreadcrumb() {
  previousGatewayBreadcrumb = gatewayRtcBreadcrumb;
  previousGatewayBreadcrumbValid =
      resetCanRetainRtcBreadcrumb(bootResetReason) &&
      isRtcBreadcrumbStructValid(previousGatewayBreadcrumb);

  memset(&gatewayRtcBreadcrumb, 0, sizeof(gatewayRtcBreadcrumb));
  gatewayRtcBreadcrumb.magic = GATEWAY_RTC_BREADCRUMB_MAGIC;
  gatewayRtcBreadcrumb.version = GATEWAY_RTC_BREADCRUMB_VERSION;
  gatewayRtcBreadcrumb.size = sizeof(GatewayRtcBreadcrumb);
  gatewayRtcBreadcrumb.bootId = bootId;
  gatewayRtcBreadcrumb.bootCounter = bootCounter;
  gatewayRtcBreadcrumb.heapFree = ESP.getFreeHeap();
  markDiagnosticPhase(DiagnosticPhase::SETUP);
}

void sampleRadioDiagnostics(bool force = false) {
  const uint32_t nowMs = millis();
  if (!force &&
      (nowMs - lastRadioDiagnosticSampleMs) <
          RADIO_DIAGNOSTIC_SAMPLE_INTERVAL_MS) {
    return;
  }
  lastRadioDiagnosticSampleMs = nowMs;

  const wl_status_t status = WiFi.status();
  lastSampledWifiStatus = static_cast<uint8_t>(status);
  if (status == WL_CONNECTED) {
    const int32_t rssi = WiFi.RSSI();
    if (rssi <= 0 && rssi >= -127) {
      wifiRssiLastDbm = static_cast<int16_t>(rssi);
      if (wifiRssiSampleCount == 0) {
        wifiRssiMinDbm = wifiRssiLastDbm;
        wifiRssiMaxDbm = wifiRssiLastDbm;
      } else {
        if (wifiRssiLastDbm < wifiRssiMinDbm) {
          wifiRssiMinDbm = wifiRssiLastDbm;
        }
        if (wifiRssiLastDbm > wifiRssiMaxDbm) {
          wifiRssiMaxDbm = wifiRssiLastDbm;
        }
      }
      ++wifiRssiSampleCount;
      if (wifiRssiLastDbm <= -80) {
        ++wifiRssiBelow80SampleCount;
      }
      if (wifiRssiLastDbm <= -90) {
        ++wifiRssiBelow90SampleCount;
      }
    }
  }

  gatewayRtcBreadcrumb.heapFree = ESP.getFreeHeap();
  gatewayRtcBreadcrumb.wifiRssi = wifiRssiLastDbm;
  gatewayRtcBreadcrumb.wifiStatus = lastSampledWifiStatus;
}

const char* resetReasonToString(esp_reset_reason_t reason) {
  switch (reason) {
    case ESP_RST_POWERON:    return "powerOn";
    case ESP_RST_EXT:        return "externalPin";
    case ESP_RST_SW:         return "software";
    case ESP_RST_PANIC:      return "panic";
    case ESP_RST_INT_WDT:    return "interruptWatchdog";
    case ESP_RST_TASK_WDT:   return "taskWatchdog";
    case ESP_RST_WDT:        return "watchdog";
    case ESP_RST_DEEPSLEEP:  return "deepSleep";
    case ESP_RST_BROWNOUT:   return "brownout";
    case ESP_RST_SDIO:       return "sdio";
    case ESP_RST_USB:        return "usb";
    case ESP_RST_JTAG:       return "jtag";
    case ESP_RST_EFUSE:      return "efuse";
    case ESP_RST_PWR_GLITCH: return "powerGlitch";
    case ESP_RST_CPU_LOCKUP: return "cpuLockup";
    case ESP_RST_UNKNOWN:
    default:                 return "unknown";
  }
}

const char* plannedRestartReasonToString(uint32_t reason) {
  switch (reason) {
    case PLANNED_RESTART_WIFI_RECOVERY: return "wifiRecovery";
    case PLANNED_RESTART_WORKER_STALL: return "workerStall";
    case PLANNED_RESTART_HTTP_STALL: return "httpStall";
    case PLANNED_RESTART_NONE:
    default:                            return "none";
  }
}

const char* gatewayEventTypeToString(GatewayEventType type) {
  switch (type) {
    case GatewayEventType::BOOT:                   return "boot";
    case GatewayEventType::WIFI_STA_CONNECTED:     return "wifiStaConnected";
    case GatewayEventType::WIFI_GOT_IP:            return "wifiGotIp";
    case GatewayEventType::WIFI_DISCONNECTED:      return "wifiDisconnected";
    case GatewayEventType::WIFI_RECONNECT_ATTEMPT: return "wifiReconnectAttempt";
    case GatewayEventType::WIFI_DRIVER_RESTART:    return "wifiDriverRestart";
    case GatewayEventType::WIFI_DEVICE_RESTART:    return "wifiDeviceRestart";
    case GatewayEventType::WS_SERVER_STARTED:      return "wsServerStarted";
    case GatewayEventType::WS_SERVER_STOPPED:      return "wsServerStopped";
    case GatewayEventType::WS_CLIENT_CONNECTED:    return "wsClientConnected";
    case GatewayEventType::WS_CLIENT_DISCONNECTED: return "wsClientDisconnected";
    case GatewayEventType::WS_SEND_FAILED:         return "wsSendFailed";
    case GatewayEventType::SLOW_LIBRARY_CALL:      return "slowLibraryCall";
    default:                                       return "unknown";
  }
}

void recordGatewayEvent(GatewayEventType type, int32_t value = 0) {
  GatewayEventRecord record = {};
  record.uptimeMs = millis();
  record.type = type;
  record.value = value;
  record.loopAgeMs = lastLoopStartedMs == 0
      ? 0
      : record.uptimeMs - lastLoopStartedMs;
  record.wifiRssi = wifiRssiLastDbm;
  record.wifiStatus = lastSampledWifiStatus;
  record.diagnosticPhase = static_cast<uint8_t>(currentDiagnosticPhase);

  portENTER_CRITICAL(&gatewayEventMux);
  record.sequence = gatewayEventNextSequence++;
  gatewayEventRing[(record.sequence - 1U) % GATEWAY_EVENT_RING_CAPACITY] = record;
  if (gatewayEventStoredCount < GATEWAY_EVENT_RING_CAPACITY) {
    ++gatewayEventStoredCount;
  }
  portEXIT_CRITICAL(&gatewayEventMux);
}

void finishTimedLibraryCall(CallTimingStats& timing,
                            DiagnosticPhase phase,
                            const char* label,
                            uint32_t startedMs) {
  const uint32_t durationMs = millis() - startedMs;
  ++timing.calls;
  timing.lastMs = durationMs;
  if (durationMs > timing.maxMs) {
    timing.maxMs = durationMs;
  }
  if (durationMs < SLOW_LIBRARY_CALL_THRESHOLD_MS) {
    return;
  }

  ++timing.slowCalls;
  const uint32_t nowMs = millis();
  if (timing.lastSlowEventMs != 0 &&
      (nowMs - timing.lastSlowEventMs) < SLOW_LIBRARY_EVENT_INTERVAL_MS) {
    return;
  }
  timing.lastSlowEventMs = nowMs;
  recordGatewayEvent(GatewayEventType::SLOW_LIBRARY_CALL,
                     static_cast<int32_t>(durationMs));
  logMessage("[DIAG] Slow library call name=" + String(label) +
             " durationMs=" + String(durationMs) +
             " phase=" + String(diagnosticPhaseToString(phase)) +
             " heap=" + String(ESP.getFreeHeap()) +
             " rssi=" + String(wifiRssiLastDbm));
}

void appendCallTiming(JsonObject parent,
                      const char* name,
                      const CallTimingStats& timing) {
  JsonObject item = parent[name].to<JsonObject>();
  item["calls"] = timing.calls;
  item["lastMs"] = timing.lastMs;
  item["maxMs"] = timing.maxMs;
  item["slowCalls"] = timing.slowCalls;
  item["lastSlowEventMs"] = timing.lastSlowEventMs;
}

void appendPreviousBreadcrumb(JsonObject parent) {
  parent["valid"] = previousGatewayBreadcrumbValid;
  if (!previousGatewayBreadcrumbValid) {
    return;
  }

  parent["bootId"] = previousGatewayBreadcrumb.bootId;
  parent["bootCounter"] = previousGatewayBreadcrumb.bootCounter;
  parent["uptimeMs"] = previousGatewayBreadcrumb.uptimeMs;
  parent["phase"] = diagnosticPhaseToString(
      static_cast<DiagnosticPhase>(previousGatewayBreadcrumb.phase));
  parent["phaseEnteredMs"] = previousGatewayBreadcrumb.phaseEnteredMs;
  parent["lastLoopStartedMs"] = previousGatewayBreadcrumb.lastLoopStartedMs;
  parent["lastLoopCompletedMs"] = previousGatewayBreadcrumb.lastLoopCompletedMs;
  parent["loopCount"] = previousGatewayBreadcrumb.loopCount;
  parent["heapFree"] = previousGatewayBreadcrumb.heapFree;
  parent["wifiRssi"] = previousGatewayBreadcrumb.wifiRssi;
  parent["wifiStatus"] = wifiStatusToString(
      static_cast<wl_status_t>(previousGatewayBreadcrumb.wifiStatus));
  parent["appStateRaw"] = previousGatewayBreadcrumb.appState;
  parent["wsActiveClientId"] = previousGatewayBreadcrumb.wsActiveClientId;
  parent["wifiReady"] = (previousGatewayBreadcrumb.flags & 0x01U) != 0;
  parent["wsReady"] = (previousGatewayBreadcrumb.flags & 0x02U) != 0;
  parent["wsServerStarted"] = (previousGatewayBreadcrumb.flags & 0x04U) != 0;
}

uint32_t getFirstStoredGatewayEventSequence() {
  uint32_t firstSequence = 1;
  portENTER_CRITICAL(&gatewayEventMux);
  firstSequence = gatewayEventNextSequence - gatewayEventStoredCount;
  portEXIT_CRITICAL(&gatewayEventMux);
  return firstSequence;
}

bool copyGatewayEvent(uint32_t sequence, GatewayEventRecord& outRecord) {
  bool found = false;
  portENTER_CRITICAL(&gatewayEventMux);
  const uint32_t firstSequence = gatewayEventNextSequence - gatewayEventStoredCount;
  if (sequence >= firstSequence && sequence < gatewayEventNextSequence) {
    GatewayEventRecord candidate =
        gatewayEventRing[(sequence - 1U) % GATEWAY_EVENT_RING_CAPACITY];
    if (candidate.sequence == sequence) {
      outRecord = candidate;
      found = true;
    }
  }
  portEXIT_CRITICAL(&gatewayEventMux);
  return found;
}

void initializeGatewayHealth() {
  bootResetReason = esp_reset_reason();
  bootId = esp_random();
  if (bootId == 0) {
    bootId = 1;
  }

  healthPreferencesReady = healthPreferences.begin(HEALTH_NAMESPACE, false);
  if (healthPreferencesReady) {
    bootCounter = healthPreferences.getUInt(HEALTH_BOOT_COUNTER_KEY, 0) + 1U;
    previousPlannedRestartReason =
        healthPreferences.getUInt(HEALTH_PLANNED_REASON_KEY, PLANNED_RESTART_NONE);
    wifiRecoveryRestartCount =
        healthPreferences.getUInt(HEALTH_WIFI_RESTARTS_KEY, 0);
    healthPreferences.putUInt(HEALTH_BOOT_COUNTER_KEY, bootCounter);
    healthPreferences.putUInt(HEALTH_PLANNED_REASON_KEY, PLANNED_RESTART_NONE);
  }

  initializeRtcBreadcrumb();
  recordGatewayEvent(GatewayEventType::BOOT,
                     static_cast<int32_t>(bootResetReason));
  logMessage("[BOOT] firmware=" + String(FIRMWARE_BUILD_ID) +
             " bootId=" + String(bootId) +
             " bootCounter=" + String(bootCounter) +
             " resetReason=" + String(resetReasonToString(bootResetReason)) +
             "(" + String(static_cast<int>(bootResetReason)) + ")" +
             " plannedRestart=" +
             String(plannedRestartReasonToString(previousPlannedRestartReason)) +
             " wifiRecoveryRestarts=" + String(wifiRecoveryRestartCount) +
             " healthNvs=" +
             String(healthPreferencesReady ? "ready" : "unavailable"));
  if (previousGatewayBreadcrumbValid) {
    logMessage("[BOOT] Previous RTC breadcrumb phase=" +
               String(diagnosticPhaseToString(static_cast<DiagnosticPhase>(
                   previousGatewayBreadcrumb.phase))) +
               " uptimeMs=" + String(previousGatewayBreadcrumb.uptimeMs) +
               " lastLoopCompletedMs=" +
               String(previousGatewayBreadcrumb.lastLoopCompletedMs) +
               " loopCount=" + String(previousGatewayBreadcrumb.loopCount) +
               " wifiStatus=" +
               String(wifiStatusToString(static_cast<wl_status_t>(
                   previousGatewayBreadcrumb.wifiStatus))) +
               " rssi=" + String(previousGatewayBreadcrumb.wifiRssi) +
               " heap=" + String(previousGatewayBreadcrumb.heapFree));
  } else {
    logMessage("[BOOT] Previous RTC breadcrumb unavailable.");
  }
}

void onWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  switch (event) {
    case ARDUINO_EVENT_WIFI_STA_CONNECTED:
      recordGatewayEvent(GatewayEventType::WIFI_STA_CONNECTED);
      break;
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      ++wifiGotIpCount;
      recordGatewayEvent(GatewayEventType::WIFI_GOT_IP);
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      ++wifiDisconnectCount;
      lastWifiDisconnectReason = info.wifi_sta_disconnected.reason;
      lastWifiDisconnectUptimeMs = millis();
      lastWifiDisconnectRssiDbm = wifiRssiLastDbm;
      lastWifiDisconnectStatus = lastSampledWifiStatus;
      lastWifiDisconnectPhase = currentDiagnosticPhase;
      lastWifiDisconnectLoopAgeMs = lastLoopStartedMs == 0
          ? 0
          : lastWifiDisconnectUptimeMs - lastLoopStartedMs;
      recordGatewayEvent(GatewayEventType::WIFI_DISCONNECTED,
                         static_cast<int32_t>(lastWifiDisconnectReason));
      break;
    default:
      break;
  }
}

String makeGatewayHelloJson() {
  DynamicJsonDocument doc(1536);
  doc["type"] = "gatewayHello";
  doc["firmware"] = FIRMWARE_BUILD_ID;
  doc["diagnosticVersion"] = GATEWAY_DIAGNOSTIC_VERSION;
  doc["bootId"] = bootId;
  doc["bootCounter"] = bootCounter;
  doc["resetReason"] = resetReasonToString(bootResetReason);
  doc["resetReasonRaw"] = static_cast<int>(bootResetReason);
  doc["plannedRestart"] =
      plannedRestartReasonToString(previousPlannedRestartReason);
  doc["wifiRecoveryRestarts"] = wifiRecoveryRestartCount;
  doc["uptimeMs"] = millis();
  doc["diagnosticPhase"] = diagnosticPhaseToString(currentDiagnosticPhase);
  doc["recoveredDelivery"] = pendingDirectDelivery.recoveredAfterRestart ||
                              pendingDirectDeliveryResponse.data.recoveredAfterRestart;
  appendPreviousBreadcrumb(
      doc["previousBreadcrumb"].to<JsonObject>());

  String json;
  serializeJson(doc, json);
  return json;
}

String makeGatewayStatusJson() {
  const uint32_t nowMs = millis();
  DynamicJsonDocument doc(3072);
  doc["type"] = "gatewayStatus";
  doc["diagnosticVersion"] = GATEWAY_DIAGNOSTIC_VERSION;
  doc["bootId"] = bootId;
  doc["bootCounter"] = bootCounter;
  doc["uptimeMs"] = nowMs;
  doc["state"] = stateToString(appState);
  doc["wifiStatus"] = wifiStatusToString(WiFi.status());
  doc["wifiReady"] = wifiReady;
  doc["wifiRssi"] = wifiRssiLastDbm;
  doc["wifiChannel"] = WiFi.channel();
  doc["wifiBssid"] = WiFi.BSSIDstr();
  doc["wifiPowerSaveMode"] = static_cast<int>(WiFi.getSleep());
  doc["wifiTxPower"] = static_cast<int>(WiFi.getTxPower());
  doc["wifiOfflineMs"] =
      wifiOfflineSinceMs == 0 ? 0 : nowMs - wifiOfflineSinceMs;
  doc["wifiDisconnects"] = wifiDisconnectCount;
  doc["wifiGotIp"] = wifiGotIpCount;
  doc["wifiReconnectOwner"] = "arduinoAuto";
  doc["wifiDriverRestarts"] = wifiDriverRestartCount;
  doc["wsServerStarted"] = wsServerStarted;
  doc["wsReady"] = wsReady;
  doc["wsConnects"] = wsConnectCount;
  doc["wsDisconnects"] = wsDisconnectCount;
  doc["wsSendFailures"] = wsSendFailureCount;
  doc["wsServerRestarts"] = wsServerRestartCount;
  doc["lastWsRxMs"] = lastWsRxMs;
  doc["lastWsTxMs"] = lastWsTxMs;
  doc["heapFree"] = ESP.getFreeHeap();
  doc["heapMinFree"] = ESP.getMinFreeHeap();
  doc["diagnosticPhase"] = diagnosticPhaseToString(currentDiagnosticPhase);
  doc["loopCount"] = loopCount;
  doc["lastLoopDurationMs"] = lastLoopDurationMs;
  doc["maxLoopDurationMs"] = maxLoopDurationMs;
  doc["maxLoopGapMs"] = maxLoopGapMs;
  doc["lastF411Event"] = lastF411Event;
  doc["lastF411Status"] = lastF411Status;
  doc["lastF411Count"] = lastF411Count;
  doc["lastF411State"] = lastF411State;
  doc["lastF103Event"] = lastF103Event;
  doc["lastF103Status"] = lastF103Status;
  doc["lastF103CommandCounter"] = lastF103CommandCounter;
  doc["lastF103Line"] = lastF103Line;

  JsonObject radio = doc["radio"].to<JsonObject>();
  radio["samples"] = wifiRssiSampleCount;
  radio["rssiLast"] = wifiRssiLastDbm;
  radio["rssiMin"] = wifiRssiSampleCount == 0 ? 0 : wifiRssiMinDbm;
  radio["rssiMax"] = wifiRssiSampleCount == 0 ? 0 : wifiRssiMaxDbm;
  radio["below80Samples"] = wifiRssiBelow80SampleCount;
  radio["below90Samples"] = wifiRssiBelow90SampleCount;

  JsonObject lastDisconnect = doc["lastWifiDisconnect"].to<JsonObject>();
  lastDisconnect["reason"] = lastWifiDisconnectReason;
  lastDisconnect["reasonName"] =
      wifiDisconnectReasonToString(lastWifiDisconnectReason);
  lastDisconnect["uptimeMs"] = lastWifiDisconnectUptimeMs;
  lastDisconnect["rssi"] = lastWifiDisconnectRssiDbm;
  lastDisconnect["wifiStatus"] = wifiStatusToString(
      static_cast<wl_status_t>(lastWifiDisconnectStatus));
  lastDisconnect["phase"] = diagnosticPhaseToString(lastWifiDisconnectPhase);
  lastDisconnect["loopAgeMs"] = lastWifiDisconnectLoopAgeMs;

  JsonObject callTiming = doc["callTiming"].to<JsonObject>();
  appendCallTiming(callTiming, "wifiBegin", wifiBeginTiming);
  appendCallTiming(callTiming, "wsServerClose", wsServerCloseTiming);
  appendCallTiming(callTiming, "wsLoop", wsLoopTiming);
  appendCallTiming(callTiming, "wsSend", wsSendTiming);

  String json;
  serializeJson(doc, json);
  return json;
}

String makeGatewayEventJson(const GatewayEventRecord& event) {
  DynamicJsonDocument doc(640);
  doc["type"] = "gatewayEvent";
  doc["bootId"] = bootId;
  doc["bootCounter"] = bootCounter;
  doc["sequence"] = event.sequence;
  doc["uptimeMs"] = event.uptimeMs;
  doc["event"] = gatewayEventTypeToString(event.type);
  doc["value"] = event.value;
  doc["wifiRssi"] = event.wifiRssi;
  doc["wifiStatus"] = wifiStatusToString(
      static_cast<wl_status_t>(event.wifiStatus));
  doc["phase"] = diagnosticPhaseToString(
      static_cast<DiagnosticPhase>(event.diagnosticPhase));
  doc["loopAgeMs"] = event.loopAgeMs;

  String json;
  serializeJson(doc, json);
  return json;
}

void changeState(AppState newState) {
  appState = newState;
  stateEnterMs = millis();
  logMessage("[STATE] " + String(stateToString(appState)));
}

void beginNewOperationContext() {
  // An accepted cancellation belongs to the previous purchase until ACK/TTL.
  // Do not silently delete it when the user makes a new selection.
  if (++operationGeneration == 0) operationGeneration = 1;
}

void clearActivePayment() {
  httpsTransport.cancel(activePaymentRequestHandle);
  if (paymentJournalReady) paymentPreferences.remove("flow");
  pendingPaymentDecision = "";
  paymentJournalRecord = "";
  activePayment.active = false;
  activePayment.transactionId = 0;
  activePayment.bootIdentity = 0;
  activePayment.productPositionId = 0;
  activePayment.priceMinor = 0;
  activePayment.productPrice = 0.0;
  activePayment.deliveryLine = 0;
  activePayment.paymentReplyForwardedToStm = false;
  activePayment.cancelledByMachine = false;
  activePaymentRequestHandle = {};
  activePaymentRequestSentToWs = false;
}

void setActivePayment(const PaymentRequestData& request) {
  beginNewOperationContext();
  activePayment.active = true;
  activePayment.transactionId = request.transactionId;
  activePayment.bootIdentity = request.bootIdentity;
  activePayment.productPositionId = request.productPositionId;
  activePayment.priceMinor = request.priceMinor;
  activePayment.productPrice = request.productPrice;
  activePayment.deliveryLine = request.deliveryLine;
  activePayment.paymentReplyForwardedToStm = false;
  activePayment.cancelledByMachine = false;
  activePaymentRequestHandle = {};
  activePaymentRequestSentToWs = false;
}

void clearPendingConfig() {
  pendingConfig.active = false;
  pendingConfig.productPositionId = 0;
  pendingConfig.deliveryLine = 0;
}

void setPendingConfig(const ConfigCommandData& cmd) {
  pendingConfig.active = true;
  pendingConfig.productPositionId = cmd.productPositionId;
  pendingConfig.deliveryLine = cmd.deliveryLine;
}

void clearPendingDirectDelivery() {
  directResultNeedsPersistence = false;
  pendingDirectDelivery.active = false;
  pendingDirectDelivery.productPositionId = 0;
  pendingDirectDelivery.deliveryLine = 0;
  pendingDirectDelivery.startStatusReceived = false;
  pendingDirectDelivery.startConfirmed = false;
  pendingDirectDelivery.startTimeoutReported = false;
  pendingDirectDelivery.motorBoardStatus = "";
  pendingDirectDelivery.startReason = "";
  pendingDirectDelivery.operationId = 0;
  pendingDirectDelivery.recoveredAfterRestart = false;
  pendingDirectDelivery.recoveryNoticePending = false;
}

void setPendingDirectDelivery(const DirectDeliveryRequestData& request,
                              uint32_t operationId) {
  beginNewOperationContext();
  nextDirectRecoveryMs = millis() + 1000U;
  directResultNeedsPersistence = false;
  pendingDirectDelivery.active = true;
  pendingDirectDelivery.productPositionId = request.productPositionId;
  pendingDirectDelivery.deliveryLine = request.deliveryLine;
  pendingDirectDelivery.startStatusReceived = false;
  pendingDirectDelivery.startConfirmed = false;
  pendingDirectDelivery.startTimeoutReported = false;
  pendingDirectDelivery.motorBoardStatus = "unknown";
  pendingDirectDelivery.startReason = "waitingForF103";
  pendingDirectDelivery.operationId = operationId;
  pendingDirectDelivery.recoveredAfterRestart = false;
  pendingDirectDelivery.recoveryNoticePending = false;
}

uint32_t calculateDeliveryJournalChecksum(const DeliveryJournalRecord& record) {
  const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&record);
  const size_t length = offsetof(DeliveryJournalRecord, checksum);
  uint32_t checksum = 2166136261U;

  for (size_t index = 0; index < length; ++index) {
    checksum ^= bytes[index];
    checksum *= 16777619U;
  }
  return checksum;
}

bool isDeliveryJournalRecordValid(const DeliveryJournalRecord& record) {
  bool phaseValid = record.phase == static_cast<uint8_t>(DeliveryJournalPhase::ACTIVE) ||
                    record.phase == static_cast<uint8_t>(DeliveryJournalPhase::RESULT);
  return record.magic == DELIVERY_JOURNAL_MAGIC &&
         record.version == DELIVERY_JOURNAL_VERSION &&
         record.size == sizeof(DeliveryJournalRecord) &&
         record.operationId != 0 &&
         record.productPositionId <= MAX_VALID_PRODUCT_POSITION_ID &&
         record.deliveryLine >= 1 && record.deliveryLine <= 10 &&
         phaseValid &&
         record.checksum == calculateDeliveryJournalChecksum(record);
}

bool writeDeliveryJournalRecord(DeliveryJournalRecord& record) {
  if (!deliveryJournalReady) {
    logMessage("[JOURNAL] NVS недоступна, запись direct delivery отклонена.");
    return false;
  }

  record.magic = DELIVERY_JOURNAL_MAGIC;
  record.version = DELIVERY_JOURNAL_VERSION;
  record.size = sizeof(DeliveryJournalRecord);
  record.checksum = calculateDeliveryJournalChecksum(record);
  size_t written = deliveryPreferences.putBytes(
      DELIVERY_JOURNAL_KEY, &record, sizeof(record));
  if (written != sizeof(record)) {
    logMessage("[JOURNAL] Не удалось атомарно сохранить direct delivery record.");
    return false;
  }
  return true;
}

bool clearDeliveryJournal() {
  if (!deliveryJournalReady) {
    return false;
  }
  if (!deliveryPreferences.isKey(DELIVERY_JOURNAL_KEY)) {
    return true;
  }
  bool removed = deliveryPreferences.remove(DELIVERY_JOURNAL_KEY);
  if (!removed) {
    logMessage("[JOURNAL] Не удалось очистить завершённую direct delivery.");
  }
  return removed;
}

bool clearConfirmedDirectDeliveryJournal(uint32_t expectedOperationId) {
  // Legacy in-memory results were not journal owners. They may complete, but must
  // never remove a record that belongs to a newer durable operation.
  if (expectedOperationId == 0) return true;

  static uint32_t loggedOperationId = 0;
  static uint8_t loggedReason = 0;
  auto logFailureOnce = [&](uint8_t reason, const String& message) {
    if (loggedOperationId == expectedOperationId && loggedReason == reason) return;
    loggedOperationId = expectedOperationId;
    loggedReason = reason;
    logMessage(message);
  };

  if (!deliveryJournalReady) {
    logFailureOnce(1, "[JOURNAL] ACK direct delivery: NVS недоступна; RESULT сохранён в pending.");
    return false;
  }
  if (!deliveryPreferences.isKey(DELIVERY_JOURNAL_KEY)) {
    // Idempotent retry after a successful remove and before RAM cleanup.
    loggedOperationId = 0;
    loggedReason = 0;
    return true;
  }

  DeliveryJournalRecord record = {};
  const size_t loaded = deliveryPreferences.getBytes(
      DELIVERY_JOURNAL_KEY, &record, sizeof(record));
  if (loaded != sizeof(record) || !isDeliveryJournalRecordValid(record)) {
    logFailureOnce(2, "[JOURNAL] ACK direct delivery: record повреждён/не читается; очистка запрещена.");
    return false;
  }
  if (record.phase != static_cast<uint8_t>(DeliveryJournalPhase::RESULT) ||
      record.operationId != expectedOperationId) {
    logFailureOnce(3, "[JOURNAL] ACK direct delivery: owner/phase mismatch; очистка запрещена.");
    return false;
  }
  if (!deliveryPreferences.remove(DELIVERY_JOURNAL_KEY)) {
    logFailureOnce(4, "[JOURNAL] ACK direct delivery: не удалось удалить подтверждённый RESULT.");
    return false;
  }
  loggedOperationId = 0;
  loggedReason = 0;
  return true;
}

bool persistActiveDirectDelivery(const DirectDeliveryRequestData& request,
                                 uint32_t operationId) {
  if (operationId == 0) return false;
  DeliveryJournalRecord record = {};
  record.operationId = operationId;
  record.productPositionId = request.productPositionId;
  record.deliveryLine = request.deliveryLine;
  record.phase = static_cast<uint8_t>(DeliveryJournalPhase::ACTIVE);
  return writeDeliveryJournalRecord(record);
}

bool persistDirectDeliveryResult(const DirectDeliveryResultData& result,
                                 uint16_t productPositionId) {
  DeliveryJournalRecord record = {};
  record.operationId = result.operationId;
  record.productPositionId = productPositionId;
  record.deliveryLine = result.deliveryLine;
  record.phase = static_cast<uint8_t>(DeliveryJournalPhase::RESULT);
  record.resultSuccess = result.snackMachineStatus == "success" ? 1U : 0U;
  return writeDeliveryJournalRecord(record);
}

uint32_t createDirectDeliveryOperationId() {
  if (!deliveryJournalReady) return 0;
  uint32_t last = deliveryPreferences.getUInt("lastop", 0);
  // Preserve an older ACTIVE/RESULT identity during migration as well.
  DeliveryJournalRecord record = {};
  if (deliveryPreferences.getBytes(DELIVERY_JOURNAL_KEY, &record, sizeof(record)) == sizeof(record)
      && isDeliveryJournalRecordValid(record) && record.operationId > last) last = record.operationId;
  if (last == UINT32_MAX) return 0;
  uint32_t next = last + 1U;
  return deliveryPreferences.putUInt("lastop", next) == sizeof(next) ? next : 0;
}

void initializeAndRestoreDeliveryJournal() {
  deliveryJournalReady = deliveryPreferences.begin(
      DELIVERY_JOURNAL_NAMESPACE, false);
  logMessage("[BOOT] resetReason=" + String(static_cast<int>(esp_reset_reason())) +
             ", deliveryJournal=" +
             String(deliveryJournalReady ? "ready" : "unavailable"));
  if (!deliveryJournalReady ||
      !deliveryPreferences.isKey(DELIVERY_JOURNAL_KEY)) {
    return;
  }

  DeliveryJournalRecord record = {};
  size_t loaded = deliveryPreferences.getBytes(
      DELIVERY_JOURNAL_KEY, &record, sizeof(record));
  if (loaded != sizeof(record) || !isDeliveryJournalRecordValid(record)) {
    logMessage("[JOURNAL] Повреждённая/несовместимая запись удалена без запуска мотора.");
    clearDeliveryJournal();
    return;
  }

  if (record.phase == static_cast<uint8_t>(DeliveryJournalPhase::ACTIVE)) {
    DirectDeliveryRequestData request;
    request.productPositionId = record.productPositionId;
    request.deliveryLine = record.deliveryLine;
    setPendingDirectDelivery(request, record.operationId);
    pendingDirectDelivery.startTimeoutReported = true;
    pendingDirectDelivery.recoveredAfterRestart = true;
    pendingDirectDelivery.recoveryNoticePending = true;
    changeState(AppState::WAITING_STM_DIRECT_DELIVERY_RESULT);
    logMessage("[JOURNAL] Восстановлено ожидание DELIVERY_RESULT: line=" +
               String(record.deliveryLine) + ", operationId=" +
               String(record.operationId));
    return;
  }

  pendingDirectDeliveryResponse.active = true;
  pendingDirectDeliveryResponse.data.deliveryLine = record.deliveryLine;
  pendingDirectDeliveryResponse.data.statusRaw =
      record.resultSuccess != 0 ? "success" : "failed";
  pendingDirectDeliveryResponse.data.snackMachineStatus =
      pendingDirectDeliveryResponse.data.statusRaw;
  pendingDirectDeliveryResponse.data.operationId = record.operationId;
  pendingDirectDeliveryResponse.data.recoveredAfterRestart = true;
  pendingDirectDeliveryResponse.deadlineMs = millis() + STATE_TIMEOUT_WS_SEND_MS;
  directResponseRetryMs = 0;
  changeState(AppState::IDLE);
  logMessage("[JOURNAL] Восстановлен неотправленный DELIVERY_RESULT: line=" +
             String(record.deliveryLine) + ", operationId=" +
             String(record.operationId));
}

void clearPendingLineCheck() {
  pendingLineCheck.active = false;
  pendingLineCheck.deliveryLine = 0;
  pendingLineCheck.httpResponseToken = 0;
}

void setPendingLineCheck(const DeliveryLineCheckRequestData& request,
                         uint32_t httpResponseToken) {
  pendingLineCheck.active = true;
  pendingLineCheck.deliveryLine = request.deliveryLine;
  pendingLineCheck.httpResponseToken = httpResponseToken;
}

void clearPendingVendReportSend() {
  httpsTransport.release(pendingVendTransportHandle);
  pendingVendReportSend.active = false;
  pendingVendReportSend.deadlineMs = 0;
  pendingVendTransportHandle = {};
}

void clearPendingDirectDeliveryResponse() {
  httpsTransport.release(pendingDirectResponseTransportHandle);
  pendingDirectDeliveryResponse.active = false;
  pendingDirectDeliveryResponse.deadlineMs = 0;
  pendingDirectResponseTransportHandle = {};
  directResponseRetryMs = 0;
}

void clearPendingOrderCancellationRequest() {
  httpsTransport.cancel(pendingCancellationTransportHandle);
  pendingOrderCancellationRequest.active = false;
  pendingOrderCancellationRequest.deadlineMs = 0;
  pendingCancellationTransportHandle = {};
  pendingCancellationGeneration = 0;
}

String makeWsUrlForLog() {
  String url = "ws://";
  url += WiFi.localIP().toString();
  if (WS_PORT != 80) {
    url += ":";
    url += String(WS_PORT);
  }
  url += WS_PATH;
  return url;
}

void printNetworkDiagnostics() {
  sampleRadioDiagnostics(true);
  logMessage("[NET] Wi-Fi status: " + String(wifiStatusToString(WiFi.status())));
  logMessage("[NET] IP: " + WiFi.localIP().toString());
  logMessage("[NET] Gateway: " + WiFi.gatewayIP().toString());
  logMessage("[NET] DNS: " + WiFi.dnsIP().toString());
  logMessage("[NET] BSSID: " + WiFi.BSSIDstr() +
             " channel=" + String(WiFi.channel()) +
             " RSSI=" + String(wifiRssiLastDbm) + " dBm" +
             " powerSave=" + String(static_cast<int>(WiFi.getSleep())) +
             " txPower=" + String(static_cast<int>(WiFi.getTxPower())));
  logMessage("[NET] Free heap: " + String(ESP.getFreeHeap()) + " bytes");
}

bool isStrictUnsignedInteger(const String& text) {
  if (text.length() == 0) {
    return false;
  }

  for (size_t i = 0; i < text.length(); ++i) {
    if (!isDigit(static_cast<unsigned char>(text[i]))) {
      return false;
    }
  }

  return true;
}

bool parseUInt32Strict(const String& text, uint32_t& outValue) {
  if (!isStrictUnsignedInteger(text)) {
    return false;
  }

  char buffer[24];
  if (text.length() >= sizeof(buffer)) {
    return false;
  }

  text.toCharArray(buffer, sizeof(buffer));
  char* endPtr = nullptr;
  errno = 0;
  unsigned long value = strtoul(buffer, &endPtr, 10);

  if (*endPtr != '\0' || errno == ERANGE || value > UINT32_MAX) {
    return false;
  }

  outValue = static_cast<uint32_t>(value);
  return true;
}

bool parseUInt8InRange(const String& text, uint8_t minValue, uint8_t maxValue, uint8_t& outValue) {
  uint32_t parsed = 0;
  if (!parseUInt32Strict(text, parsed)) {
    return false;
  }

  if (parsed < minValue || parsed > maxValue) {
    return false;
  }

  outValue = static_cast<uint8_t>(parsed);
  return true;
}

bool parseUInt16InRange(const String& text, uint16_t minValue, uint16_t maxValue, uint16_t& outValue) {
  uint32_t parsed = 0;
  if (!parseUInt32Strict(text, parsed)) {
    return false;
  }

  if (parsed < minValue || parsed > maxValue) {
    return false;
  }

  outValue = static_cast<uint16_t>(parsed);
  return true;
}

bool readJsonIntField(const DynamicJsonDocument& doc, const char* key, int& outValue) {
  if (key == nullptr || !doc[key].is<int>()) {
    return false;
  }

  outValue = doc[key].as<int>();
  return true;
}

bool readJsonIntFieldAny(const DynamicJsonDocument& doc, const char* key1,
                         const char* key2, const char* key3, int& outValue) {
  return readJsonIntField(doc, key1, outValue) ||
         readJsonIntField(doc, key2, outValue) ||
         readJsonIntField(doc, key3, outValue);
}

bool isControllerAddressedToThisDevice(const DynamicJsonDocument& doc) {
  int controllerValue = 0;
  if (!readJsonIntFieldAny(doc, "controllerId", "controllerid", "controllerld", controllerValue)) {
    return true;
  }

  return controllerValue == 0 || controllerValue == static_cast<int>(CONTROLLER_ID);
}

void setSelectionMotorMapping(uint16_t selectionId, uint8_t motor) {
  size_t firstFree = SELECTION_MAP_CAPACITY;

  for (size_t i = 0; i < SELECTION_MAP_CAPACITY; ++i) {
    if (selectionMotorMap[i].used && selectionMotorMap[i].selectionId == selectionId) {
      selectionMotorMap[i].motor = motor;
      return;
    }

    if (!selectionMotorMap[i].used && firstFree == SELECTION_MAP_CAPACITY) {
      firstFree = i;
    }
  }

  if (firstFree == SELECTION_MAP_CAPACITY) {
    firstFree = SELECTION_MAP_CAPACITY - 1;
  }

  selectionMotorMap[firstFree].used = true;
  selectionMotorMap[firstFree].selectionId = selectionId;
  selectionMotorMap[firstFree].motor = motor;
}

uint8_t findMotorBySelectionId(uint16_t selectionId) {
  for (size_t i = 0; i < SELECTION_MAP_CAPACITY; ++i) {
    if (selectionMotorMap[i].used && selectionMotorMap[i].selectionId == selectionId) {
      return selectionMotorMap[i].motor;
    }
  }

  return 0;
}

void initDefaultSelectionMap() {
  for (uint8_t i = 0; i < 10; ++i) {
    setSelectionMotorMapping(static_cast<uint16_t>(101 + i), static_cast<uint8_t>(i + 1));
  }
}

bool extractTokenValue(const String& token, const String& expectedKey, String& outValue) {
  int eqPos = token.indexOf('=');
  if (eqPos <= 0) {
    return false;
  }

  String key = token.substring(0, eqPos);
  key.trim();

  if (key != expectedKey) {
    return false;
  }

  outValue = token.substring(eqPos + 1);
  outValue.trim();
  return true;
}

bool splitByComma(const String& line, String tokens[], size_t maxTokens, size_t& outCount) {
  outCount = 0;

  int start = 0;
  while (start <= line.length()) {
    if (outCount >= maxTokens) {
      return false;
    }

    int commaPos = line.indexOf(',', start);
    if (commaPos < 0) {
      tokens[outCount++] = line.substring(start);
      break;
    }

    tokens[outCount++] = line.substring(start, commaPos);
    start = commaPos + 1;
  }

  for (size_t i = 0; i < outCount; ++i) {
    tokens[i].trim();
  }

  return true;
}

UsbIncomingLineType detectUsbIncomingLineType(const String& line) {
  if (line.startsWith("PAYMENT_REQUEST")) {
    return UsbIncomingLineType::PAYMENT_REQUEST;
  }
  if (line.startsWith("VEND_REPORT")) {
    return UsbIncomingLineType::VEND_REPORT;
  }
  if (line.startsWith("SET_MAP_RESULT")) {
    return UsbIncomingLineType::SET_MAP_RESULT;
  }
  if (line.startsWith("DELIVERY_START_RESULT")) {
    return UsbIncomingLineType::DELIVERY_START_RESULT;
  }
  if (line.startsWith("DELIVERY_RESULT")) {
    return UsbIncomingLineType::DELIVERY_RESULT;
  }
  if (line.startsWith("CONTROLLER_DIAGNOSTIC")) {
    return UsbIncomingLineType::CONTROLLER_DIAGNOSTIC;
  }
  if (line.startsWith("ORDER_CANCELLED,tx=")) {
    return UsbIncomingLineType::ORDER_CANCELLED;
  }
  if (line.startsWith("DELIVERY_LINE_CHECK_RESULT")) {
    return UsbIncomingLineType::DELIVERY_LINE_CHECK_RESULT;
  }
  return UsbIncomingLineType::UNKNOWN;
}

bool isValidWsPaymentStatus(const String& value) {
  return value == "paid" || value == "error";
}

String makeRequestStatusJson(const String& status);
String makeDirectDeliveryRequestStatusJson(const String& status,
                                           uint16_t deliveryLine,
                                           const String& motorCommandStatus,
                                           const String& motorBoardStatus,
                                           const String& reason,
                                           uint32_t operationId,
                                           bool recoveredAfterRestart);
String makeOrderCancellationRequestJson();
String makeDeliveryLineCheckResponseJson(uint16_t deliveryLine, const String& status);

bool isValidMachineStatusRaw(const String& value) {
  return value == "success" || value == "failure" || value == "session_complete" || value == "cancelled";
}

bool isValidLocalStatusRaw(const String& value) {
  return value == "success" || value == "fail" || value == "skip";
}

String mapMachineStatusToWs(const String& machineStatusRaw) {
  if (machineStatusRaw == "success") {
    return "success";
  }
  if (machineStatusRaw == "failure") {
    return "failed";
  }
  if (machineStatusRaw == "session_complete" || machineStatusRaw == "cancelled") {
    return "skipped";
  }
  return "failed";
}

String mapLocalStatusToWs(const String& localStatusRaw) {
  if (localStatusRaw == "success") {
    return "success";
  }
  if (localStatusRaw == "fail") {
    return "failed";
  }
  if (localStatusRaw == "skip") {
    return "skipped";
  }
  return "failed";
}

String makeCoffeeMachineStatusForApi(const String& machineStatusRaw, const String& snackMachineStatus) {
  (void)snackMachineStatus;
  return mapMachineStatusToWs(machineStatusRaw);
}

bool readByteFromStm(char& outChar) {
#if STM_USB_HOST_MODE
  serviceStmTransport();
  if (!isStmUsbMounted() || STM_USB.available() <= 0) {
    return false;
  }

  int value = STM_USB.read();
  if (value < 0) {
    return false;
  }

  outChar = static_cast<char>(value);
  return true;
#else
  if (STM_PORT.available() <= 0) {
    return false;
  }

  outChar = static_cast<char>(STM_PORT.read());
  return true;
#endif
}

bool readLineFromStm(String& outLine) {
  char c;
  while (readByteFromStm(c)) {
    controllerLink.noteByte(millis());

    if (c == '\r') {
      continue;
    }

    if (c == '\n') {
      if (stmRxBuffer.length() == 0) {
        continue;
      }

      outLine = stmRxBuffer;
      stmRxBuffer = "";
      return true;
    }

    if (stmRxBuffer.length() < STM_RX_LINE_LIMIT) {
      stmRxBuffer += c;
    } else {
      logMessage("[STM] Строка слишком длинная, буфер сброшен.");
      stmRxBuffer = "";
      return false;
    }
  }

  return false;
}

bool sendLineToStm(const String& line) {
#if STM_USB_HOST_MODE
  if (!waitForStmUsbMounted(STM_USB_MOUNT_WAIT_MS)) {
    logMessage("[STM TX] FAIL USB HOST NOT READY: " + line);
    return false;
  }

  String framedLine = line + "\r\n";
  if (framedLine.length() > STM_USB_TX_BUFFER_SIZE) {
    logMessage("[STM TX] FAIL line too long: " + line);
    return false;
  }

  size_t written = STM_USB.write(
      reinterpret_cast<const uint8_t*>(framedLine.c_str()),
      framedLine.length());
  bool ok = (written == framedLine.length());
  logMessage(String(ok ? "[STM TX] " : "[STM TX] FAIL: ") + line);
  return ok;
#else
  size_t written = STM_PORT.print(line);
  written += STM_PORT.print("\r\n");
  STM_PORT.flush();
  bool ok = (written >= line.length() + 2);

  logMessage(String(ok ? "[STM TX] " : "[STM TX] FAIL: ") + line);
  return ok;
#endif
}

bool parseUsbPaymentRequestLine(const String& line, PaymentRequestData& outData) {
  String tokens[6];
  size_t tokenCount = 0;
  if (!splitByComma(line, tokens, 6, tokenCount)) {
    return false;
  }

  if (tokenCount != 6 || tokens[0] != "PAYMENT_REQUEST") {
    return false;
  }

  String idStr;
  String priceStr;
  if (!extractTokenValue(tokens[1], "id", idStr) ||
      !extractTokenValue(tokens[2], "price", priceStr)) {
    return false;
  }

  uint16_t idValue = 0;
  uint32_t priceMinor = 0;
  if (!parseUInt16InRange(idStr, MIN_VALID_PRODUCT_POSITION_ID, MAX_VALID_MDB_SELECTION_ID, idValue) ||
      !parseUInt32Strict(priceStr, priceMinor)) {
    return false;
  }

  outData.productPositionId = idValue;
  outData.priceMinor = priceMinor;
  outData.productPrice = static_cast<double>(priceMinor) / PRICE_MINOR_DIVISOR;
  String txStr, lineStr;
  if (!extractTokenValue(tokens[3], "tx", txStr) ||
      !extractTokenValue(tokens[4], "line", lineStr) ||
      !parseUInt32Strict(txStr, outData.transactionId) || outData.transactionId == 0 ||
      !parseUInt16InRange(lineStr, 0, 10, outData.deliveryLine)) return false;
  String bootStr;
  if (!extractTokenValue(tokens[5], "boot", bootStr) ||
      !parseUInt32Strict(bootStr, outData.bootIdentity) || outData.bootIdentity == 0) return false;
  return true;
}

bool parseUsbDeliveryResultLine(const String& line, VendReportData& outData) {
  String tokens[6];
  size_t tokenCount = 0;
  if (!splitByComma(line, tokens, 6, tokenCount)) {
    return false;
  }

  if (tokenCount != 6 || tokens[0] != "VEND_REPORT") {
    return false;
  }

  String idStr;
  String machineStatus;
  String localStatus;
  if (!extractTokenValue(tokens[1], "id", idStr) ||
      !extractTokenValue(tokens[2], "machine", machineStatus) ||
      !extractTokenValue(tokens[3], "local", localStatus)) {
    return false;
  }

  uint16_t idValue = 0;
  if (!parseUInt16InRange(idStr, MIN_VALID_PRODUCT_POSITION_ID, MAX_VALID_MDB_SELECTION_ID, idValue)) {
    return false;
  }

  if (!isValidMachineStatusRaw(machineStatus) || !isValidLocalStatusRaw(localStatus)) {
    return false;
  }

  String txStr;
  if (!extractTokenValue(tokens[4], "tx", txStr) ||
      !parseUInt32Strict(txStr, outData.transactionId) || outData.transactionId == 0) return false;
  outData.productPositionId = idValue;
  outData.machineStatusRaw = machineStatus;
  outData.localStatusRaw = localStatus;
  outData.snackMachineStatus = mapLocalStatusToWs(localStatus);
  outData.coffeeMachineStatus = makeCoffeeMachineStatusForApi(
      machineStatus,
      outData.snackMachineStatus);
  String bootStr;
  if (!extractTokenValue(tokens[5], "boot", bootStr) ||
      !parseUInt32Strict(bootStr, outData.bootIdentity) || outData.bootIdentity == 0) return false;
  return true;
}

bool parseUsbConfigurationAckLine(const String& line, ConfigAckData& outData) {
  String tokens[5];
  size_t tokenCount = 0;
  if (!splitByComma(line, tokens, 5, tokenCount)) {
    return false;
  }

  if (tokenCount < 2 || tokens[0] != "SET_MAP_RESULT") {
    return false;
  }

  if (tokens[1] == "error") {
    outData.success = false;
    outData.motor = 0;
    outData.productPositionId = 0;
    outData.storageStatus = "unknown";
    return true;
  }

  if ((tokenCount != 4 && tokenCount != 5) || tokens[1] != "ok") {
    return false;
  }

  String motorStr;
  String idStr;
  if (!extractTokenValue(tokens[2], "motor", motorStr) ||
      !extractTokenValue(tokens[3], "id", idStr)) {
    return false;
  }

  uint16_t motor = 0;
  uint16_t id = 0;
  if (!parseUInt16InRange(motorStr, MIN_VALID_DELIVERY_LINE, MAX_VALID_DELIVERY_LINE, motor) ||
      !parseUInt16InRange(idStr, MIN_VALID_PRODUCT_POSITION_ID, MAX_VALID_PRODUCT_POSITION_ID, id)) {
    return false;
  }

  outData.success = true;
  outData.motor = motor;
  outData.productPositionId = id;
  outData.storageStatus = "unknown";
  if (tokenCount == 5) {
    if (!extractTokenValue(tokens[4], "storage", outData.storageStatus) ||
        (outData.storageStatus != "ready" &&
         outData.storageStatus != "unavailable")) {
      return false;
    }
  }
  return true;
}

bool parseDirectDeliveryResultLine(const String& line, DirectDeliveryResultData& outData) {
  String tokens[4];
  size_t tokenCount = 0;
  if (!splitByComma(line, tokens, 4, tokenCount)) {
    return false;
  }

  if (tokenCount != 4 || tokens[0] != "DELIVERY_RESULT") {
    return false;
  }

  String lineStr;
  String status;
  if (!extractTokenValue(tokens[1], "line", lineStr) ||
      !extractTokenValue(tokens[2], "status", status)) {
    return false;
  }

  uint16_t deliveryLine = 0;
  if (!parseUInt16InRange(lineStr, MIN_VALID_DELIVERY_LINE, MAX_VALID_DELIVERY_LINE, deliveryLine)) {
    return false;
  }

  if (status != "success" && status != "failed") {
    return false;
  }

  outData.deliveryLine = deliveryLine;
  outData.statusRaw = status;
  outData.snackMachineStatus = status;
  String opStr;
  if (!extractTokenValue(tokens[3], "op", opStr) ||
      !parseUInt32Strict(opStr, outData.operationId) || outData.operationId == 0) return false;
  outData.recoveredAfterRestart = false;
  return true;
}

bool parseDirectDeliveryStartResultLine(const String& line,
                                        DirectDeliveryStartResultData& outData) {
  String tokens[6];
  size_t tokenCount = 0;
  if (!splitByComma(line, tokens, 6, tokenCount)) {
    return false;
  }

  if (tokenCount != 6 || tokens[0] != "DELIVERY_START_RESULT") {
    return false;
  }

  String lineStr;
  String status;
  String motorBoardStatus;
  String reason;
  if (!extractTokenValue(tokens[1], "line", lineStr) ||
      !extractTokenValue(tokens[2], "status", status) ||
      !extractTokenValue(tokens[3], "f103Status", motorBoardStatus) ||
      !extractTokenValue(tokens[4], "reason", reason)) {
    return false;
  }

  uint16_t deliveryLine = 0;
  if (!parseUInt16InRange(lineStr, MIN_VALID_DELIVERY_LINE,
                          MAX_VALID_DELIVERY_LINE, deliveryLine)) {
    return false;
  }

  if (status != "started" && status != "failed") {
    return false;
  }
  if (motorBoardStatus != "idle" && motorBoardStatus != "busy" &&
      motorBoardStatus != "success" && motorBoardStatus != "failed" &&
      motorBoardStatus != "unavailable") {
    return false;
  }
  if (reason.length() == 0 || reason.indexOf(',') >= 0) {
    return false;
  }
  if (status == "started" && motorBoardStatus != "busy" &&
      motorBoardStatus != "success" && motorBoardStatus != "failed") {
    return false;
  }

  outData.deliveryLine = deliveryLine;
  String opStr;
  if (!extractTokenValue(tokens[5], "op", opStr) ||
      !parseUInt32Strict(opStr, outData.operationId) || outData.operationId == 0) return false;
  outData.started = status == "started";
  outData.motorBoardStatus = motorBoardStatus;
  outData.reason = reason;
  return true;
}

bool parseDeliveryLineCheckResultLine(const String& line, DeliveryLineCheckResultData& outData) {
  String tokens[3];
  size_t tokenCount = 0;
  if (!splitByComma(line, tokens, 3, tokenCount)) {
    return false;
  }

  if (tokenCount != 3 || tokens[0] != "DELIVERY_LINE_CHECK_RESULT") {
    return false;
  }

  String lineStr;
  String status;
  if (!extractTokenValue(tokens[1], "line", lineStr) ||
      !extractTokenValue(tokens[2], "status", status)) {
    return false;
  }

  uint16_t deliveryLine = 0;
  if (!parseUInt16InRange(lineStr, MIN_VALID_DELIVERY_LINE, MAX_VALID_DELIVERY_LINE, deliveryLine)) {
    return false;
  }

  if (status != "ready" && status != "invalidDeliveryLine") {
    return false;
  }

  outData.deliveryLine = deliveryLine;
  outData.status = status;
  return true;
}

bool parseControllerDiagnosticLine(const String& line,
                                   ControllerDiagnosticData& outData) {
  String tokens[6];
  size_t tokenCount = 0;
  if (!splitByComma(line, tokens, 6, tokenCount) || tokenCount != 6 ||
      tokens[0] != "CONTROLLER_DIAGNOSTIC") {
    return false;
  }

  String lineStr;
  String countStr;
  if (!extractTokenValue(tokens[1], "source", outData.source) ||
      !extractTokenValue(tokens[2], "event", outData.event) ||
      !extractTokenValue(tokens[3], "line", lineStr) ||
      !extractTokenValue(tokens[4], "count", countStr) ||
      !extractTokenValue(tokens[5], "status", outData.status)) {
    return false;
  }

  if (!parseUInt16InRange(lineStr, MIN_VALID_DELIVERY_LINE,
                          MAX_VALID_DELIVERY_LINE, outData.deliveryLine) ||
      !parseUInt32Strict(countStr, outData.count) ||
      outData.source.length() == 0 || outData.event.length() == 0 ||
      outData.status.length() == 0 || outData.status.indexOf(',') >= 0) {
    return false;
  }
  return true;
}

String buildUsbPaymentReplyForTransaction(PaymentReplyType reply, uint32_t tx, uint32_t boot) {
  const char* status = reply == PaymentReplyType::OK ? "PAYMENT_OK" :
      reply == PaymentReplyType::FAIL ? "PAYMENT_FAIL" : "PAYMENT_TIMEOUT";
  return String(status) + ",tx=" + String(tx) + ",boot=" + String(boot);
}

String buildUsbPaymentReplyLine(PaymentReplyType reply) {
  return buildUsbPaymentReplyForTransaction(reply, activePayment.transactionId, activePayment.bootIdentity);
}

String serializeActivePayment() {
  return "PAYMENT_REQUEST,id=" + String(activePayment.productPositionId) +
      ",price=" + String(activePayment.priceMinor) + ",tx=" + String(activePayment.transactionId) +
      ",line=" + String(activePayment.deliveryLine) + ",boot=" + String(activePayment.bootIdentity);
}

bool persistPaymentFlow(const char* phase) {
  if (!paymentJournalReady || !activePayment.active) return false;
  String record = String(phase) + "|" + serializeActivePayment();
  if (record == paymentJournalRecord) return true;
  if (paymentPreferences.putString("flow", record) != record.length()) return false;
  paymentJournalRecord = record;
  return true;
}

bool sendPaymentDecision(PaymentReplyType reply) {
  if (reply != PaymentReplyType::OK) return sendLineToStm(buildUsbPaymentReplyLine(reply));
  // Persist the paid decision before UART; restart retries the same boot/tx only.
  if (!persistPaymentFlow("D")) return false;
  pendingPaymentDecision = buildUsbPaymentReplyLine(reply);
  paymentDecisionRetryMs = millis() + 500U;
  activePayment.paymentReplyForwardedToStm = true;
  changeState(AppState::WAITING_STM_DELIVERY_RESULT);
  (void)sendLineToStm(pendingPaymentDecision);
  return true; // Durable admission; UART errors retry without a second decision.
}

void servicePaymentDecision() {
  if (pendingPaymentDecision.length() && (int32_t)(millis()-paymentDecisionRetryMs)>=0) {
    sendLineToStm(pendingPaymentDecision);
    paymentDecisionRetryMs = millis() + 500U;
  }
}

void restorePaymentFlow() {
  paymentJournalReady = paymentPreferences.begin("payment", false);
  if (!paymentJournalReady) return;
  paymentJournalRecord = paymentPreferences.getString("flow", "");
  PaymentRequestData request;
  if (paymentJournalRecord.length()<3 || !parseUsbPaymentRequestLine(paymentJournalRecord.substring(2), request)) return;
  VendReportData final;
  bool completed = durableVendRecord.length() && parseUsbDeliveryResultLine(durableVendRecord.substring(2), final) &&
      (request.bootIdentity < final.bootIdentity || (request.bootIdentity == final.bootIdentity && request.transactionId <= final.transactionId));
  if (completed) {
    clearActivePayment();
    return;
  }
  setActivePayment(request);
  activePaymentRequestSentToWs = true; // Never trigger a second charge by replaying the WS request after reboot.
  if (paymentJournalRecord.startsWith("D|") || paymentJournalRecord.startsWith("A|")) {
    activePayment.paymentReplyForwardedToStm = true;
    changeState(AppState::WAITING_STM_DELIVERY_RESULT);
    if (paymentJournalRecord.startsWith("D|")) pendingPaymentDecision=buildUsbPaymentReplyLine(PaymentReplyType::OK);
  } else changeState(AppState::WAITING_WS_PAYMENT_REPLY);
}

void handleBackendOfflineDuringActiveFlow(const String& reason) {
#if !APP_TEST_MODE
  bool activeFlow = (appState == AppState::WAITING_WS_PAYMENT_REPLY && activePayment.active) ||
                    (appState == AppState::WAITING_STM_DELIVERY_RESULT && activePayment.active) ||
                    (appState == AppState::WAITING_STM_DIRECT_DELIVERY_RESULT && pendingDirectDelivery.active) ||
                    (appState == AppState::WAITING_STM_LINE_CHECK_RESULT && pendingLineCheck.active) ||
                    pendingVendReportSend.active ||
                    pendingDirectDeliveryResponse.active ||
                    pendingOrderCancellationRequest.active;
  if (!activeFlow) {
    return;
  }

  uint32_t nowMs = millis();
  if ((nowMs - lastOfflineFlowLogMs) < OFFLINE_FLOW_LOG_INTERVAL_MS) {
    return;
  }

  lastOfflineFlowLogMs = nowMs;
  logMessage("[FLOW] " + reason + ", транзакцию не роняем, ждём reconnect/timeout.");
#endif
}

void failActivePaymentBecausePaymentMessageInvalid(const String& reason) {
#if !APP_TEST_MODE
  if (appState != AppState::WAITING_WS_PAYMENT_REPLY || !activePayment.active) {
	queueLocalVendFlowDiagnostic("terminalIgnored", 0U, 0U, 0U,
	                             "invalidMessageNoActivePayment");
	logMessage("[FLOW] " + reason + ", но активной оплаты нет.");
	sendTextToWebSocket(makeRequestStatusJson("error"));
	return;
  }

	queueLocalVendFlowDiagnostic("terminalRejected",
	                             activePayment.productPositionId,
	                             activePayment.deliveryLine,
	                             activePayment.transactionId,
	                             "invalidPaymentMessage");
  logMessage("[FLOW] " + reason + ", отправляем PAYMENT_FAIL в STM32 и requestStatus=error.");
  sendLineToStm(buildUsbPaymentReplyLine(PaymentReplyType::FAIL));
  sendTextToWebSocket(makeRequestStatusJson("error"));
  clearActivePayment();
  changeState(AppState::IDLE);
#endif
}

String buildUsbConfigurationCommandLine(const ConfigCommandData& cmd) {
  String line = "SET_MAP,motor=";
  line += String(cmd.deliveryLine);
  line += ",id=";
  line += String(cmd.productPositionId);
  return line;
}

String buildUsbDirectDeliveryCommandLine(const DirectDeliveryRequestData& request) {
  String line = "DELIVERY_REQUEST,line=";
  line += String(request.deliveryLine);
  line += ",op=";
  line += String(pendingDirectDelivery.operationId);
  return line;
}

String buildUsbDeliveryLineCheckCommandLine(const DeliveryLineCheckRequestData& request) {
  String line = "DELIVERY_LINE_CHECK,line=";
  line += String(request.deliveryLine);
  return line;
}

String makePaymentRequestJson(const PaymentRequestData& data) {
  DynamicJsonDocument doc(256);
  doc["type"] = "paymentRequest";
  doc["productPositionId"] = data.productPositionId;
  doc["productPrice"] = data.productPrice;
  doc["deliveryLine"] = data.deliveryLine;
  doc["controllerId"] = CONTROLLER_ID;

  String json;
  serializeJson(doc, json);
  return json;
}

String makeDeliveryResultJson(const VendReportData& data) {
  DynamicJsonDocument doc(256);
  doc["type"] = "productDeliveryResponse";
  doc["coffeeMachineStatus"] = data.coffeeMachineStatus;
  doc["snackMachineStatus"] = data.snackMachineStatus;

  String json;
  serializeJson(doc, json);
  return json;
}

String makeRequestStatusJson(const String& status) {
  DynamicJsonDocument doc(128);
  doc["type"] = "requestStatus";
  doc["status"] = status;

  String json;
  serializeJson(doc, json);
  return json;
}

String makeDirectDeliveryRequestStatusJson(const String& status,
                                           uint16_t deliveryLine,
                                           const String& motorCommandStatus,
                                           const String& motorBoardStatus,
                                           const String& reason,
                                           uint32_t operationId,
                                           bool recoveredAfterRestart) {
  DynamicJsonDocument doc(320);
  doc["type"] = "requestStatus";
  doc["status"] = status;
  doc["deliveryLine"] = deliveryLine;
  doc["motorCommandStatus"] = motorCommandStatus;
  doc["motorBoardStatus"] = motorBoardStatus;
  doc["reason"] = reason;
  if (operationId != 0) {
    doc["operationId"] = operationId;
  }
  doc["recoveredAfterRestart"] = recoveredAfterRestart;

  String json;
  serializeJson(doc, json);
  return json;
}

String makeOrderCancellationRequestJson() {
  DynamicJsonDocument doc(64);
  doc["type"] = "orderCancellationRequest";

  String json;
  serializeJson(doc, json);
  return json;
}

String makeDeliveryLineCheckResponseJson(uint16_t deliveryLine, const String& status) {
  DynamicJsonDocument doc(160);
  doc["type"] = "deliveryLineCheckResponse";
  doc["status"] = status;
  doc["deliveryLine"] = deliveryLine;

  String json;
  serializeJson(doc, json);
  return json;
}

String makeControllerDiagnosticJson(const ControllerDiagnosticData& data) {
  DynamicJsonDocument doc(192);
  doc["type"] = "cd";
  doc["s"] = data.source;
  doc["e"] = data.event;
  if (data.deliveryLine != 0) {
    doc["l"] = data.deliveryLine;
  }
  if (data.count != 0) {
    doc["n"] = data.count;
  }
  doc["v"] = data.status;

  String json;
  serializeJson(doc, json);
  return json;
}

bool isUrgentControllerDiagnostic(const ControllerDiagnosticData& data) {
  if (data.source == "mdb" || data.source == "mdbManager") return false;
  if (data.source == "eventQueue" || data.source == "uartTx" ||
      data.source == "mdbCommandQueue" || data.status == "fallback" ||
      data.status == "dropped") {
    return true;
  }

  if (data.source == "f411") {
    return data.event != "health";
  }

  if (data.source == "f103") {
    if (data.event == "startFail" || data.event == "vendStartFail") {
      return true;
    }
    const bool routineEvent =
        data.event == "lineCheck" || data.event == "startOk" ||
        data.event == "vendStartOk" || data.event == "resultOk" ||
        data.event == "resultFail" || data.event == "sensorSuccess" ||
        data.event == "motorFailed";
    const bool i2cHealthy = data.status.indexOf("-e0-x0000-") >= 0;
    return !routineEvent || !i2cHealthy;
  }

  return true;
}

void queueControllerDiagnostic(const ControllerDiagnosticData& data,
                               bool urgent) {
  if (controllerDiagnosticPending && pendingControllerDiagnosticUrgent &&
      !urgent) {
    return;
  }
  pendingControllerDiagnostic = data;
  controllerDiagnosticPending = true;
  pendingControllerDiagnosticUrgent = urgent;
}

void queueVendFlowDiagnostic(const ControllerDiagnosticData& data) {
  if (vendFlowDiagnosticCount >= VEND_FLOW_DIAGNOSTIC_QUEUE_CAPACITY) {
    ++vendFlowDiagnosticDropCount;
    return;
  }
  vendFlowDiagnosticQueue[vendFlowDiagnosticTail] = data;
  vendFlowDiagnosticTail = static_cast<uint8_t>(
      (vendFlowDiagnosticTail + 1U) % VEND_FLOW_DIAGNOSTIC_QUEUE_CAPACITY);
  ++vendFlowDiagnosticCount;
}

void queueLocalVendFlowDiagnostic(const String& event, uint16_t productPositionId,
                                  uint16_t deliveryLine, uint32_t transactionId,
                                  const String& reason) {
  controllerLink.record(event.c_str(),reason.c_str(),millis(),transactionId,activePayment.bootIdentity);
  diagnosticRecord(DiagnosticEvent::Flow,event.c_str(),reason.c_str(),0,true,("reason="+reason+",id="+String(productPositionId)+",line="+String(deliveryLine)+",tx="+String(transactionId)).c_str());
  ControllerDiagnosticData diagnostic;
  diagnostic.source = "vendFlow";
  diagnostic.event = event;
  diagnostic.deliveryLine = deliveryLine;
  diagnostic.count = productPositionId;
  diagnostic.status = "r" + reason + "-t" + String(transactionId);
  queueVendFlowDiagnostic(diagnostic);
}

String makeDirectDeliveryResponseJson(const DirectDeliveryResultData& data) {
  DynamicJsonDocument doc(160);
  doc["type"] = "productDeliveryResponse";
  doc["coffeeMachineStatus"] = "skipped";
  doc["snackMachineStatus"] = data.snackMachineStatus;

  String json;
  serializeJson(doc, json);
  return json;
}

String makeDirectDeliveryResponseJson(const String& snackMachineStatus) {
  DirectDeliveryResultData data;
  data.deliveryLine = 0;
  data.statusRaw = snackMachineStatus;
  data.snackMachineStatus = snackMachineStatus;
  data.operationId = 0;
  data.recoveredAfterRestart = false;
  return makeDirectDeliveryResponseJson(data);
}

String makeDeliverySuccessJson() {
  DynamicJsonDocument doc(256);
  doc["type"] = "productDeliveryResponse";
  doc["coffeeMachineStatus"] = "success";
  doc["snackMachineStatus"] = "success";

  String json;
  serializeJson(doc, json);
  return json;
}

String makeConfigurationStatusJson(const String& configurationStatus,
                                   const String& storageStatus = "unknown") {
  DynamicJsonDocument doc(128);
  doc["type"] = "setProductPositionResponse";
  doc["configurationStatus"] = configurationStatus;
  doc["storageStatus"] = storageStatus;
  doc["controllerId"] = CONTROLLER_ID;

  String json;
  serializeJson(doc, json);
  return json;
}

void disconnectWebSocketClient(uint8_t clientId) {
  const DiagnosticPhase previousPhase = currentDiagnosticPhase;
  markDiagnosticPhase(DiagnosticPhase::WS_CLIENT_DISCONNECT);
  const uint32_t startedMs = millis();
  webSocket.disconnect(clientId);
  finishTimedLibraryCall(wsClientDisconnectTiming,
                         DiagnosticPhase::WS_CLIENT_DISCONNECT,
                         "webSocket.disconnect", startedMs);
  markDiagnosticPhase(previousPhase);
}

bool sendTextToWebSocket(String text) {
  const HttpsCommandHandle handle =
      httpsTransport.enqueue(text, HttpsCommandKind::Transient,
                             STATE_TIMEOUT_WS_SEND_MS);
  const bool ok = handle.valid();
  logMessage(String(ok ? "[HTTPS QUEUE] " : "[HTTPS QUEUE] FAIL: ") +
             "bytes=" + String(text.length()) + " payload=" + text);
  return ok;
}

void pumpGatewayTelemetry() {
  if (!canSendWebSocketNow()) {
    return;
  }

  const uint32_t nowMs = millis();
  if ((nowMs - lastDeviceStatusMs) >= DEVICE_STATUS_INTERVAL_MS) {
    gatewayStatusPending = true;
  }

  if (gatewayHelloPending) {
    if (sendTextToWebSocket(makeGatewayHelloJson())) {
      gatewayHelloPending = false;
    }
    return;
  }

  if (gatewayStatusPending) {
    if (sendTextToWebSocket(makeGatewayStatusJson())) {
      gatewayStatusPending = false;
      lastDeviceStatusMs = nowMs;
    }
    return;
  }

  /* Flow diagnostics wait until the business FSM is idle. They must never
     precede or delay payment/final delivery messages. */
  if (appState == AppState::IDLE && vendFlowDiagnosticCount != 0U) {
    ControllerDiagnosticData& diagnostic =
        vendFlowDiagnosticQueue[vendFlowDiagnosticHead];
    if (sendTextToWebSocket(makeControllerDiagnosticJson(diagnostic))) {
      vendFlowDiagnosticHead = static_cast<uint8_t>(
          (vendFlowDiagnosticHead + 1U) % VEND_FLOW_DIAGNOSTIC_QUEUE_CAPACITY);
      --vendFlowDiagnosticCount;
    }
    return;
  }

  if (appState == AppState::IDLE && vendFlowDiagnosticDropCount != 0U) {
    ControllerDiagnosticData overflow;
    overflow.source = "vendFlow";
    overflow.event = "logOverflow";
    overflow.deliveryLine = 0;
    overflow.count = vendFlowDiagnosticDropCount;
    overflow.status = "rdiagnosticQueueFull";
    if (sendTextToWebSocket(makeControllerDiagnosticJson(overflow))) {
      vendFlowDiagnosticDropCount = 0U;
    }
    return;
  }

  if (controllerDiagnosticPending &&
      (pendingControllerDiagnosticUrgent ||
       (nowMs - lastControllerDiagnosticMs) >=
           CONTROLLER_DIAGNOSTIC_INTERVAL_MS)) {
    if (sendTextToWebSocket(
            makeControllerDiagnosticJson(pendingControllerDiagnostic))) {
      controllerDiagnosticPending = false;
      pendingControllerDiagnosticUrgent = false;
      lastControllerDiagnosticMs = nowMs;
    }
    return;
  }

  if (!gatewayEventReplayPending) {
    return;
  }

  const uint32_t firstSequence = getFirstStoredGatewayEventSequence();
  if (gatewayEventReplaySequence < firstSequence) {
    gatewayEventReplaySequence = firstSequence;
  }

  GatewayEventRecord event = {};
  if (!copyGatewayEvent(gatewayEventReplaySequence, event)) {
    gatewayEventReplayPending = false;
    return;
  }

  if (sendTextToWebSocket(makeGatewayEventJson(event))) {
    ++gatewayEventReplaySequence;
  }
}

bool canSendWebSocketNow() {
  return httpsTransport.available();
}

bool isDeadlineExpired(uint32_t deadlineMs) {
  return (int32_t)(millis() - deadlineMs) >= 0;
}

void queueVendReportForWebSocket(const VendReportData& data) {
  finalResultRemaining(false,data.bootIdentity,data.transactionId);
  pendingVendReportSend.active = true;
  pendingVendReportSend.data = data;
  pendingVendReportSend.deadlineMs = millis() + STATE_TIMEOUT_WS_SEND_MS;
  pendingVendTransportHandle = {};
  logMessage("[FLOW] VEND_REPORT сохранён, ждём WS reconnect для отправки.");
}

void queueDirectDeliveryResponseForWebSocket(const DirectDeliveryResultData& data) {
  finalResultRemaining(true,0,data.operationId);
  pendingDirectDeliveryResponse.active = true;
  pendingDirectDeliveryResponse.data = data;
  pendingDirectDeliveryResponse.deadlineMs = millis() + STATE_TIMEOUT_WS_SEND_MS;
  pendingDirectResponseTransportHandle = {};
  directResponseRetryMs = 0;
  logMessage("[FLOW] DELIVERY_RESULT сохранён, ждём WS reconnect для отправки.");
}

void queueOrderCancellationRequestForWebSocket() {
  if (pendingOrderCancellationRequest.active) return; // Coalesce; never extend TTL.
  httpsTransport.cancel(pendingCancellationTransportHandle);
  pendingOrderCancellationRequest.active = true;
  pendingOrderCancellationRequest.deadlineMs = millis() + STATE_TIMEOUT_WS_SEND_MS;
  pendingCancellationTransportHandle = {};
  pendingCancellationGeneration = operationGeneration;
  logMessage("[FLOW] orderCancellationRequest сохранён, ждём WS reconnect для отправки.");
}

String serializeVendReport(const VendReportData& data) {
  return "VEND_REPORT,id=" + String(data.productPositionId) +
      ",machine=" + data.machineStatusRaw + ",local=" + data.localStatusRaw +
      ",tx=" + String(data.transactionId) + ",boot=" + String(data.bootIdentity);
}

bool persistVendReport(const VendReportData& data, bool sent) {
  if (!vendJournalReady) return false;
  String record = String(sent ? "S|" : "P|") + serializeVendReport(data);
  if (durableVendRecord == record) return true;
  if ((int32_t)(millis() - vendJournalRetryMs) < 0) return false;
  if (vendPreferences.putString("result", record) != record.length()) {
    vendJournalRetryMs = millis() + 500U;
    return false;
  }
  durableVendRecord = record;
  return true;
}

void acknowledgeVendReport(const VendReportData& data) {
  sendLineToStm("VEND_ACK,tx=" + String(data.transactionId) + ",boot=" + String(data.bootIdentity));
}

void restorePendingVendReport() {
  vendJournalReady = vendPreferences.begin("vend", false);
  if (!vendJournalReady) return;
  durableVendRecord = vendPreferences.getString("result", "");
  VendReportData data;
  if (durableVendRecord.startsWith("P|") &&
      parseUsbDeliveryResultLine(durableVendRecord.substring(2), data)) queueVendReportForWebSocket(data);
}

bool sendOrQueueVendReport(const VendReportData& data) {
  if(finalResultTerminal(false,data.bootIdentity,data.transactionId)){acknowledgeVendReport(data);return true;}
  VendReportData previous;
  if (durableVendRecord.length() && parseUsbDeliveryResultLine(durableVendRecord.substring(2), previous)) {
    bool older = data.bootIdentity < previous.bootIdentity ||
        (data.bootIdentity == previous.bootIdentity && data.transactionId < previous.transactionId);
    bool same = data.bootIdentity == previous.bootIdentity && data.transactionId == previous.transactionId;
    if (older || (same && (durableVendRecord.startsWith("S|") || durableVendRecord.startsWith("X|")))) {
      acknowledgeVendReport(data);
      return true;
    }
  }
  if (pendingVendReportSend.active && (pendingVendReportSend.data.bootIdentity != data.bootIdentity ||
      pendingVendReportSend.data.transactionId != data.transactionId)) return false;
  bool alreadyPending = pendingVendReportSend.active;
  if (!alreadyPending) {
    queueVendReportForWebSocket(data);
    vendReportTransportSent = false;
  }
  // F411 may release its copy after the pending result is durable on ESP.
  // Enqueue is not a remote ACK and must never advance P| to S|.
  if (persistVendReport(data, false)) acknowledgeVendReport(data);
  return true;
}


bool sendOrQueueDirectDeliveryResponse(const DirectDeliveryResultData& data) {
  queueDirectDeliveryResponseForWebSocket(data);
  if(finalResultRemaining(true,0,data.operationId)==0)return false;
  if (!canSendWebSocketNow()) return false;
  pendingDirectResponseTransportHandle = httpsTransport.enqueue(
      makeDirectDeliveryResponseJson(data), HttpsCommandKind::DurableFinal, 0);
  return pendingDirectResponseTransportHandle.valid();
}

bool sendOrQueueOrderCancellationRequest() {
  queueOrderCancellationRequestForWebSocket();
  if (pendingCancellationTransportHandle.valid()) return true;
  if (!canSendWebSocketNow()) return false;
  pendingCancellationTransportHandle = httpsTransport.enqueue(
      makeOrderCancellationRequestJson(), HttpsCommandKind::Cancellation,
      STATE_TIMEOUT_WS_SEND_MS);
  return pendingCancellationTransportHandle.valid();
}

void sendOrQueueDirectDeliveryResponse(const String& snackMachineStatus) {
  DirectDeliveryResultData data;
  data.deliveryLine = 0;
  data.statusRaw = snackMachineStatus;
  data.snackMachineStatus = snackMachineStatus;
  data.operationId = 0;
  data.recoveredAfterRestart = false;
  sendOrQueueDirectDeliveryResponse(data);
}

bool finalizeConfirmedDirectDeliveryResponse() {
  if (!pendingDirectDeliveryResponse.active) return true;
  if (!clearConfirmedDirectDeliveryJournal(
          pendingDirectDeliveryResponse.data.operationId)) {
    return false;
  }
  finalResultComplete(true,0,pendingDirectDeliveryResponse.data.operationId,true);
  clearPendingDirectDeliveryResponse();
  logMessage("[FLOW] Отложенный DELIVERY_RESULT отправлен.");
  return true;
}

void pumpPendingWebSocketWork() {
  servicePaymentDecision();
  if (appState == AppState::WAITING_WS_PAYMENT_REPLY &&
      activePayment.active &&
      !activePayment.cancelledByMachine &&
      !activePaymentRequestSentToWs &&
      !pendingOrderCancellationRequest.active &&
      (uint32_t)(millis() - stateEnterMs) < STATE_TIMEOUT_PAYMENT_MS &&
      canSendWebSocketNow()) {
    PaymentRequestData request;
    request.productPositionId = activePayment.productPositionId;
    request.priceMinor = activePayment.priceMinor;
    request.productPrice = activePayment.productPrice;
    request.deliveryLine = activePayment.deliveryLine;

    activePaymentRequestHandle = httpsTransport.enqueue(
        makePaymentRequestJson(request), HttpsCommandKind::Payment,
        STATE_TIMEOUT_PAYMENT_MS - (millis() - stateEnterMs));
    if (activePaymentRequestHandle.valid()) {
      activePaymentRequestSentToWs = true;
      logMessage("[FLOW] Отложенный PAYMENT_REQUEST отправлен WS-клиенту.");
    }
  }

  if (pendingOrderCancellationRequest.active) {
    if (isDeadlineExpired(pendingOrderCancellationRequest.deadlineMs)) {
      logMessage("[TIMEOUT] Не удалось отправить orderCancellationRequest в WS за 60 сек.");
      clearPendingOrderCancellationRequest();
    } else if (pendingCancellationTransportHandle.valid() &&
               httpsTransport.stateFor(
                   pendingCancellationTransportHandle) ==
                   HttpsCommandState::Confirmed) {
      clearPendingOrderCancellationRequest();
      logMessage("[FLOW] orderCancellationRequest подтверждён backend.");
    } else if (!pendingCancellationTransportHandle.valid() &&
               canSendWebSocketNow()) {
      const uint32_t remainingMs =
          pendingOrderCancellationRequest.deadlineMs - millis();
      pendingCancellationTransportHandle = httpsTransport.enqueue(
          makeOrderCancellationRequestJson(), HttpsCommandKind::Cancellation,
          remainingMs);
    }
  }

  if (pendingVendReportSend.active && finalResultRemaining(false,pendingVendReportSend.data.bootIdentity,pendingVendReportSend.data.transactionId)==0) {
    const VendReportData expired=pendingVendReportSend.data;
    const bool received=httpsTransport.stateFor(pendingVendTransportHandle)==HttpsCommandState::Confirmed;
    finalResultComplete(false,expired.bootIdentity,expired.transactionId,received);
    const String tombstone=String(received?"S|":"X|")+serializeVendReport(expired);
    if(vendJournalReady)vendPreferences.putString("result",tombstone);
    durableVendRecord=tombstone;
    diagnosticRecord(DiagnosticEvent::Flow,received?"vendReportConfirmed":"vendReportExpired",received?"Result confirmed; local cleanup deadline":"Result abandoned after 70 seconds",0,!received);
    acknowledgeVendReport(expired);
    clearPendingVendReportSend();vendReportTransportSent=false;
  }
  if (pendingVendReportSend.active) {
    // ACK only after an atomic NVS record exists. F411 retains/retries until ACK.
    bool durable = persistVendReport(pendingVendReportSend.data, false);
    if (!pendingVendTransportHandle.valid() && durable &&
        canSendWebSocketNow() &&
        (int32_t)(millis() - vendReportSendRetryMs) >= 0) {
      pendingVendTransportHandle = httpsTransport.enqueue(
          makeDeliveryResultJson(pendingVendReportSend.data),
          HttpsCommandKind::DurableFinal, 0);
      vendReportTransportSent =
          pendingVendTransportHandle.valid();
      vendReportSendRetryMs = millis() + 500U;
    }
    const HttpsCommandState state =
        httpsTransport.stateFor(pendingVendTransportHandle);
    const bool confirmed = state == HttpsCommandState::Confirmed;
    if (confirmed && persistVendReport(pendingVendReportSend.data, true)) {
      finalResultComplete(false,pendingVendReportSend.data.bootIdentity,pendingVendReportSend.data.transactionId,true);
      acknowledgeVendReport(pendingVendReportSend.data);
      clearPendingVendReportSend();
      vendReportTransportSent = false;
    } else if (state == HttpsCommandState::Failed) {
      httpsTransport.release(pendingVendTransportHandle);
      pendingVendTransportHandle = {};
      vendReportTransportSent = false;
      vendReportSendRetryMs = millis() + FINAL_RESULT_RETRY_MS;
    }
  }

  if (pendingDirectDeliveryResponse.active && finalResultRemaining(true,0,pendingDirectDeliveryResponse.data.operationId)==0) {
    const bool received=httpsTransport.stateFor(pendingDirectResponseTransportHandle)==HttpsCommandState::Confirmed;
    finalResultComplete(true,0,pendingDirectDeliveryResponse.data.operationId,received);
    diagnosticRecord(DiagnosticEvent::Flow,received?"directReportConfirmed":"directReportExpired",received?"Result confirmed; local cleanup deadline":"Result abandoned after 70 seconds",0,!received);
    clearConfirmedDirectDeliveryJournal(pendingDirectDeliveryResponse.data.operationId);
    clearPendingDirectDeliveryResponse();
  }
  if (pendingDirectDeliveryResponse.active) {
    const HttpsCommandState state =
        httpsTransport.stateFor(
            pendingDirectResponseTransportHandle);
    if (state == HttpsCommandState::Confirmed) {
      if (!finalizeConfirmedDirectDeliveryResponse()) return;
    } else if (state == HttpsCommandState::Failed) {
      httpsTransport.release(pendingDirectResponseTransportHandle);
      pendingDirectResponseTransportHandle = {};
      directResponseRetryMs = millis() + FINAL_RESULT_RETRY_MS;
    } else if (!pendingDirectResponseTransportHandle.valid() &&
               canSendWebSocketNow() &&
               (directResponseRetryMs == 0 ||
                static_cast<int32_t>(millis() - directResponseRetryMs) >= 0)) {
      pendingDirectResponseTransportHandle = httpsTransport.enqueue(
          makeDirectDeliveryResponseJson(pendingDirectDeliveryResponse.data),
          HttpsCommandKind::DurableFinal, 0);
    }
  }
}

#include "UniverNetwork.h"

void connectToWiFiBlocking() {
  logMessage("[WIFI] Подключение к SSID: " + String(WIFI_SSID));

  WiFi.onEvent(onWiFiEvent);
  DiagnosticPhase previousPhase = currentDiagnosticPhase;
  markDiagnosticPhase(DiagnosticPhase::WIFI_MODE_STA);
  uint32_t startedMs = millis();
  WiFi.mode(WIFI_STA);
  finishTimedLibraryCall(wifiModeStaTiming, DiagnosticPhase::WIFI_MODE_STA,
                         "WiFi.mode(STA)", startedMs);
  markDiagnosticPhase(previousPhase);
  // Arduino Wi-Fi owns all ordinary reconnect attempts. Application code only
  // escalates to one full driver restart after a prolonged outage.
  WiFi.setAutoReconnect(true);
  markDiagnosticPhase(DiagnosticPhase::WIFI_CONFIG);
  startedMs = millis();
  const bool configOk =
      WiFi.config(LOCAL_IP, LOCAL_GATEWAY, LOCAL_SUBNET, LOCAL_DNS);
  finishTimedLibraryCall(wifiConfigTiming, DiagnosticPhase::WIFI_CONFIG,
                         "WiFi.config", startedMs);
  markDiagnosticPhase(previousPhase);
  if (!configOk) {
    logMessage("[WIFI] Static IP config failed, продолжаем DHCP.");
  }
  markDiagnosticPhase(DiagnosticPhase::WIFI_BEGIN);
  startedMs = millis();
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  finishTimedLibraryCall(wifiBeginTiming, DiagnosticPhase::WIFI_BEGIN,
                         "WiFi.begin(credentials)", startedMs);
  markDiagnosticPhase(previousPhase);
  wifiReady = (WiFi.status() == WL_CONNECTED);
  sampleRadioDiagnostics(true);
  if (wifiReady) {
    logMessage("[WIFI] Подключено.");
    printNetworkDiagnostics();
  } else {
    wifiOfflineSinceMs = millis();
    logMessage("[WIFI] Старт без ожидания сети. STM bridge уже активен.");
  }
}

void stopWebSocketServer(const String& reason) {
  if (!wsServerStarted) {
    return;
  }

  wsServerStarted = false;
  wsReady = false;
  wsActiveClientId = 255;
  setLed(true);
  const DiagnosticPhase previousPhase = currentDiagnosticPhase;
  markDiagnosticPhase(DiagnosticPhase::WS_SERVER_CLOSE);
  const uint32_t startedMs = millis();
  webSocket.close();
  finishTimedLibraryCall(wsServerCloseTiming,
                         DiagnosticPhase::WS_SERVER_CLOSE,
                         "webSocket.close", startedMs);
  markDiagnosticPhase(previousPhase);
  ++wsServerRestartCount;
  recordGatewayEvent(GatewayEventType::WS_SERVER_STOPPED,
                     static_cast<int32_t>(wsServerRestartCount));
  logMessage("[WSS] Сервер остановлен: " + reason +
             ", restartCount=" + String(wsServerRestartCount));
}

void ensureWiFiConnected() {
  const uint32_t now = millis();
  if (WiFi.status() == WL_CONNECTED) {
    if (!wifiReady) {
      const uint32_t offlineMs =
          wifiOfflineSinceMs == 0 ? 0 : now - wifiOfflineSinceMs;
      logMessage("[WIFI] Соединение восстановлено afterMs=" +
                 String(offlineMs));
      printNetworkDiagnostics();
    }
    wifiReady = true;
    wifiOfflineSinceMs = 0;
    wifiDriverRestartedThisOutage = false;
    if (wifiRecoveryRestartCount != 0) {
      wifiRecoveryRestartCount = 0;
      if (healthPreferencesReady) {
        healthPreferences.putUInt(HEALTH_WIFI_RESTARTS_KEY, 0);
      }
    }
    return;
  }

  if (wifiOfflineSinceMs == 0) {
    wifiOfflineSinceMs = now;
    logMessage("[WIFI] Потеряно соединение. Статус=" +
               String(wifiStatusToString(WiFi.status())) +
               " disconnectReason=" + String(lastWifiDisconnectReason));
    stopWebSocketServer("Wi-Fi offline");
    handleBackendOfflineDuringActiveFlow("Wi-Fi offline во время оплаты");
  }
  wifiReady = false;
  setLed(true);

  const uint32_t offlineMs = now - wifiOfflineSinceMs;
  if (!wifiDriverRestartedThisOutage &&
      offlineMs >= WIFI_DRIVER_RESTART_AFTER_MS) {
    wifiDriverRestartedThisOutage = true;
    ++wifiDriverRestartCount;
    recordGatewayEvent(GatewayEventType::WIFI_DRIVER_RESTART,
                       static_cast<int32_t>(wifiDriverRestartCount));
    logMessage("[WIFI] 120 сек offline: однократный полный restart Wi-Fi driver.");
    DiagnosticPhase previousPhase = currentDiagnosticPhase;
    WiFi.setAutoReconnect(false);
    markDiagnosticPhase(DiagnosticPhase::WIFI_MODE_OFF);
    uint32_t startedMs = millis();
    WiFi.mode(WIFI_OFF);
    finishTimedLibraryCall(wifiModeOffTiming, DiagnosticPhase::WIFI_MODE_OFF,
                           "WiFi.mode(OFF)", startedMs);
    markDiagnosticPhase(previousPhase);
    delay(50);
    markDiagnosticPhase(DiagnosticPhase::WIFI_MODE_STA);
    startedMs = millis();
    WiFi.mode(WIFI_STA);
    finishTimedLibraryCall(wifiModeStaTiming, DiagnosticPhase::WIFI_MODE_STA,
                           "WiFi.mode(STA)", startedMs);
    markDiagnosticPhase(previousPhase);
    markDiagnosticPhase(DiagnosticPhase::WIFI_CONFIG);
    startedMs = millis();
    const bool configOk =
        WiFi.config(LOCAL_IP, LOCAL_GATEWAY, LOCAL_SUBNET, LOCAL_DNS);
    finishTimedLibraryCall(wifiConfigTiming, DiagnosticPhase::WIFI_CONFIG,
                           "WiFi.config", startedMs);
    markDiagnosticPhase(previousPhase);
    if (!configOk) {
      logMessage("[WIFI] Static IP config failed после driver restart.");
    }
    WiFi.setAutoReconnect(true);
    markDiagnosticPhase(DiagnosticPhase::WIFI_BEGIN);
    startedMs = millis();
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    finishTimedLibraryCall(wifiBeginTiming, DiagnosticPhase::WIFI_BEGIN,
                           "WiFi.begin(credentials)", startedMs);
    markDiagnosticPhase(previousPhase);
  }

  if (offlineMs >= WIFI_DEVICE_RESTART_AFTER_MS &&
      wifiRecoveryRestartCount == 0) {
    wifiRecoveryRestartCount = 1;
    if (healthPreferencesReady) {
      healthPreferences.putUInt(HEALTH_WIFI_RESTARTS_KEY,
                                wifiRecoveryRestartCount);
      healthPreferences.putUInt(HEALTH_PLANNED_REASON_KEY,
                                PLANNED_RESTART_WIFI_RECOVERY);
    }
    recordGatewayEvent(GatewayEventType::WIFI_DEVICE_RESTART,
                       static_cast<int32_t>(offlineMs));
    logMessage("[WIFI] 300 сек offline: один контролируемый restart ESP.");
    markDiagnosticPhase(DiagnosticPhase::DEVICE_RESTART);
    delay(50);
    ESP.restart();
  }
}

void setupWebSocketServer() {
  if (wsServerStarted) {
    return;
  }

  webSocket.onEvent([](uint8_t num, WStype_t type, uint8_t* payload, size_t length) {
    String text = payloadToString(payload, length);

    switch (type) {
      case WStype_ERROR:
        logMessage("[WSS] ERROR client=" + String(num) + ": " + text);
        if (num == wsActiveClientId) {
          handleBackendOfflineDuringActiveFlow("WebSocket error во время оплаты");
        }
        break;

      case WStype_DISCONNECTED:
        ++wsDisconnectCount;
        recordGatewayEvent(GatewayEventType::WS_CLIENT_DISCONNECTED, num);
        logMessage("[WSS] DISCONNECTED client=" + String(num) +
                   " active=" + String(wsActiveClientId) +
                   " clients=" + String(webSocket.connectedClients()) +
                   " uptimeMs=" + String(millis()) +
                   " heap=" + String(ESP.getFreeHeap()));
        if (num == wsActiveClientId) {
          wsReady = false;
          wsActiveClientId = 255;
          setLed(true);
          handleBackendOfflineDuringActiveFlow("WebSocket disconnected во время оплаты");
#if APP_TEST_MODE
          clearActivePayment();
          changeState(AppState::IDLE);
#endif
        }
        break;

      case WStype_CONNECTED: {
          ++wsConnectCount;
          recordGatewayEvent(GatewayEventType::WS_CLIENT_CONNECTED, num);
          uint8_t previousClientId = wsActiveClientId;
          wsActiveClientId = num;
          wsReady = true;
          setLed(false);
          logMessage("[WSS] CONNECTED client=" + String(num) +
                     " remote=" + webSocket.remoteIP(num).toString() +
                     " url=" + text +
                     " clients=" + String(webSocket.connectedClients()) +
                     " uptimeMs=" + String(millis()) +
                     " heap=" + String(ESP.getFreeHeap()) +
                     " rssi=" + String(wifiRssiLastDbm));
          if (previousClientId != 255 && previousClientId != num) {
            logMessage("[WSS] ACTIVE CLIENT REPLACED old=" + String(previousClientId) +
                       " new=" + String(num) + "; disconnecting old client");
            disconnectWebSocketClient(previousClientId);
          }
          logMessage("[LED] WS клиент подключён: D2 выключен.");
          gatewayHelloPending = true;
          gatewayStatusPending = true;
          gatewayEventReplaySequence = getFirstStoredGatewayEventSequence();
          gatewayEventReplayPending = true;
#if APP_TEST_MODE
          lastTestPaymentAttemptMs = millis() - TEST_PAYMENT_INTERVAL_MS;
#endif
        }
        break;

      case WStype_TEXT:
        if (num != wsActiveClientId) return;
        lastWsRxMs = millis();

        logMessage("[WS RX] client=" + String(num) +
                   " bytes=" + String(length) +
                   " state=" + String(stateToString(appState)) +
                   " uptimeMs=" + String(millis()) +
                   " heap=" + String(ESP.getFreeHeap()) +
                   " rssi=" + String(wifiRssiLastDbm) +
                   " payload=" + text);
        {
          DynamicJsonDocument doc(512);
          DeserializationError err = deserializeJson(doc, text);
          if (err) {
            logMessage("[WS RX] JSON parse error: " + String(err.c_str()));
            failActivePaymentBecausePaymentMessageInvalid("Некорректный JSON paymentResponse");
            return;
          }

          if (!doc["type"].is<const char*>()) {
            logMessage("[WS RX] type отсутствует или имеет неверный тип.");
            failActivePaymentBecausePaymentMessageInvalid("Некорректный WS message: нет type");
            return;
          }

          String messageType = doc["type"].as<String>();

          if (messageType == "diagnosticSnapshotRequest") {
            gatewayStatusPending = true;
            logMessage("[WS RX] Запрошен внеочередной gatewayStatus.");
            return;
          }

	          if (messageType == "paymentResponse") {
	            if (!doc["paymentStatus"].is<const char*>()) {
	              logMessage("[WS RX] paymentStatus отсутствует или имеет неверный тип.");
	              failActivePaymentBecausePaymentMessageInvalid("Некорректный paymentResponse: нет paymentStatus");
              return;
            }

            String paymentStatus = doc["paymentStatus"].as<String>();
            if (!isValidWsPaymentStatus(paymentStatus)) {
              logMessage("[WS RX] Некорректный paymentStatus: " + paymentStatus);
              failActivePaymentBecausePaymentMessageInvalid("Некорректный paymentStatus: " + paymentStatus);
	              return;
	            }

            if (appState == AppState::WAITING_STM_DELIVERY_RESULT && activePayment.active) {
              sendTextToWebSocket(makeRequestStatusJson("accepted"));
              logMessage("[FLOW] Повторный paymentResponse принят: PAYMENT_OK уже отправлен в STM32.");
              return;
            }

            if (pendingVendReportSend.active) {
              sendTextToWebSocket(makeRequestStatusJson("accepted"));
              logMessage("[FLOW] Повторный paymentResponse принят: итог продажи уже ждёт WS.");
              return;
            }

            if (appState != AppState::WAITING_WS_PAYMENT_REPLY || !activePayment.active) {
              queueLocalVendFlowDiagnostic("terminalIgnored", 0U, 0U, 0U,
                                           "paymentResponseWithoutRequest");
              sendTextToWebSocket(makeRequestStatusJson("error"));
              logMessage("[FLOW] paymentResponse без активного запроса отклонён.");
              return;
            }

#if APP_TEST_MODE
	            logMessage("[TEST] paymentStatus=" + paymentStatus);
            sendTextToWebSocket(makeRequestStatusJson("accepted"));
	            if (paymentStatus == "paid" && TEST_SEND_DELIVERY_SUCCESS_AFTER_PAID) {
	              sendTextToWebSocket(makeDeliverySuccessJson());
	              logMessage("[TEST] Автоматически отправлен delivery success.");
	            }
	            clearActivePayment();
	            changeState(AppState::IDLE);
#else
            if (activePayment.cancelledByMachine) {
              queueLocalVendFlowDiagnostic("terminalRejected",
                                           activePayment.productPositionId,
                                           activePayment.deliveryLine,
                                           activePayment.transactionId,
                                           "machineCancelled");
              bool sent = sendLineToStm(buildUsbPaymentReplyLine(PaymentReplyType::FAIL));
              sendTextToWebSocket(makeRequestStatusJson(sent ? "accepted" : "error"));
              if (!sent) {
                logMessage("[FLOW] Заказ отменён кофемашиной, но PAYMENT_FAIL не ушёл в STM32.");
                return;
              }
              clearActivePayment();
              changeState(AppState::IDLE);
              logMessage("[FLOW] Заказ отменён кофемашиной, paymentResponse закрыт через PAYMENT_FAIL.");
              return;
            }

            if (paymentStatus == "paid") {
              if (!sendPaymentDecision(PaymentReplyType::OK)) {
                queueLocalVendFlowDiagnostic("terminalRejected",
                                             activePayment.productPositionId,
                                             activePayment.deliveryLine,
                                             activePayment.transactionId,
                                             "paymentForwardFailed");
                sendTextToWebSocket(makeRequestStatusJson("error"));
                logMessage("[FLOW] Не удалось отправить PAYMENT_OK в STM32.");
                return;
              }
              activePayment.paymentReplyForwardedToStm = true;
              sendTextToWebSocket(makeRequestStatusJson("accepted"));
              changeState(AppState::WAITING_STM_DELIVERY_RESULT);
              logMessage("[FLOW] В STM32 отправлен PAYMENT_OK, ждём VEND_REPORT.");
            } else {
              if (!sendLineToStm(buildUsbPaymentReplyLine(PaymentReplyType::FAIL))) {
                queueLocalVendFlowDiagnostic("terminalRejected",
                                             activePayment.productPositionId,
                                             activePayment.deliveryLine,
                                             activePayment.transactionId,
                                             "denialForwardFailed");
                sendTextToWebSocket(makeRequestStatusJson("error"));
                logMessage("[FLOW] Не удалось отправить PAYMENT_FAIL в STM32.");
                return;
              }
              sendTextToWebSocket(makeRequestStatusJson("accepted"));
              clearActivePayment();
              changeState(AppState::IDLE);
              logMessage("[FLOW] В STM32 отправлен PAYMENT_FAIL, оплата завершена отказом.");
            }
	#endif
            return;
          }

	          if (messageType == "setProductPositionRequest") {
	            if (!isControllerAddressedToThisDevice(doc)) {
	              logMessage("[WS RX] setProductPositionRequest не для этого controllerId.");
	              return;
	            }
	
	            int idValue = 0;
	            int deliveryLineValue = 0;
	            if (!readJsonIntFieldAny(doc, "productPositionId", "productPositionid", "productPositionld", idValue) ||
	                !readJsonIntField(doc, "deliveryLine", deliveryLineValue)) {
	              logMessage("[WS RX] productPositionId/deliveryLine имеют неверный тип.");
	              return;
	            }
	
	            if (idValue < MIN_VALID_PRODUCT_POSITION_ID || idValue > MAX_VALID_PRODUCT_POSITION_ID ||
	                deliveryLineValue < MIN_VALID_DELIVERY_LINE || deliveryLineValue > MAX_VALID_DELIVERY_LINE) {
	              logMessage("[WS RX] Конфиг вне диапазона.");
	              return;
            }
	
	            ConfigCommandData cmd;
	            cmd.productPositionId = static_cast<uint16_t>(idValue);
	            cmd.deliveryLine = static_cast<uint16_t>(deliveryLineValue);
	
#if APP_TEST_MODE
	            setSelectionMotorMapping(cmd.productPositionId, static_cast<uint8_t>(cmd.deliveryLine));
	            logMessage("[TEST] Принято изменение спирали: id=" + String(cmd.productPositionId) +
	                       ", deliveryLine=" + String(cmd.deliveryLine));
	            sendTextToWebSocket(makeConfigurationStatusJson("success"));
#else
            if (appState != AppState::IDLE) {
              logMessage("[WS RX] Конфиг проигнорирован: устройство занято.");
              return;
            }

            if (!sendLineToStm(buildUsbConfigurationCommandLine(cmd))) {
              logMessage("[FLOW] Не удалось отправить SET_MAP в STM32.");
              return;
            }
            setPendingConfig(cmd);
            changeState(AppState::WAITING_STM_CONFIG_ACK);
            logMessage("[FLOW] SET_MAP отправлен в STM32, ждём SET_MAP_RESULT.");
#endif
	            return;
	          }

          if (messageType == "deliveryLineCheckRequest") {
            if (!isControllerAddressedToThisDevice(doc)) {
              logMessage("[WS RX] deliveryLineCheckRequest не для этого controllerId.");
              return;
            }

            int deliveryLineValue = 0;
            if (!readJsonIntField(doc, "deliveryLine", deliveryLineValue)) {
              logMessage("[WS RX] deliveryLineCheckRequest без deliveryLine.");
              sendTextToWebSocket(makeDeliveryLineCheckResponseJson(0, "invalidDeliveryLine"));
              return;
            }

            if (deliveryLineValue < MIN_VALID_DELIVERY_LINE ||
                deliveryLineValue > MAX_VALID_DELIVERY_LINE) {
              logMessage("[WS RX] deliveryLineCheckRequest вне диапазона.");
              sendTextToWebSocket(makeDeliveryLineCheckResponseJson(0, "invalidDeliveryLine"));
              return;
            }

            DeliveryLineCheckRequestData request;
            request.deliveryLine = static_cast<uint16_t>(deliveryLineValue);

#if APP_TEST_MODE
            String status = (request.deliveryLine >= 1 && request.deliveryLine <= 10)
                ? "ready" : "invalidDeliveryLine";
            sendTextToWebSocket(makeDeliveryLineCheckResponseJson(request.deliveryLine, status));
#else
            if (appState == AppState::WAITING_STM_LINE_CHECK_RESULT &&
                pendingLineCheck.active &&
                pendingLineCheck.deliveryLine == request.deliveryLine) {
              logMessage("[FLOW] Повторный deliveryLineCheckRequest: ждём ответ STM32.");
              return;
            }

            if (appState != AppState::IDLE) {
              logMessage("[WS RX] deliveryLineCheckRequest: устройство занято.");
              sendTextToWebSocket(makeDeliveryLineCheckResponseJson(
                  request.deliveryLine, "invalidDeliveryLine"));
              return;
            }

            if (!sendLineToStm(buildUsbDeliveryLineCheckCommandLine(request))) {
              logMessage("[FLOW] Не удалось отправить DELIVERY_LINE_CHECK в STM32.");
              sendTextToWebSocket(makeDeliveryLineCheckResponseJson(
                  request.deliveryLine, "invalidDeliveryLine"));
              return;
            }

            setPendingLineCheck(request, 0);
            changeState(AppState::WAITING_STM_LINE_CHECK_RESULT);
            logMessage("[FLOW] DELIVERY_LINE_CHECK отправлен в STM32.");
#endif
            return;
          }

	          if (messageType == "productDeliveryRequest") {
	            if (!isControllerAddressedToThisDevice(doc)) {
	              logMessage("[WS RX] productDeliveryRequest не для этого controllerId.");
	              return;
	            }

	            int idValue = 0;
	            int deliveryLineValue = 0;
	            if (!readJsonIntFieldAny(doc, "productPositionId", "productPositionid", "productPositionld", idValue) ||
	                !readJsonIntField(doc, "deliveryLine", deliveryLineValue)) {
	              logMessage("[WS RX] productPositionId/deliveryLine имеют неверный тип.");
              sendTextToWebSocket(makeRequestStatusJson("error"));
	              return;
	            }

	            if (idValue < MIN_VALID_PRODUCT_POSITION_ID || idValue > MAX_VALID_PRODUCT_POSITION_ID ||
	                deliveryLineValue < MIN_VALID_DELIVERY_LINE || deliveryLineValue > MAX_VALID_DELIVERY_LINE) {
	              logMessage("[WS RX] Запрос выдачи вне диапазона.");
              sendTextToWebSocket(makeRequestStatusJson("error"));
	              return;
	            }

	            DirectDeliveryRequestData request;
	            request.productPositionId = static_cast<uint16_t>(idValue);
	            request.deliveryLine = static_cast<uint16_t>(deliveryLineValue);

#if APP_TEST_MODE
	            logMessage("[TEST] Принята выдача товара: deliveryLine=" + String(request.deliveryLine));
            sendTextToWebSocket(makeDirectDeliveryRequestStatusJson(
                "accepted", request.deliveryLine, "started", "busy", "testMode",
                0, false));
	            sendOrQueueDirectDeliveryResponse(String("success"));
#else
            if (appState == AppState::WAITING_STM_DIRECT_DELIVERY_RESULT &&
                pendingDirectDelivery.active &&
                pendingDirectDelivery.productPositionId == request.productPositionId &&
                pendingDirectDelivery.deliveryLine == request.deliveryLine) {
              if (pendingDirectDelivery.recoveredAfterRestart) {
                pendingDirectDelivery.recoveryNoticePending = false;
                sendTextToWebSocket(makeDirectDeliveryRequestStatusJson(
                    "error", pendingDirectDelivery.deliveryLine,
                    "recoveredWaitingResult", "unknown", "espRestarted",
                    pendingDirectDelivery.operationId, true));
              } else if (pendingDirectDelivery.startStatusReceived) {
                sendTextToWebSocket(makeDirectDeliveryRequestStatusJson(
                    pendingDirectDelivery.startConfirmed ? "accepted" : "error",
                    pendingDirectDelivery.deliveryLine,
                    pendingDirectDelivery.startConfirmed ? "started" : "failed",
                    pendingDirectDelivery.motorBoardStatus,
                    pendingDirectDelivery.startReason,
                    pendingDirectDelivery.operationId, false));
              }
              logMessage("[FLOW] Повторный productDeliveryRequest: физическая команда не дублируется.");
              return;
            }

            if (pendingDirectDeliveryResponse.active &&
                pendingDirectDeliveryResponse.data.deliveryLine == request.deliveryLine) {
              sendTextToWebSocket(makeDirectDeliveryRequestStatusJson(
                  "accepted", request.deliveryLine, "resultPending", "unknown", "wsReconnect",
                  pendingDirectDeliveryResponse.data.operationId,
                  pendingDirectDeliveryResponse.data.recoveredAfterRestart));
              logMessage("[FLOW] Повторный productDeliveryRequest принят: итог выдачи уже ждёт WS.");
              return;
            }

	            if (appState != AppState::IDLE) {
	              logMessage("[WS RX] Выдача проигнорирована: устройство занято.");
              sendTextToWebSocket(makeRequestStatusJson("error"));
	              return;
	            }

              uint32_t operationId = createDirectDeliveryOperationId();
              if (!persistActiveDirectDelivery(request, operationId)) {
                sendTextToWebSocket(makeDirectDeliveryRequestStatusJson(
                    "error", request.deliveryLine, "failed", "unavailable",
                    "journalWriteFailed", operationId, false));
                return;
              }
	            setPendingDirectDelivery(request, operationId);
            changeState(AppState::WAITING_STM_DIRECT_DELIVERY_RESULT);
	            if (!sendLineToStm(buildUsbDirectDeliveryCommandLine(request))) {
	              logMessage("[FLOW] Не удалось отправить DELIVERY_REQUEST в STM32.");
              // The UART write may have reached F411. Retain ACTIVE and query only.
              pendingDirectDelivery.startTimeoutReported = true;
              sendTextToWebSocket(makeDirectDeliveryRequestStatusJson(
                  "error", request.deliveryLine, "failed", "unavailable", "uartWriteFailed",
                  operationId, false));
	              return;
	            }
	            logMessage("[FLOW] DELIVERY_REQUEST отправлен в STM32, ждём подтверждение F103 BUSY.");
#endif
	            return;
	          }
	
	          logMessage("[WS RX] Неизвестный type: " + messageType);
	        }
        break;

      case WStype_BIN:
        logMessage("[WSS] BIN client=" + String(num) + " length=" + String(length));
        break;

      case WStype_PING:
        logMessage("[WSS] PING client=" + String(num));
        break;

      case WStype_PONG:
        logMessage("[WSS] PONG client=" + String(num));
        break;

      case WStype_FRAGMENT_TEXT_START:
      case WStype_FRAGMENT_BIN_START:
      case WStype_FRAGMENT:
      case WStype_FRAGMENT_FIN:
        logMessage("[WSS] Fragment event client=" + String(num));
        break;
    }
  });

  const DiagnosticPhase previousPhase = currentDiagnosticPhase;
  markDiagnosticPhase(DiagnosticPhase::WS_SERVER_BEGIN);
  const uint32_t startedMs = millis();
  webSocket.begin();
  finishTimedLibraryCall(wsServerBeginTiming,
                         DiagnosticPhase::WS_SERVER_BEGIN,
                         "webSocket.begin", startedMs);
  markDiagnosticPhase(previousPhase);
  webSocket.enableHeartbeat(WS_HEARTBEAT_INTERVAL_MS,
                            WS_HEARTBEAT_PONG_TIMEOUT_MS,
                            WS_HEARTBEAT_DISCONNECT_COUNT);
  wsServerStarted = true;
  recordGatewayEvent(GatewayEventType::WS_SERVER_STARTED,
                     static_cast<int32_t>(wsServerRestartCount));
  logMessage("[WSS] Server started: " + makeWsUrlForLog());
  logMessage("[WSS] Heartbeat: intervalMs=" + String(WS_HEARTBEAT_INTERVAL_MS) +
             " pongTimeoutMs=" + String(WS_HEARTBEAT_PONG_TIMEOUT_MS) +
             " disconnectCount=" + String(WS_HEARTBEAT_DISCONNECT_COUNT));
  if (strlen(WS_SUBPROTOCOL) > 0 || strlen(WS_EXTRA_HEADERS) > 0) {
    logMessage("[WSS] Handshake headers принимает клиент, API payload совместим со старым клиентом.");
  }
}

bool completeHttpResponse(uint32_t token, uint16_t code, const String& body) {
  if (body.length() > HTTP_JSON_RESPONSE_LIMIT) return false;
  bool completed = false;
  portENTER_CRITICAL(&httpAdmissionMux);
  if (httpBridge.token == token && httpBridge.state == HttpBridgeState::Claimed) {
    httpBridge.responseCode = code;
    memcpy(httpBridge.responseBody, body.c_str(), body.length() + 1);
    httpBridge.state = HttpBridgeState::Complete;
    completed = true;
  }
  portEXIT_CRITICAL(&httpAdmissionMux);
  return completed;
}

void setHttpTimeoutBody(uint32_t token, const String& body) {
  if (body.length() > HTTP_JSON_RESPONSE_LIMIT) return;
  portENTER_CRITICAL(&httpAdmissionMux);
  if (httpBridge.token == token && httpBridge.state == HttpBridgeState::Claimed) {
    memcpy(httpBridge.timeoutBody, body.c_str(), body.length() + 1);
  }
  portEXIT_CRITICAL(&httpAdmissionMux);
}

bool readRequiredControllerId(const DynamicJsonDocument& doc) {
  int controllerId = -1;
  return readJsonIntField(doc, "controllerId", controllerId) &&
         (controllerId == 0 || controllerId == static_cast<int>(CONTROLLER_ID));
}

#include "UniverNetworkHttp.h"
#include "UniverReboot.h"

HttpAdmission admitTerminalJson(const String& route, const String& body,
                                uint32_t token, uint16_t& responseCode,
                                String& responseBody) {
  responseCode = 500;
  responseBody = "{}";
  if(route == "reboot") {responseCode=acceptUniverReboot()?200:500;return HttpAdmission::Complete;}
  if(univerRebootBlocksBusiness())return HttpAdmission::Complete;
  if(route == "network") { applyNetworkSettings(body,responseCode,responseBody); return HttpAdmission::Complete; }
  if (body.length() == 0 || body.length() > HTTP_JSON_BODY_LIMIT) {
    return HttpAdmission::Complete;
  }

  DynamicJsonDocument doc(512);
  if (deserializeJson(doc, body) || !doc["type"].is<const char*>()) {
    return HttpAdmission::Complete;
  }
  const String type = doc["type"].as<String>();

  if (route == "payment") {
    if (type != "paymentResponse" || !doc["paymentStatus"].is<const char*>()) {
      return HttpAdmission::Complete;
    }
    const String paymentStatus = doc["paymentStatus"].as<String>();
    if (!isValidWsPaymentStatus(paymentStatus)) return HttpAdmission::Complete;

    if ((appState == AppState::WAITING_STM_DELIVERY_RESULT && activePayment.active) ||
        pendingVendReportSend.active) {
      responseCode = 200;
      return HttpAdmission::Complete;
    }
    if (appState != AppState::WAITING_WS_PAYMENT_REPLY || !activePayment.active) {
      queueLocalVendFlowDiagnostic("terminalIgnored", 0U, 0U, 0U,
                                   "paymentResponseWithoutRequest");
      return HttpAdmission::Complete;
    }

    if (activePayment.cancelledByMachine || paymentStatus == "error") {
      if (!sendLineToStm(buildUsbPaymentReplyLine(PaymentReplyType::FAIL))) {
        return HttpAdmission::Complete;
      }
      clearActivePayment();
      changeState(AppState::IDLE);
      responseCode = 200;
      return HttpAdmission::Complete;
    }

    if (!sendPaymentDecision(PaymentReplyType::OK)) {
      return HttpAdmission::Complete;
    }
    activePayment.paymentReplyForwardedToStm = true;
    responseCode = 200;
    return HttpAdmission::Complete;
  }

  if (route != "delivery") return HttpAdmission::Complete;

  if (type == "deliveryLineCheckRequest") {
    int line = 0;
    if (!readJsonIntField(doc, "deliveryLine", line) ||
        line < MIN_VALID_DELIVERY_LINE || line > MAX_VALID_DELIVERY_LINE ||
        !readRequiredControllerId(doc)) {
      responseBody = makeDeliveryLineCheckResponseJson(
          line >= MIN_VALID_DELIVERY_LINE && line <= MAX_VALID_DELIVERY_LINE
              ? static_cast<uint16_t>(line) : 0,
          "invalidDeliveryLine");
      return HttpAdmission::Complete;
    }
    DeliveryLineCheckRequestData request;
    request.deliveryLine = static_cast<uint16_t>(line);
    if (appState == AppState::WAITING_STM_LINE_CHECK_RESULT &&
        pendingLineCheck.active &&
        pendingLineCheck.deliveryLine == request.deliveryLine) {
      pendingLineCheck.httpResponseToken = token;
      setHttpTimeoutBody(token, makeDeliveryLineCheckResponseJson(
                                    request.deliveryLine, "invalidDeliveryLine"));
      return HttpAdmission::Waiting;
    }
    if (appState != AppState::IDLE) {
      responseBody = makeDeliveryLineCheckResponseJson(request.deliveryLine, "busy");
      return HttpAdmission::Complete;
    }
    if (!sendLineToStm(buildUsbDeliveryLineCheckCommandLine(request))) {
      responseBody = makeDeliveryLineCheckResponseJson(
          request.deliveryLine, "invalidDeliveryLine");
      return HttpAdmission::Complete;
    }
    setPendingLineCheck(request, token);
    setHttpTimeoutBody(token, makeDeliveryLineCheckResponseJson(
                                  request.deliveryLine, "invalidDeliveryLine"));
    changeState(AppState::WAITING_STM_LINE_CHECK_RESULT);
    return HttpAdmission::Waiting;
  }

  if (type != "productDeliveryRequest") return HttpAdmission::Complete;

  int id = 0;
  int line = 0;
  if (!readJsonIntField(doc, "productPositionId", id) ||
      !readJsonIntField(doc, "deliveryLine", line) ||
      id < MIN_VALID_PRODUCT_POSITION_ID || id > MAX_VALID_PRODUCT_POSITION_ID ||
      line < MIN_VALID_DELIVERY_LINE || line > MAX_VALID_DELIVERY_LINE ||
      !readRequiredControllerId(doc)) {
    const uint16_t responseLine =
        line >= MIN_VALID_DELIVERY_LINE && line <= MAX_VALID_DELIVERY_LINE
            ? static_cast<uint16_t>(line) : 0;
    responseBody = makeDirectDeliveryRequestStatusJson(
        "error", responseLine, "failed", "unavailable", "invalidRequest", 0,
        false);
    return HttpAdmission::Complete;
  }

  DirectDeliveryRequestData request;
  request.productPositionId = static_cast<uint16_t>(id);
  request.deliveryLine = static_cast<uint16_t>(line);
  if (appState == AppState::WAITING_STM_DIRECT_DELIVERY_RESULT &&
      pendingDirectDelivery.active &&
      pendingDirectDelivery.productPositionId == request.productPositionId &&
      pendingDirectDelivery.deliveryLine == request.deliveryLine) {
    if (pendingDirectDelivery.startStatusReceived ||
        pendingDirectDelivery.startTimeoutReported ||
        pendingDirectDelivery.recoveredAfterRestart) {
      const bool accepted = pendingDirectDelivery.startStatusReceived &&
                            pendingDirectDelivery.startConfirmed;
      responseCode = accepted ? 200 : 500;
      responseBody = makeDirectDeliveryRequestStatusJson(
          accepted ? "accepted" : "error", request.deliveryLine,
          pendingDirectDelivery.startStatusReceived
              ? (accepted ? "started" : "failed")
              : (pendingDirectDelivery.recoveredAfterRestart
                     ? "recoveredWaitingResult" : "confirmationTimeout"),
          pendingDirectDelivery.motorBoardStatus,
          pendingDirectDelivery.startStatusReceived
              ? pendingDirectDelivery.startReason
              : (pendingDirectDelivery.recoveredAfterRestart
                     ? "espRestarted" : "startConfirmationTimeout"),
          pendingDirectDelivery.operationId,
          pendingDirectDelivery.recoveredAfterRestart);
      return HttpAdmission::Complete;
    }
    pendingDirectHttpResponse = {true, token,
                                 pendingDirectDelivery.operationId,
                                 pendingDirectDelivery.deliveryLine,
                                 pendingDirectDelivery.recoveredAfterRestart,
                                 stateEnterMs + STATE_TIMEOUT_DELIVERY_START_MS};
    setHttpTimeoutBody(token, makeDirectDeliveryRequestStatusJson(
                                  "error", request.deliveryLine,
                                  "confirmationTimeout", "unknown",
                                  "startConfirmationTimeout",
                                  pendingDirectDelivery.operationId,
                                  pendingDirectDelivery.recoveredAfterRestart));
    return HttpAdmission::Waiting;
  }

  if (pendingDirectDeliveryResponse.active) {
    responseBody = makeDirectDeliveryRequestStatusJson(
        "error", request.deliveryLine, "resultPending", "unknown",
        "resultPending", pendingDirectDeliveryResponse.data.operationId,
        pendingDirectDeliveryResponse.data.recoveredAfterRestart);
    return HttpAdmission::Complete;
  }
  if (appState != AppState::IDLE) {
    responseBody = makeDirectDeliveryRequestStatusJson(
        "error", request.deliveryLine, "failed", "busy", "controllerBusy", 0,
        false);
    return HttpAdmission::Complete;
  }

  if(univerRebootActive())return HttpAdmission::Complete;
  const uint32_t operationId = createDirectDeliveryOperationId();
  if (!persistActiveDirectDelivery(request, operationId)) {
    responseBody = makeDirectDeliveryRequestStatusJson(
        "error", request.deliveryLine, "failed", "unavailable",
        "journalWriteFailed", operationId, false);
    return HttpAdmission::Complete;
  }
  setPendingDirectDelivery(request, operationId);
  changeState(AppState::WAITING_STM_DIRECT_DELIVERY_RESULT);
  pendingDirectHttpResponse = {true, token, operationId, request.deliveryLine,
                               false,
                               stateEnterMs + STATE_TIMEOUT_DELIVERY_START_MS};
  setHttpTimeoutBody(token, makeDirectDeliveryRequestStatusJson(
                                "error", request.deliveryLine,
                                "confirmationTimeout", "unknown",
                                "startConfirmationTimeout", operationId, false));
  if (!sendLineToStm(buildUsbDirectDeliveryCommandLine(request))) {
    pendingDirectDelivery.startTimeoutReported = true;
    pendingDirectHttpResponse.active = false;
    responseBody = makeDirectDeliveryRequestStatusJson(
        "error", request.deliveryLine, "failed", "unavailable",
        "uartWriteFailed", operationId, false);
    return HttpAdmission::Complete;
  }
  return HttpAdmission::Waiting;
}

bool waitForHttpResponse(const char* route, const String& body,
                         uint16_t& responseCode, char* responseBody,
                         size_t responseBodySize) {
  responseCode = 500;
  strlcpy(responseBody, "{}", responseBodySize);
  if (body.length() > HTTP_JSON_BODY_LIMIT) return true;

  uint32_t token = 0;
  const uint32_t startedMs = millis();
  portENTER_CRITICAL(&httpAdmissionMux);
  if (httpBridge.state == HttpBridgeState::Free) {
    token = nextHttpToken++;
    if (token == 0) token = nextHttpToken++;
    httpBridge = {};
    httpBridge.state = HttpBridgeState::Pending;
    httpBridge.token = token;
    strlcpy(httpBridge.route, route, sizeof(httpBridge.route));
    memcpy(httpBridge.body, body.c_str(), body.length() + 1);
  }
  portEXIT_CRITICAL(&httpAdmissionMux);
  if (token == 0) return true;

  for (;;) {
    const bool connected = httpServer.client().connected();
    const uint32_t elapsedMs = static_cast<uint32_t>(millis() - startedMs);
    portENTER_CRITICAL(&httpAdmissionMux);
    const bool same = httpBridge.token == token;
    const HttpBridgeState state =
        same ? httpBridge.state : HttpBridgeState::Free;
    if (same && state == HttpBridgeState::Complete) {
      responseCode = httpBridge.responseCode;
      strlcpy(responseBody, httpBridge.responseBody, responseBodySize);
      httpBridge = {};
      portEXIT_CRITICAL(&httpAdmissionMux);
      return connected;
    }
    if (!connected && same) {
      httpBridge = {};
      portEXIT_CRITICAL(&httpAdmissionMux);
      return false;
    }
    if (same && state == HttpBridgeState::Pending &&
        elapsedMs >= HTTP_ADMISSION_TIMEOUT_MS) {
      httpBridge = {};
      portEXIT_CRITICAL(&httpAdmissionMux);
      return true;
    }
    if (same && elapsedMs >= HTTP_PROCESSING_TIMEOUT_MS) {
      strlcpy(responseBody, httpBridge.timeoutBody, responseBodySize);
      httpBridge = {};
      portEXIT_CRITICAL(&httpAdmissionMux);
      return true;
    }
    portEXIT_CRITICAL(&httpAdmissionMux);
    vTaskDelay(pdMS_TO_TICKS(1));
  }
}

void pumpHttpAdmission() {
  char route[sizeof(httpBridge.route)] = {};
  char body[sizeof(httpBridge.body)] = {};
  uint32_t token = 0;

  portENTER_CRITICAL(&httpAdmissionMux);
  if (httpBridge.state == HttpBridgeState::Pending) {
    httpBridge.state = HttpBridgeState::Claimed;
    token = httpBridge.token;
    memcpy(route, httpBridge.route, sizeof(route));
    memcpy(body, httpBridge.body, sizeof(body));
  }
  portEXIT_CRITICAL(&httpAdmissionMux);
  if (token == 0) return;

  uint16_t responseCode = 500;
  String responseBody = "{}";
  const HttpAdmission result = admitTerminalJson(
      String(route), String(body), token, responseCode, responseBody);
  if (result == HttpAdmission::Complete) {
    completeHttpResponse(token, responseCode, responseBody);
  }
}

static String diagnosticTerminalBody(const String& body) {
  JsonDocument in,out;
  if(deserializeJson(in,body))return "invalidJson";
  String type=in["type"].as<String>();
  if(type!="paymentResponse"&&type!="deliveryLineCheckRequest"&&type!="productDeliveryRequest")return "invalidType";
  out["type"]=type;
  for(const char* field:{"productPositionId","deliveryLine","controllerId"})if(in[field].is<uint32_t>())out[field]=in[field].as<uint32_t>();
  if(type=="paymentResponse") {
    String status=in["paymentStatus"].as<String>();out["paymentStatus"]=(status=="paid"||status=="error")?status:"invalid";
  }
  String result;serializeJson(out,result);return result;
}
void handleHttpCommand(const char* route) {
  diagnosticBusy(true);
  diagnosticRecord(DiagnosticEvent::TerminalRequest,route,"Terminal request",0,false,diagnosticTerminalBody(httpServer.arg("plain")).c_str());
  uint16_t responseCode = 500;
  char responseBody[HTTP_JSON_RESPONSE_LIMIT + 1] = "{}";
  if (!waitForHttpResponse(route, httpServer.arg("plain"), responseCode,
                           responseBody, sizeof(responseBody))) {
    diagnosticRecord(DiagnosticEvent::TerminalResponse,route,"disconnect",0,true);return;
  }
  httpServer.send(responseCode, "application/json", responseBody);
  diagnosticRecord(DiagnosticEvent::TerminalResponse,route,"Terminal response",responseCode,responseCode!=200,responseBody);
}

#include "UniverStatusHttp.h"

void setupHttpServer() {
  if (httpServerStarted) return;
  httpServer.setRequestLimits(
      HTTP_REQUEST_RECEIVE_TIMEOUT_MS, HTTP_RESPONSE_WRITE_TIMEOUT_MS,
      4096, 1024, 32, HTTP_JSON_BODY_LIMIT);
  const char* networkHeaders[]={"Origin"};httpServer.collectHeaders(networkHeaders,1);
  httpServer.on("/",HTTP_GET,handleNetworkPage);
  httpServer.on("/network-settings",HTTP_GET,handleNetworkSnapshot);
  httpServer.on("/wifi-config",HTTP_POST,[](){handleNetworkForm(false);});
  httpServer.on("/update-wifi",HTTP_GET,[](){handleNetworkForm(true);});
  httpServer.on("/status", HTTP_GET, handleUniverStatus);
  httpServer.on("/reboot", HTTP_GET, handleUniverReboot);
  httpServer.on("/payment", HTTP_POST,
                []() { handleHttpCommand("payment"); });
  httpServer.on("/delivery", HTTP_POST,
                []() { handleHttpCommand("delivery"); });
  httpServer.onNotFound([]() {
    httpServer.send(500, "application/json", "{}");
  });
  httpServer.begin();
  httpServerStarted = true;
  logMessage("[HTTP] Local terminal API listening on port 8000.");
}

void httpServerTaskEntry(void*) {
  for (;;) {
    noteHttpTaskProgress();
    serviceNetworkPortalHttp();
    // The HTTP task owns retries. A failed begin must not disable HTTP forever.
    static uint32_t lastListenAttemptMs=0;
    if (!httpServer.isListening() && (uint32_t)(millis()-lastListenAttemptMs)>=1000U) {
      lastListenAttemptMs=millis();
      httpServer.begin();
    }
    httpServer.handleClient();
    vTaskDelay(pdMS_TO_TICKS(1));
  }
}

bool startHttpServerTask() {
  if (httpServerTask != nullptr) return true;
  return xTaskCreatePinnedToCore(httpServerTaskEntry, "http-terminal", 6144,
                                 nullptr, 1, &httpServerTask, 0) == pdPASS;
}

bool startConfiguredHttpServer() {
  setupHttpServer();
  if (startHttpServerTask()) return true;
  httpServer.stop();
  httpServerStarted = false;
  logMessage("[HTTP] Не удалось создать задачу local terminal API; port закрыт.");
  return false;
}

void processNewPaymentRequestFromStm(const String& line) {
  PaymentRequestData request;
  if (!parseUsbPaymentRequestLine(line, request)) {
    queueLocalVendFlowDiagnostic("gatewayIgnored", 0U, 0U, 0U,
                                 "paymentRequestParseFailed");
    logMessage("[STM RX] Не удалось распарсить PAYMENT_REQUEST: " + line);
    return;
  }

  if (activePayment.active && activePayment.transactionId == request.transactionId && activePayment.bootIdentity == request.bootIdentity) {
    queueLocalVendFlowDiagnostic("gatewayIgnored", request.productPositionId,
                                 request.deliveryLine, request.transactionId,
                                 "duplicatePaymentRequest");
    return;
  }
  if (univerRebootActive() || appState != AppState::IDLE || pendingVendReportSend.active) {
    queueLocalVendFlowDiagnostic("gatewayRejected", request.productPositionId,
                                 request.deliveryLine, request.transactionId,
                                 pendingVendReportSend.active
                                     ? "vendReportPending" : "gatewayBusy");
    logMessage("[FLOW] PAYMENT_REQUEST отклонён: устройство занято, state=" +
               String(stateToString(appState)));
    sendLineToStm(buildUsbPaymentReplyForTransaction(PaymentReplyType::FAIL, request.transactionId, request.bootIdentity));
    return;
  }

  setActivePayment(request);
  if (!persistPaymentFlow("W")) {
    queueLocalVendFlowDiagnostic("gatewayRejected", request.productPositionId,
                                 request.deliveryLine, request.transactionId,
                                 "paymentJournalFailed");
    sendLineToStm(buildUsbPaymentReplyLine(PaymentReplyType::FAIL));
    clearActivePayment();
    return;
  }
  changeState(AppState::WAITING_WS_PAYMENT_REPLY);
  if (canSendWebSocketNow() && !pendingOrderCancellationRequest.active) {
    activePaymentRequestHandle = httpsTransport.enqueue(
        makePaymentRequestJson(request), HttpsCommandKind::Payment,
        STATE_TIMEOUT_PAYMENT_MS);
  }
  if (activePaymentRequestHandle.valid()) {
    activePaymentRequestSentToWs = true;
    logMessage("[FLOW] PAYMENT_REQUEST отправлен WS-клиенту.");
  } else {
    diagnosticRecord(DiagnosticEvent::Flow,"paymentDeferred","Payment deferred",0,true,("wifi="+String(wifiReady?1:0)+",transport="+String(httpsTransport.available()?1:0)+",clock="+String(httpsTransport.tlsClockReady()?1:0)).c_str());
    logMessage("[FLOW] PAYMENT_REQUEST сохранён, ждём WS reconnect/timeout.");
  }
}

void processDeliveryResultFromStm(const String& line) {
  VendReportData result;
  if (!parseUsbDeliveryResultLine(line, result)) {
    logMessage("[STM RX] Не удалось распарсить VEND_REPORT: " + line);
    return;
  }

  if (!activePayment.active) {
    sendOrQueueVendReport(result);
    logMessage("[FLOW] VEND_REPORT без активной транзакции отправлен как поздний итог.");
    return;
  }

  if (result.bootIdentity != activePayment.bootIdentity || result.transactionId != activePayment.transactionId ||
      result.productPositionId != activePayment.productPositionId) {
    queueLocalVendFlowDiagnostic("resultMismatch", result.productPositionId,
                                 findMotorBySelectionId(result.productPositionId),
                                 result.transactionId,
                                 "activeTransactionMismatch");
    logMessage("[FLOW] VEND_REPORT id не совпал. Ожидали " +
               String(activePayment.productPositionId) + ", получили " +
               String(result.productPositionId));
    sendOrQueueVendReport(result);
    return;
  }

  // Fallback if the preceding ORDER_CANCELLED UART line was lost. A successful
  // snack deliberately has machine=cancelled: it must NEVER create cancellation.
  if (result.machineStatusRaw == "cancelled" && result.localStatusRaw != "success" &&
      (result.localStatusRaw == "skip" || !activePayment.paymentReplyForwardedToStm) &&
      activePaymentRequestSentToWs && !activePayment.cancelledByMachine) {
    activePayment.cancelledByMachine = true;
    if (httpsTransport.cancelPaymentAndWasAttempted(activePaymentRequestHandle))
      sendOrQueueOrderCancellationRequest();
  }
  bool sentNow = sendOrQueueVendReport(result);
  if (!sentNow) return;
  clearActivePayment();
  changeState(AppState::IDLE);
  logMessage(sentNow ? "[FLOW] Итог продажи отправлен." :
                       "[FLOW] Итог продажи ждёт WS reconnect.");
}

void processConfigurationAckFromStm(const String& line) {
  ConfigAckData ack;
  if (!parseUsbConfigurationAckLine(line, ack)) {
    logMessage("[STM RX] Не удалось распарсить SET_MAP_RESULT: " + line);
    return;
  }

  if (!pendingConfig.active) {
    logMessage("[FLOW] SET_MAP_RESULT без активной команды.");
    changeState(AppState::IDLE);
    return;
  }

  if (ack.success) {
    if (ack.motor == pendingConfig.deliveryLine &&
        ack.productPositionId == pendingConfig.productPositionId) {
      setSelectionMotorMapping(ack.productPositionId, static_cast<uint8_t>(ack.motor));
      logMessage("[FLOW] Конфигурация подтверждена.");
    } else {
      logMessage("[FLOW] SET_MAP_RESULT не совпал с ожидаемой командой.");
    }
  } else {
    logMessage("[FLOW] STM32 вернул SET_MAP_RESULT,error.");
  }

  clearPendingConfig();
  changeState(AppState::IDLE);
}

void processDirectDeliveryResultFromStm(const String& line) {
  DirectDeliveryResultData result;
  if (!parseDirectDeliveryResultLine(line, result) || !pendingDirectDelivery.active ||
      result.operationId != pendingDirectDelivery.operationId ||
      result.deliveryLine != pendingDirectDelivery.deliveryLine) return;
  if (directResultNeedsPersistence) return; // Keep the first definitive result.
  result.recoveredAfterRestart = pendingDirectDelivery.recoveredAfterRestart;
  directResultToPersist = result;
  directResultNeedsPersistence = true;
  nextDirectRecoveryMs = millis();
}

void serviceDirectDeliveryRecovery() {
  if (!pendingDirectDelivery.active || (int32_t)(millis() - nextDirectRecoveryMs) < 0) return;
  nextDirectRecoveryMs = millis() + 1000U;
  if (directResultNeedsPersistence) {
    if (!persistDirectDeliveryResult(directResultToPersist, pendingDirectDelivery.productPositionId)) return;
    sendOrQueueDirectDeliveryResponse(directResultToPersist);
    clearPendingDirectDelivery();
    changeState(AppState::IDLE);
    return;
  }
  String query = "DELIVERY_QUERY,line=" + String(pendingDirectDelivery.deliveryLine) +
                 ",op=" + String(pendingDirectDelivery.operationId);
  sendLineToStm(query);
}

void processDirectDeliveryStartResultFromStm(const String& line) {
  DirectDeliveryStartResultData result;
  if (!parseDirectDeliveryStartResultLine(line, result)) {
    logMessage("[STM RX] Не удалось распарсить DELIVERY_START_RESULT: " + line);
    return;
  }

  const bool matchesDelivery = pendingDirectDelivery.active &&
      result.deliveryLine == pendingDirectDelivery.deliveryLine &&
      result.operationId == pendingDirectDelivery.operationId;
  const bool matchesHttp = pendingDirectHttpResponse.active &&
      result.deliveryLine == pendingDirectHttpResponse.deliveryLine &&
      result.operationId == pendingDirectHttpResponse.operationId;
  if (!matchesDelivery && !matchesHttp) {
    logMessage("[FLOW] DELIVERY_START_RESULT без активной команды: line=" +
               String(result.deliveryLine) + ", reason=" + result.reason);
    return;
  }

  if (matchesDelivery && !pendingDirectDelivery.startStatusReceived) {
    pendingDirectDelivery.startStatusReceived = true;
    pendingDirectDelivery.startConfirmed = result.started;
    pendingDirectDelivery.motorBoardStatus = result.motorBoardStatus;
    pendingDirectDelivery.startReason = result.reason;
  }

  if (matchesHttp) {
    completeHttpResponse(
        pendingDirectHttpResponse.token, result.started ? 200 : 500,
        makeDirectDeliveryRequestStatusJson(
            result.started ? "accepted" : "error", result.deliveryLine,
            result.started ? "started" : "failed", result.motorBoardStatus,
            result.reason, result.operationId,
            pendingDirectHttpResponse.recoveredAfterRestart));
    pendingDirectHttpResponse.active = false;
  } else if (matchesDelivery && pendingDirectDelivery.startTimeoutReported) {
    logMessage("[FLOW] Поздний DELIVERY_START_RESULT после уже сообщённого timeout.");
  }

  logMessage("[FLOW] F103 start status: line=" + String(result.deliveryLine) +
             ", started=" + String(result.started ? "yes" : "no") +
             ", f103Status=" + result.motorBoardStatus +
             ", reason=" + result.reason);
}

void processDeliveryLineCheckResultFromStm(const String& line) {
  DeliveryLineCheckResultData result;
  if (!parseDeliveryLineCheckResultLine(line, result)) {
    logMessage("[STM RX] Не удалось распарсить DELIVERY_LINE_CHECK_RESULT: " + line);
    return;
  }

  if (!pendingLineCheck.active) {
    logMessage("[FLOW] DELIVERY_LINE_CHECK_RESULT без активного запроса.");
    return;
  }

  if (result.deliveryLine != pendingLineCheck.deliveryLine) {
    logMessage("[FLOW] DELIVERY_LINE_CHECK_RESULT line не совпал. Ожидали " +
               String(pendingLineCheck.deliveryLine) + ", получили " +
               String(result.deliveryLine));
    completeHttpResponse(
        pendingLineCheck.httpResponseToken, 500,
        makeDeliveryLineCheckResponseJson(
            pendingLineCheck.deliveryLine, "invalidDeliveryLine"));
    clearPendingLineCheck();
    changeState(AppState::IDLE);
    return;
  }

  completeHttpResponse(
      pendingLineCheck.httpResponseToken, result.status == "ready" ? 200 : 500,
      makeDeliveryLineCheckResponseJson(result.deliveryLine, result.status));
  clearPendingLineCheck();
  changeState(AppState::IDLE);
  logMessage("[FLOW] DELIVERY_LINE_CHECK_RESULT подготовлен для HTTP-клиента.");
}

void processOrderCancelledFromStm() {
  if (activePayment.active &&
      (appState == AppState::WAITING_WS_PAYMENT_REPLY ||
       appState == AppState::WAITING_STM_DELIVERY_RESULT)) {
    activePayment.cancelledByMachine = true;
    PaymentRequestData cancelledRequest;
    cancelledRequest.productPositionId = activePayment.productPositionId;
    cancelledRequest.priceMinor = activePayment.priceMinor;
    cancelledRequest.productPrice = activePayment.productPrice;
    cancelledRequest.deliveryLine = activePayment.deliveryLine;
    const bool paymentAttempted = httpsTransport.cancelPaymentAndWasAttempted(activePaymentRequestHandle);
    if (paymentAttempted) sendOrQueueOrderCancellationRequest();
    logMessage("[FLOW] STM32 сообщил отмену заказа кофемашиной.");
    return;
  }

  logMessage("[FLOW] ORDER_CANCELLED без активной оплаты отклонён как stale.");
}

void processControllerDiagnosticFromStm(const String& line) {
  ControllerDiagnosticData diagnostic;
  if (!parseControllerDiagnosticLine(line, diagnostic)) {
    logMessage("[STM RX] Не удалось распарсить CONTROLLER_DIAGNOSTIC: " + line);
    return;
  }

  if (diagnostic.source == "f411") {
    lastF411Event = diagnostic.event;
    lastF411Status = diagnostic.status;
    lastF411Count = diagnostic.count;
    lastF411State = diagnostic.deliveryLine;
  } else if (diagnostic.source == "f103") {
    lastF103Event = diagnostic.event;
    lastF103Status = diagnostic.status;
    lastF103CommandCounter = diagnostic.count;
    lastF103Line = diagnostic.deliveryLine;
  }

  if (diagnostic.source == "vendFlow") {
    diagnosticRecord(DiagnosticEvent::Flow,diagnostic.event.c_str(),"Controller flow event",0,true,line.c_str());
    queueVendFlowDiagnostic(diagnostic);
    logMessage("[VEND FLOW] event=" + diagnostic.event +
               ", id=" + String(diagnostic.count) +
               ", line=" + String(diagnostic.deliveryLine) +
               ", status=" + diagnostic.status);
    return;
  }

  const bool urgent = isUrgentControllerDiagnostic(diagnostic);
  // Controller diagnostics remain local; the shortened public contract defines
  // only three outbound business message types.
  queueControllerDiagnostic(diagnostic, urgent);
  logMessage("[DIAGNOSTIC] source=" + diagnostic.source +
             ", event=" + diagnostic.event +
             ", line=" + String(diagnostic.deliveryLine) +
             ", count=" + String(diagnostic.count) +
             ", status=" + diagnostic.status);
}

void processLineFromStm(const String& line) {
  if(receiveUniverReboot(line))return;
  if(line.startsWith("PAYMENT_REQUEST,")||line.startsWith("VEND_REPORT,")||line.startsWith("ORDER_CANCELLED,")||line.startsWith("DELIVERY_RESULT,")||line.startsWith("DELIVERY_START_RESULT,"))
    diagnosticRecord(DiagnosticEvent::Flow,"stm","UART controller event",0,line.startsWith("ORDER_CANCELLED,")||(line.startsWith("VEND_REPORT,")&&line.indexOf("machine=success")<0&&line.indexOf("local=success")<0),line.c_str());
  logMessage("[STM RX] " + line);
  if (line.startsWith("PAYMENT_ACK,")) {
    String expected = "PAYMENT_ACK,tx=" + String(activePayment.transactionId) +
        ",boot=" + String(activePayment.bootIdentity);
    if (activePayment.active && line == expected && pendingPaymentDecision.length() && persistPaymentFlow("A")) {
      pendingPaymentDecision="";
	} else {
	  queueLocalVendFlowDiagnostic("gatewayIgnored",
	                               activePayment.active
	                                   ? activePayment.productPositionId : 0U,
	                               activePayment.active
	                                   ? activePayment.deliveryLine : 0U,
	                               activePayment.active
	                                   ? activePayment.transactionId : 0U,
	                               "paymentAckMismatch");
    }
    return;
  }
  UsbIncomingLineType lineType = detectUsbIncomingLineType(line);

  if (lineType == UsbIncomingLineType::VEND_REPORT) {
    processDeliveryResultFromStm(line);
    return;
  }

  if (lineType == UsbIncomingLineType::ORDER_CANCELLED) {
    String tokens[3], txStr, bootStr; size_t count=0;
    uint32_t tx=0, boot=0;
    if (!splitByComma(line, tokens, 3, count) || count!=3 ||
        !extractTokenValue(tokens[1], "tx", txStr) || !extractTokenValue(tokens[2], "boot", bootStr) ||
        !parseUInt32Strict(txStr, tx) || !parseUInt32Strict(bootStr, boot) || !activePayment.active ||
        tx != activePayment.transactionId || boot != activePayment.bootIdentity) {
      queueLocalVendFlowDiagnostic("gatewayIgnored",
                                   activePayment.active
                                       ? activePayment.productPositionId : 0U,
                                   activePayment.active
                                       ? activePayment.deliveryLine : 0U,
                                   tx, "orderCancelledMismatch");
      return;
    }
    processOrderCancelledFromStm();
    return;
  }

  if (lineType == UsbIncomingLineType::DELIVERY_RESULT) {
    processDirectDeliveryResultFromStm(line);
    return;
  }
  if (lineType == UsbIncomingLineType::DELIVERY_START_RESULT) {
    processDirectDeliveryStartResultFromStm(line);
    return;
  }

  if (lineType == UsbIncomingLineType::CONTROLLER_DIAGNOSTIC) {
    processControllerDiagnosticFromStm(line);
    return;
  }

  switch (appState) {
    case AppState::IDLE:
      if (lineType == UsbIncomingLineType::PAYMENT_REQUEST) {
        processNewPaymentRequestFromStm(line);
      } else {
        logMessage("[FLOW] В IDLE ожидается PAYMENT_REQUEST.");
      }
      break;

    case AppState::WAITING_WS_PAYMENT_REPLY:
      if (lineType == UsbIncomingLineType::PAYMENT_REQUEST) {
        processNewPaymentRequestFromStm(line);
      } else {
        logMessage("[FLOW] Строка от STM проигнорирована: ждём paymentStatus.");
      }
      break;

    case AppState::WAITING_STM_DELIVERY_RESULT:
      if (lineType == UsbIncomingLineType::VEND_REPORT) {
        processDeliveryResultFromStm(line);
      } else if (lineType == UsbIncomingLineType::PAYMENT_REQUEST) {
        processNewPaymentRequestFromStm(line);
      } else {
        logMessage("[FLOW] Ожидается VEND_REPORT.");
      }
      break;

    case AppState::WAITING_STM_CONFIG_ACK:
      if (lineType == UsbIncomingLineType::SET_MAP_RESULT) {
        processConfigurationAckFromStm(line);
      } else if (lineType == UsbIncomingLineType::PAYMENT_REQUEST) {
        processNewPaymentRequestFromStm(line);
      } else {
        logMessage("[FLOW] Ожидается SET_MAP_RESULT.");
      }
      break;

    case AppState::WAITING_STM_DIRECT_DELIVERY_RESULT:
      if (lineType == UsbIncomingLineType::DELIVERY_RESULT) {
        processDirectDeliveryResultFromStm(line);
      } else if (lineType == UsbIncomingLineType::PAYMENT_REQUEST) {
        processNewPaymentRequestFromStm(line);
      } else {
        logMessage("[FLOW] Ожидается DELIVERY_RESULT.");
      }
      break;

    case AppState::WAITING_STM_LINE_CHECK_RESULT:
      if (lineType == UsbIncomingLineType::DELIVERY_LINE_CHECK_RESULT) {
        processDeliveryLineCheckResultFromStm(line);
      } else if (lineType == UsbIncomingLineType::PAYMENT_REQUEST) {
        processNewPaymentRequestFromStm(line);
      } else {
        logMessage("[FLOW] Ожидается DELIVERY_LINE_CHECK_RESULT.");
      }
      break;
  }
}

void servicePendingDirectHttpResponseTimeout() {
  if (!pendingDirectHttpResponse.active ||
      static_cast<int32_t>(millis() - pendingDirectHttpResponse.deadlineMs) < 0) {
    return;
  }
  if (pendingDirectDelivery.active &&
      pendingDirectDelivery.operationId == pendingDirectHttpResponse.operationId) {
    pendingDirectDelivery.startTimeoutReported = true;
  }
  completeHttpResponse(
      pendingDirectHttpResponse.token, 500,
      makeDirectDeliveryRequestStatusJson(
          "error", pendingDirectHttpResponse.deliveryLine,
          "confirmationTimeout", "unknown", "startConfirmationTimeout",
          pendingDirectHttpResponse.operationId,
          pendingDirectHttpResponse.recoveredAfterRestart));
  pendingDirectHttpResponse.active = false;
  logMessage("[TIMEOUT] Не получено подтверждение F103 BUSY; итог выдачи всё ещё ожидается.");
}

void handleStateTimeouts() {
  serviceDirectDeliveryRecovery();
  servicePendingDirectHttpResponseTimeout();
  uint32_t elapsed = millis() - stateEnterMs;

  switch (appState) {
    case AppState::IDLE:
      return;

    case AppState::WAITING_WS_PAYMENT_REPLY:
      if (elapsed >= STATE_TIMEOUT_PAYMENT_MS) {
        diagnosticRecord(DiagnosticEvent::Flow,"paymentTimeout","Payment timed out",0,true,("tx="+String(activePayment.transactionId)+",id="+String(activePayment.productPositionId)).c_str());
        logMessage("[TIMEOUT] Не дождались paymentStatus.");
        PaymentRequestData expiredRequest;
        expiredRequest.productPositionId = activePayment.productPositionId;
        expiredRequest.priceMinor = activePayment.priceMinor;
        expiredRequest.productPrice = activePayment.productPrice;
        expiredRequest.deliveryLine = activePayment.deliveryLine;
        const bool paymentAttempted = httpsTransport.cancelPaymentAndWasAttempted(activePaymentRequestHandle);
        if (paymentAttempted && !activePayment.cancelledByMachine)
          sendOrQueueOrderCancellationRequest();
#if APP_TEST_MODE
        clearActivePayment();
#else
        sendLineToStm(buildUsbPaymentReplyLine(PaymentReplyType::TIMEOUT));
        clearActivePayment();
#endif
        changeState(AppState::IDLE);
      }
      break;

    case AppState::WAITING_STM_DELIVERY_RESULT:
      if (elapsed >= STATE_TIMEOUT_DELIVERY_MS) {
        diagnosticRecord(DiagnosticEvent::Flow,"vendReportTimeout","Vend report timed out",0,true,("tx="+String(activePayment.transactionId)+",id="+String(activePayment.productPositionId)).c_str());
        logMessage("[TIMEOUT] Не дождались VEND_REPORT.");
        clearActivePayment();
        changeState(AppState::IDLE);
      }
      break;

    case AppState::WAITING_STM_CONFIG_ACK:
      if (elapsed >= STATE_TIMEOUT_CONFIG_ACK_MS) {
        logMessage("[TIMEOUT] Не дождались SET_MAP_RESULT.");
        clearPendingConfig();
        changeState(AppState::IDLE);
      }
      break;

    case AppState::WAITING_STM_DIRECT_DELIVERY_RESULT:
      if (pendingDirectDelivery.active &&
          !pendingDirectDelivery.startStatusReceived &&
          !pendingDirectDelivery.startTimeoutReported &&
          elapsed >= STATE_TIMEOUT_DELIVERY_START_MS) {
        pendingDirectDelivery.startTimeoutReported = true;
        logMessage("[TIMEOUT] Не получено подтверждение F103 BUSY; итог выдачи всё ещё ожидается.");
      }
      if (elapsed >= STATE_TIMEOUT_DELIVERY_MS && pendingDirectDelivery.active && !directResultNeedsPersistence) {
        // The physical F103 deadline is much shorter. Fail conservatively on unavailable recovery.
        directResultToPersist.deliveryLine = pendingDirectDelivery.deliveryLine;
        directResultToPersist.statusRaw = "failed";
        directResultToPersist.snackMachineStatus = "failed";
        directResultToPersist.operationId = pendingDirectDelivery.operationId;
        directResultToPersist.recoveredAfterRestart = pendingDirectDelivery.recoveredAfterRestart;
        directResultNeedsPersistence = true;
        nextDirectRecoveryMs = millis();
      }
      break;

    case AppState::WAITING_STM_LINE_CHECK_RESULT:
      if (elapsed >= STATE_TIMEOUT_LINE_CHECK_MS) {
        logMessage("[TIMEOUT] Не дождались DELIVERY_LINE_CHECK_RESULT.");
        if (pendingLineCheck.active) {
          completeHttpResponse(
              pendingLineCheck.httpResponseToken, 500,
              makeDeliveryLineCheckResponseJson(
                  pendingLineCheck.deliveryLine, "invalidDeliveryLine"));
        }
        clearPendingLineCheck();
        changeState(AppState::IDLE);
      }
      break;
  }
}

void handleTestModePaymentTimer() {
#if APP_TEST_MODE
  if (!wsReady || appState != AppState::IDLE) {
    return;
  }

  uint32_t now = millis();
  if (now - lastTestPaymentAttemptMs < TEST_PAYMENT_INTERVAL_MS) {
    return;
  }

  lastTestPaymentAttemptMs = now;

  PaymentRequestData request;
  request.productPositionId = TEST_PRODUCT_POSITION_ID;
  request.priceMinor = TEST_PRICE_MINOR;
  request.productPrice = static_cast<double>(TEST_PRICE_MINOR) / PRICE_MINOR_DIVISOR;
  request.deliveryLine = findMotorBySelectionId(TEST_PRODUCT_POSITION_ID);
  if (request.deliveryLine == 0) {
    request.deliveryLine = TEST_DELIVERY_LINE;
  }

  logMessage("[TEST] Имитируем оплату: id=" + String(request.productPositionId) +
             ", priceMinor=" + String(request.priceMinor) +
             ", deliveryLine=" + String(request.deliveryLine));

  const HttpsCommandHandle paymentHandle = httpsTransport.enqueue(
      makePaymentRequestJson(request), HttpsCommandKind::Payment,
      STATE_TIMEOUT_PAYMENT_MS);
  if (paymentHandle.valid()) {
    setActivePayment(request);
    activePaymentRequestHandle = paymentHandle;
    changeState(AppState::WAITING_WS_PAYMENT_REPLY);
  }
#endif
}

// Controlled fallbacks never repeat a physical operation or transplant a paid key.
static void retireOldF411Context(uint32_t newBoot) {
  if(activePayment.active && activePayment.bootIdentity && activePayment.bootIdentity<newBoot) {
    const bool paymentAttempted = httpsTransport.cancelPaymentAndWasAttempted(activePaymentRequestHandle);
    if (paymentAttempted && !activePayment.paymentReplyForwardedToStm &&
        !activePayment.cancelledByMachine) sendOrQueueOrderCancellationRequest();
    controllerLink.record("paymentCancelled","F411 boot changed; old payment retired",millis(),activePayment.transactionId,activePayment.bootIdentity);
    finalResultComplete(false,activePayment.bootIdentity,activePayment.transactionId,false);
    clearActivePayment();
    if(appState==AppState::WAITING_WS_PAYMENT_REPLY||appState==AppState::WAITING_STM_DELIVERY_RESULT)changeState(AppState::IDLE);
  }
  if(pendingVendReportSend.active && pendingVendReportSend.data.bootIdentity<newBoot) {
    const auto old=pendingVendReportSend.data;
    finalResultComplete(false,old.bootIdentity,old.transactionId,false);
    const String record="X|"+serializeVendReport(old);
    if(vendJournalReady)vendPreferences.putString("result",record);
    durableVendRecord=record;
    clearPendingVendReportSend();
    controllerLink.record("resultCancelled","F411 boot changed; old final abandoned",millis(),old.transactionId,old.bootIdentity);
  }
}
static void serviceControllerLinkRecovery() {
  uint32_t now=millis();
  const HttpTaskHealth http=getHttpTaskHealth();
  now=millis();
  if(ControllerLinkMonitor::workerRestartDue(now,http.loops,http.lastLoopMs)) {
    controllerLink.record("httpRestart","HTTP task has no progress for 55s",now);
    if(healthPreferencesReady)healthPreferences.putUInt(HEALTH_PLANNED_REASON_KEY,PLANNED_RESTART_HTTP_STALL);
    markDiagnosticPhase(DiagnosticPhase::DEVICE_RESTART);
    ESP.restart();
    return;
  }
  const HttpsWorkerHealth worker=HttpsCommandTransport::health();
  now=millis();
  static uint32_t retriedWorkerAt=0;
  if(worker.loops==0&&(uint32_t)(now-retriedWorkerAt)>=5000U) {
    retriedWorkerAt=now;
    httpsTransport.begin(); // retries task-creation failure; existing task is never duplicated
  }
  if(ControllerLinkMonitor::workerRestartDue(now,worker.loops,worker.lastLoopMs)) {
    controllerLink.record("workerRestart","HTTPS worker has no progress for 55s",now);
    if(healthPreferencesReady)healthPreferences.putUInt(HEALTH_PLANNED_REASON_KEY,PLANNED_RESTART_WORKER_STALL);
    markDiagnosticPhase(DiagnosticPhase::DEVICE_RESTART);
    ESP.restart();
    return;
  }
#if !STM_USB_HOST_MODE && !APP_TEST_MODE
  if(controllerLink.uartRestartDue(now)) {
    controllerLink.markUartRestart(now);
    STM_PORT.end();
    stmRxBuffer="";
    STM_PORT.begin(STM_UART_BAUD,SERIAL_8N1,STM_UART_RX_PIN,STM_UART_TX_PIN);
  }
  // Read-only query also discovers fresh reason retained by F411 after ESP lost a frame.
  static uint32_t queried=0;
  if((uint32_t)(now-queried)>=5000U){queried=now;sendLineToStm("F411_QUERY");}
#endif
}

void setup() {
  pinMode(LED_PIN, OUTPUT);
  setLed(true);

#if SERIAL_DEBUG_ENABLED
#if STM_USB_HOST_MODE
  Serial0.begin(115200);
#else
#if !APP_TEST_MODE
  Serial0.begin(115200);
#else
  Serial.begin(115200);
#endif
#endif
#endif
#if !STM_USB_HOST_MODE
  STM_PORT.begin(STM_UART_BAUD, SERIAL_8N1, STM_UART_RX_PIN, STM_UART_TX_PIN);
#endif
  delay(300);
  initDefaultSelectionMap();

  logMessage("");
  logMessage("====== ESP32-S3 VMC HTTP/HTTPS API v2 ======");
  logMessage(String("[CONFIG] Mode: ") +
             (APP_TEST_MODE ? "TEST" : (STM_USB_HOST_MODE ? "USB_HOST" : "UART")));
  logMessage("[CONFIG] Local HTTP: http://" + LOCAL_IP.toString() +
             ":8000/payment and /delivery");
  logMessage("[CONFIG] WIFI_SSID=" + String(WIFI_SSID));
  logMessage("[CONFIG] LED D2/GPIO=" + String(LED_PIN));
#if !STM_USB_HOST_MODE
  logMessage("[CONFIG] STM UART baud=" + String(STM_UART_BAUD) +
             " rx=GPIO" + String(STM_UART_RX_PIN) +
             " tx=GPIO" + String(STM_UART_TX_PIN));
#endif

#if STM_USB_HOST_MODE
  initStmUsbHost();
#endif
  WiFi.onEvent(onWiFiEvent);
  beginUniverNetwork();
  initializeGatewayHealth();
  beginRebootDiagnostics(bootCounter,resetCanRetainRtcBreadcrumb(bootResetReason));
  diagnosticBegin(bootCounter,FIRMWARE_BUILD_ID,resetReasonToString(bootResetReason));
  beginUniverReboot();
  if(!univerRebootBlocksBusiness()) {
    finalResultPolicyBegin();
    initializeAndRestoreDeliveryJournal();
    restorePendingVendReport();
    restorePaymentFlow();
  }
  if (!httpsTransport.begin()) {
    logMessage("[HTTPS] Не удалось создать transport worker; API недоступен.");
  }
  httpsTransport.notifyWifiReady(wifiReady);
  if(univerRebootBlocksBusiness())httpsTransport.suspendForReboot();
  startConfiguredHttpServer();
  if (!pendingDirectDelivery.active && !activePayment.active) {
    changeState(AppState::IDLE);
  }
  enableLoopWDT();
  logMessage("[WDT] ESP loop task watchdog enabled.");
}

void loop() {
  const uint32_t loopStartedMs = millis();
  if (lastLoopStartedMs != 0) {
    const uint32_t loopGapMs = loopStartedMs - lastLoopStartedMs;
    if (loopGapMs > maxLoopGapMs) {
      maxLoopGapMs = loopGapMs;
    }
  }
  lastLoopStartedMs = loopStartedMs;
  ++loopCount;
  markDiagnosticPhase(DiagnosticPhase::LOOP_START);

  markDiagnosticPhase(DiagnosticPhase::STM_SERVICE_1);
  serviceStmTransport();
  markDiagnosticPhase(DiagnosticPhase::WIFI_RECOVERY);
  serviceUniverNetwork();
  bool terminalPending;
  portENTER_CRITICAL(&httpAdmissionMux);terminalPending=httpBridge.state!=HttpBridgeState::Free;portEXIT_CRITICAL(&httpAdmissionMux);
  diagnosticBusy(terminalPending || appState!=AppState::IDLE);
  httpsTransport.notifyWifiReady(wifiReady);
  sampleRadioDiagnostics();
  markDiagnosticPhase(DiagnosticPhase::STM_SERVICE_2);
  serviceStmTransport();
  pumpHttpAdmission();
  if(univerRebootBlocksBusiness()) {
    httpsTransport.suspendForReboot();
    String line;
    while(readLineFromStm(line)){
      if(controllerLink.receive(line.c_str(),millis()))continue;
      (void)receiveUniverReboot(line);
    }
    serviceUniverReboot();
    lastLoopCompletedMs=millis();lastLoopDurationMs=lastLoopCompletedMs-loopStartedMs;
    return;
  }
  markDiagnosticPhase(DiagnosticPhase::TELEMETRY);
  markDiagnosticPhase(DiagnosticPhase::PENDING_WORK_1);
  pumpPendingWebSocketWork();
  pumpHttpAdmission();
  markDiagnosticPhase(DiagnosticPhase::STM_SERVICE_3);
  serviceStmTransport();

#if !APP_TEST_MODE
  markDiagnosticPhase(DiagnosticPhase::STM_RX);
  String line;
  while (readLineFromStm(line)) {
    const bool telemetry=controllerLink.receive(line.c_str(),millis());
    if(telemetry){if(controllerLink.state().seen)retireOldF411Context(controllerLink.state().boot);continue;}
    processLineFromStm(line);
  }
#endif
  serviceUniverReboot();
  serviceControllerLinkRecovery();

  markDiagnosticPhase(DiagnosticPhase::TEST_MODE);
  handleTestModePaymentTimer();
  markDiagnosticPhase(DiagnosticPhase::PENDING_WORK_2);
  pumpPendingWebSocketWork();
  markDiagnosticPhase(DiagnosticPhase::STATE_TIMEOUTS);
  handleStateTimeouts();

  lastLoopCompletedMs = millis();
  lastLoopDurationMs = lastLoopCompletedMs - loopStartedMs;
  if (lastLoopDurationMs > maxLoopDurationMs) {
    maxLoopDurationMs = lastLoopDurationMs;
  }
  markDiagnosticPhase(DiagnosticPhase::LOOP_IDLE);
}
