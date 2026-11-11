#include "WifiConfigService.h"
#include "SetupConfig.h"

#include <Preferences.h>
#include <WiFi.h>
#include <string.h>

namespace {
const char *const WIFI_NAMESPACE = "univerWifi";
const char *const CREDENTIALS_KEY = "credentials-v2";
const uint32_t CREDENTIALS_MAGIC = 0x57494632U;
const uint32_t RECONNECT_DELAY_MILLIS = 250;

// NVS commits this small value atomically; a failed save keeps the old profile.
struct StoredCredentials {
  uint32_t magic;
  char ssid[33];
  char password[65];
};

bool validSsid(const char *value) {
  const size_t size = strnlen(value, 33);
  if (size == 0 || size > 32) return false;
  for (size_t i = 0; i < size; ++i) {
    if (static_cast<unsigned char>(value[i]) < 32 || value[i] == 127) return false;
  }
  return true;
}

bool validPassword(const char *value) {
  const size_t size = strnlen(value, 65);
  if (size == 0) return true;  // Open upstream networks are supported.
  if (size < 8 || size > 64) return false;
  for (size_t i = 0; i < size; ++i) {
    const unsigned char c = value[i];
    if (c < 32 || c > 126) return false;
    if (size == 64 && !((c >= '0' && c <= '9') ||
        (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false;
  }
  return true;
}

int base64Value(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}
}  // namespace

const char *const WifiConfigService::PORTAL_SSID = "Univer-Setup";
const char *const WifiConfigService::PORTAL_PASSWORD = "F7q9-M4v2-R8k6-P3x5";

WifiConfigService::WifiConfigService(const DeviceConfig &config)
  : deviceConfig_(config), attemptStartedAt_(0), reconnectScheduledAt_(0), offlineStartedAt_(0),
    portalLastClientAt_(0), attemptingLocal_(false), connected_(false),
    reconnectScheduled_(false), portalOpen_(false), portalHadClients_(false),
    pendingEvent_(Event::None) {
  memset(&localConfig_, 0, sizeof(localConfig_));
  memset(&defaultConfig_, 0, sizeof(defaultConfig_));
  strncpy(defaultConfig_.ssid, config.wifiSsid, sizeof(defaultConfig_.ssid) - 1);
  strncpy(defaultConfig_.password, config.wifiPassword, sizeof(defaultConfig_.password) - 1);
  defaultConfig_.valid = validSsid(defaultConfig_.ssid) && validPassword(defaultConfig_.password);
}

IPAddress WifiConfigService::portalIp() { return IPAddress(192, 168, 4, 1); }

void WifiConfigService::begin(uint32_t currentMillis) {
  WiFi.persistent(false);
  WiFi.setAutoReconnect(false);
  WiFi.mode(WIFI_STA);
  loadLocalConfig();
  restartConnectionWindow(currentMillis);
}

void WifiConfigService::restartConnectionWindow(uint32_t currentMillis) {
  connected_ = false;
  offlineStartedAt_ = currentMillis;
  startAttempt(localConfig_.valid, currentMillis);
}

WifiConfigService::Event WifiConfigService::service(uint32_t currentMillis) {
  if (reconnectScheduled_ && currentMillis - reconnectScheduledAt_ >= RECONNECT_DELAY_MILLIS) {
    reconnectScheduled_ = false;
    stopPortal();  // Explicitly submitted credentials start a new station attempt.
    restartConnectionWindow(currentMillis);
  }

  if (portalOpen_) {
    if (WiFi.softAPgetStationNum() > 0) {
      portalLastClientAt_ = currentMillis;
      portalHadClients_ = true;
    } else if (portalHadClients_) {
      portalHadClients_ = false;
      portalLastClientAt_ = currentMillis;
    } else if (currentMillis - portalLastClientAt_ >= PORTAL_IDLE_TIMEOUT_MILLIS) {
      stopPortal();
      pendingEvent_ = Event::PortalStopped;
      restartConnectionWindow(currentMillis);
    }
    return consumePendingEvent();
  }

  const bool wifiConnected = WiFi.status() == WL_CONNECTED;
  if (wifiConnected && !connected_) {
    if (configureDiscoveredAddress()) {
      connected_ = true;
      pendingEvent_ = attemptingLocal_ ? Event::LocalConnected : Event::DefaultConnected;
    } else {
      pendingEvent_ = Event::AddressDiscoveryFailed;
      startAttempt(localConfig_.valid ? !attemptingLocal_ : false, currentMillis);
    }
  } else if (!wifiConnected && connected_) {
    offlineStartedAt_ = currentMillis;
    connected_ = false;
    startAttempt(localConfig_.valid ? !attemptingLocal_ : false, currentMillis);
  }

  if (!connected_ && currentMillis - offlineStartedAt_ >= OFFLINE_TIMEOUT_MILLIS) {
    if (startPortal(currentMillis)) {
      pendingEvent_ = Event::PortalStarted;
    } else {
      pendingEvent_ = Event::PortalStartFailed;
      // Retry a failed AP start only after another full station window.
      restartConnectionWindow(currentMillis);
    }
  } else if (!wifiConnected && !connected_ &&
             currentMillis - attemptStartedAt_ >= CONNECT_TIMEOUT_MILLIS) {
    pendingEvent_ = attemptingLocal_ ? Event::LocalConnectionTimedOut : Event::DefaultConnectionTimedOut;
    startAttempt(localConfig_.valid ? !attemptingLocal_ : false, currentMillis);
  }
  return consumePendingEvent();
}

bool WifiConfigService::configureDiscoveredAddress() {
  const IPAddress leased = WiFi.localIP();
  const IPAddress gateway = WiFi.gatewayIP();
  const IPAddress mask = WiFi.subnetMask();
  // The accepted product network is precisely 192.168.x.0/24.
  if (leased[0] != 192 || leased[1] != 168 || leased[3] == 0 || leased[3] == 255 ||
      mask != IPAddress(255, 255, 255, 0) || gateway[0] != 192 || gateway[1] != 168 ||
      gateway[2] != leased[2] || gateway[3] == 0 || gateway[3] == 10 || gateway[3] == 255) {
    return false;
  }
  const IPAddress target(192, 168, leased[2], 10);
  IPAddress dns = WiFi.dnsIP(0);
  if (dns == IPAddress(0, 0, 0, 0)) dns = gateway;
  const IPAddress backupDns = WiFi.dnsIP(1);
  return WiFi.config(target, gateway, mask, dns, backupDns) && WiFi.localIP() == target;
}

bool WifiConfigService::startPortal(uint32_t currentMillis) {
  WiFi.disconnect(false, false);
  if (!WiFi.mode(WIFI_AP)) return false;
  const IPAddress ip = portalIp();
  if (!WiFi.softAPConfig(ip, ip, IPAddress(255, 255, 255, 0)) ||
      !WiFi.softAP(setupApSsid(deviceConfig_), setupApPassword(deviceConfig_))) {
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    return false;
  }
  connected_ = false;
  portalOpen_ = true;
  portalHadClients_ = false;
  portalLastClientAt_ = currentMillis;
  return true;
}

void WifiConfigService::stopPortal() {
  if (!portalOpen_) return;
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  portalOpen_ = false;
}

bool WifiConfigService::updateLocalConfig(const String &encodedSsid,
                                         const String &encodedPassword,
                                         const char *&errorMessage) {
  char ssid[SSID_SIZE];
  char password[PASSWORD_SIZE];
  if (!decodeConfigValue(encodedSsid, ssid, sizeof(ssid))) {
    errorMessage = "invalidWifiSsid";
    return false;
  }
  if (!decodeConfigValue(encodedPassword, password, sizeof(password))) {
    errorMessage = "invalidWifiPassword";
    return false;
  }
  return updateLocalCredentials(String(ssid), String(password), errorMessage);
}

bool WifiConfigService::updateLocalCredentials(const String &ssid, const String &password,
                                               const char *&errorMessage) {
  NetworkConfig candidate = {};
  if (ssid.length() > 32 || !validSsid(ssid.c_str())) {
    errorMessage = "invalidWifiSsid";
    return false;
  }
  if (password.length() > 64 || !validPassword(password.c_str())) {
    errorMessage = "invalidWifiPassword";
    return false;
  }
  // Reject embedded NULs before copying user-controlled String values.
  if (strlen(ssid.c_str()) != ssid.length() || strlen(password.c_str()) != password.length()) {
    errorMessage = "invalidWifiCredentials";
    return false;
  }
  ssid.toCharArray(candidate.ssid, sizeof(candidate.ssid));
  password.toCharArray(candidate.password, sizeof(candidate.password));
  candidate.valid = true;
  if (!saveLocalConfig(candidate)) {
    errorMessage = "wifiConfigSaveFailed";
    return false;
  }
  localConfig_ = candidate;
  errorMessage = nullptr;
  return true;
}

bool WifiConfigService::loadLocalConfig() {
  localConfig_.valid = false;
  Preferences preferences;
  if (!preferences.begin(WIFI_NAMESPACE, true)) return false;
  StoredCredentials stored = {};
  const size_t storedSize = preferences.getBytesLength(CREDENTIALS_KEY);
  if (storedSize == sizeof(stored)) {
    const size_t read = preferences.getBytes(CREDENTIALS_KEY, &stored, sizeof(stored));
    if (read == sizeof(stored) && stored.magic == CREDENTIALS_MAGIC &&
        validSsid(stored.ssid) && validPassword(stored.password)) {
      memcpy(localConfig_.ssid, stored.ssid, sizeof(stored.ssid));
      memcpy(localConfig_.password, stored.password, sizeof(stored.password));
      localConfig_.valid = true;
    }
  } else if (storedSize == 0 && preferences.getBool("local-valid", false)) {
    // Read old profiles, ignoring the old manually entered router IP.
    preferences.getString("ssid-local", localConfig_.ssid, sizeof(localConfig_.ssid));
    preferences.getString("password-local", localConfig_.password, sizeof(localConfig_.password));
    localConfig_.valid = validSsid(localConfig_.ssid) && validPassword(localConfig_.password);
  }
  preferences.end();
  return localConfig_.valid;
}

bool WifiConfigService::saveLocalConfig(const NetworkConfig &candidate) {
  StoredCredentials stored = {};
  stored.magic = CREDENTIALS_MAGIC;
  memcpy(stored.ssid, candidate.ssid, sizeof(stored.ssid));
  memcpy(stored.password, candidate.password, sizeof(stored.password));
  Preferences preferences;
  if (!preferences.begin(WIFI_NAMESPACE, false)) return false;
  const bool saved = preferences.putBytes(CREDENTIALS_KEY, &stored, sizeof(stored)) == sizeof(stored);
  preferences.end();
  return saved;
}

bool WifiConfigService::decodeConfigValue(const String &encoded, char *output, size_t size) const {
  const size_t length = encoded.length();
  if (length % 4 != 0) return false;
  size_t written = 0;
  for (size_t i = 0; i < length; i += 4) {
    const bool pad2 = encoded.c_str()[i + 2] == '=';
    const bool pad3 = encoded.c_str()[i + 3] == '=';
    if ((pad2 && !pad3) || ((pad2 || pad3) && i + 4 != length)) return false;
    int values[4];
    for (size_t j = 0; j < 4; ++j) {
      values[j] = (j >= 2 && encoded.c_str()[i + j] == '=') ? 0 : base64Value(encoded.c_str()[i + j]);
      if (values[j] < 0) return false;
    }
    if ((pad2 && (values[1] & 15)) || (pad3 && !pad2 && (values[2] & 3))) return false;
    const uint8_t bytes[] = {
      static_cast<uint8_t>((values[0] << 2) | (values[1] >> 4)),
      static_cast<uint8_t>((values[1] << 4) | (values[2] >> 2)),
      static_cast<uint8_t>((values[2] << 6) | values[3])
    };
    const size_t count = pad2 ? 1 : (pad3 ? 2 : 3);
    for (size_t j = 0; j < count; ++j) {
      if (bytes[j] == 0 || written + 1 >= size) return false;
      output[written++] = static_cast<char>(bytes[j]);
    }
  }
  output[written] = '\0';
  return true;
}

void WifiConfigService::startAttempt(bool useLocal, uint32_t currentMillis) {
  attemptingLocal_ = useLocal && localConfig_.valid;
  const NetworkConfig &config = attemptingLocal_ ? localConfig_ : defaultConfig_;
  WiFi.disconnect(false, false);
  WiFi.mode(WIFI_STA);
  // A zero local address restarts DHCP in the pinned ESP32 Core.
  const IPAddress zero(0, 0, 0, 0);
  WiFi.config(zero, zero, zero);
  if (config.valid) WiFi.begin(config.ssid, config.password);
  attemptStartedAt_ = currentMillis;
  if (pendingEvent_ == Event::None) {
    pendingEvent_ = attemptingLocal_ ? Event::LocalConnectStarted : Event::DefaultConnectStarted;
  }
}

void WifiConfigService::reconnectUsingLocalConfig(uint32_t currentMillis) {
  reconnectScheduledAt_ = currentMillis;
  reconnectScheduled_ = true;
}

bool WifiConfigService::isConnected() const { return connected_; }
bool WifiConfigService::isPortalOpen() const { return portalOpen_; }
const char *WifiConfigService::localSsid() const {
  return localConfig_.valid ? localConfig_.ssid : defaultConfig_.ssid;
}

WifiConfigService::Event WifiConfigService::consumePendingEvent() {
  const Event event = pendingEvent_;
  pendingEvent_ = Event::None;
  return event;
}
