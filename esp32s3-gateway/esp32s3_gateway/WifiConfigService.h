#pragma once

#include <Arduino.h>
#include <IPAddress.h>

#include "DeviceConfig.h"

class WifiConfigService {
 public:
  enum class Event : uint8_t {
    None,
    LocalConnectStarted,
    DefaultConnectStarted,
    LocalConnected,
    DefaultConnected,
    LocalConnectionTimedOut,
    DefaultConnectionTimedOut,
    AddressDiscoveryFailed,
    PortalStarted,
    PortalStopped,
    PortalStartFailed
  };

  static const uint32_t CONNECT_TIMEOUT_MILLIS = 10000;
  static const uint32_t OFFLINE_TIMEOUT_MILLIS = 120000;
  static const uint32_t PORTAL_IDLE_TIMEOUT_MILLIS = 180000;
  static const char *const PORTAL_SSID;
  static const char *const PORTAL_PASSWORD;

  explicit WifiConfigService(const DeviceConfig &config);

  void begin(uint32_t currentMillis);
  Event service(uint32_t currentMillis);
  bool updateLocalConfig(
    const String &encodedSsid,
    const String &encodedPassword,
    const char *&errorMessage
  );
  bool updateLocalCredentials(const String &ssid, const String &password,
                              const char *&errorMessage);
  void reconnectUsingLocalConfig(uint32_t currentMillis);
  bool isConnected() const;
  bool isPortalOpen() const;
  static IPAddress portalIp();
  const char *localSsid() const;

 private:
  static const size_t SSID_SIZE = 33;
  static const size_t PASSWORD_SIZE = 65;

  struct NetworkConfig {
    char ssid[SSID_SIZE];
    char password[PASSWORD_SIZE];
    bool valid;
  };

  bool loadLocalConfig();
  bool saveLocalConfig(const NetworkConfig &candidate);
  bool decodeConfigValue(
    const String &encoded,
    char *output,
    size_t outputSize
  ) const;
  bool configureDiscoveredAddress();
  bool startPortal(uint32_t currentMillis);
  void stopPortal();
  void restartConnectionWindow(uint32_t currentMillis);
  void startAttempt(bool useLocal, uint32_t currentMillis);
  Event consumePendingEvent();

  NetworkConfig localConfig_;
  NetworkConfig defaultConfig_;
  const DeviceConfig deviceConfig_;
  uint32_t attemptStartedAt_;
  uint32_t reconnectScheduledAt_;
  uint32_t offlineStartedAt_;
  uint32_t portalLastClientAt_;
  bool attemptingLocal_;
  bool connected_;
  bool reconnectScheduled_;
  bool portalOpen_;
  bool portalHadClients_;
  Event pendingEvent_;
};
