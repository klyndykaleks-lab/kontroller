#ifndef ESP_LINK_H
#define ESP_LINK_H
#include <stdbool.h>
#include <stdint.h>
#define ESP_LINK_LINE_SIZE 128U
typedef struct {
    uint32_t txCompleted, txRetries, rxDrops, rxRestarts, txBackpressure;
} EspLinkStats;
bool EspLinkInit(void);
bool EspLinkSendLine(const char *line);
bool EspLinkTxIdle(void);
bool EspLinkReceiveLine(char line[ESP_LINK_LINE_SIZE]);
EspLinkStats EspLinkGetStats(void);
void espLinkTask(void *argument);
#endif
