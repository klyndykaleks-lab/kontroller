#ifndef MOTOR_CONTROL_H
#define MOTOR_CONTROL_H

#include <stdbool.h>
#include <stdint.h>

enum { MOTOR_IDLE = 0, MOTOR_BUSY = 1, MOTOR_SUCCESS = 2, MOTOR_FAILED = 3 };
typedef enum {
    MOTOR_COMMAND_ACCEPTED,
    MOTOR_COMMAND_BUSY,
    MOTOR_COMMAND_INVALID,
    MOTOR_COMMAND_SENSOR_BLOCKED
} MotorCommandResult;

/* Main loop is the only writer. IRQ callbacks only latch input events. */
typedef struct {
    uint8_t status, phase, line, turns, counter;
    bool outputActive;
    uint32_t startedAt, phaseStartedAt, durationMs;
} MotorControl;

void MotorControlInit(MotorControl *motor);
MotorCommandResult MotorControlStart(MotorControl *motor, uint8_t line, uint8_t turns, uint32_t now,
                                     bool sensorBlocked);
void MotorControlStep(MotorControl *motor, uint32_t now, bool sensorEvent, uint32_t sensorTick,
                      bool sensorBlocked);

#endif
