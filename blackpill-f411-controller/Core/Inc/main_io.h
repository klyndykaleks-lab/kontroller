#ifndef MAIN_IO_H
#define MAIN_IO_H
#include <stdbool.h>
#include <stdint.h>
void MainIoInit(void);
void MainIoStopForMaintenance(void);
void MainIoTick(void);
bool MainDropBlocked(void);
void MainDropArm(uint32_t boot, uint32_t operation);
void MainDropDisarm(void);
bool MainDropDetected(uint32_t boot, uint32_t operation);
#endif
