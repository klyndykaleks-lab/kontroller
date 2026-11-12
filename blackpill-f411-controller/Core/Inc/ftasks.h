
#include <stdbool.h>
#include "configuration.h"

void managerTask(void *argument);
void keypadTask(void *argument);
void mdbTask(void *argument);
void displayTask(void *argument);
void loggerTask(void *argument);
void climatTask(void *argument);
void latchTask(void *argument);
void UsbCdcProcessLine(const char *line);
bool QueueManagerEvent(const event_t *event);
void F411_MdbHeartbeat(void);
void F411_WatchdogServiceFromManager(void);
uint8_t F411_GetResetFlags(void);
const char *F411_GetResetReason(void);
