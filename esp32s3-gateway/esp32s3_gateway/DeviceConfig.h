#pragma once

#include <stddef.h>
#include <stdint.h>

const size_t DEVICE_CONFIG_BLOB_SIZE = 512;
const uint16_t DEVICE_CONFIG_SCHEMA_VERSION = 1;

struct __attribute__((packed)) DeviceConfig {
  uint8_t magic[16];
  uint16_t schemaVersion;
  uint16_t structureSize;
  uint32_t crc32;
  char pointId[13];
  char wifiSsid[33];
  char wifiPassword[65];
  uint8_t controllerIp[4];
  uint8_t gatewayIp[4];
  uint8_t subnetMask[4];
  uint8_t dnsIp[4];
  int8_t latchGpio;
  int8_t doorSensorGpio;
  int8_t extraOutputGpio;
  int8_t buzzerGpio;
  int8_t temperatureGpio;
  uint8_t defaultLatchType;
  uint8_t defaultDoorSensorType;
  uint8_t temperatureParasitePower;
  uint8_t alignmentPadding;
  uint32_t latchPulseMilliseconds;
  uint8_t reserved[348];
};

static_assert(
  sizeof(DeviceConfig) == DEVICE_CONFIG_BLOB_SIZE,
  "DeviceConfig must remain a fixed-size patchable blob"
);
static_assert(offsetof(DeviceConfig, crc32) == 20, "DeviceConfig CRC offset changed");
static_assert(offsetof(DeviceConfig, pointId) == 24, "DeviceConfig payload offset changed");
static_assert(
  offsetof(DeviceConfig, latchPulseMilliseconds) == 160,
  "DeviceConfig pulse offset changed"
);
static_assert(
  offsetof(DeviceConfig, reserved) == 164,
  "DeviceConfig reserved offset changed"
);

bool isDeviceConfigValid(const DeviceConfig &config);
bool deviceConfigBlobValid();
const DeviceConfig &deviceConfig();
