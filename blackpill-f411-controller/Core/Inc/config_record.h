#ifndef CONFIG_RECORD_H
#define CONFIG_RECORD_H
#include "configuration.h"
#include <stdbool.h>
#define CONFIG_RECORD_SIZE 76U
bool ConfigRecordEncode(uint8_t record[CONFIG_RECORD_SIZE], const VendingConfig_t *config,
                        uint32_t sequence);
bool ConfigRecordDecode(const uint8_t record[CONFIG_RECORD_SIZE], VendingConfig_t *config,
                        uint32_t *sequence);
#endif
