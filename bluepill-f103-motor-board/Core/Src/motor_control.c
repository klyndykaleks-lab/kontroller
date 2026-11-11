#include "motor_control.h"
#include <string.h>

#define MOTOR_PAUSE_MS 3000U
#define MOTOR_DROP_WAIT_MS 5000U
#define SENSOR_START_GUARD_MS 15U

void MotorControlInit(MotorControl *motor) { memset(motor, 0, sizeof(*motor)); }

static void MotorFinish(MotorControl *motor, uint8_t status) {
    motor->outputActive = false;
    motor->phase = 0U;
    motor->status = status;
}

MotorCommandResult MotorControlStart(MotorControl *motor, uint8_t line, uint8_t turns, uint32_t now,
                                     bool sensorBlocked) {
    if (motor->status == MOTOR_BUSY) {
        return MOTOR_COMMAND_BUSY;
    }
    if (line < 1U || line > 10U || turns == 0U) {
        return MOTOR_COMMAND_INVALID;
    }
    if (sensorBlocked) {
        return MOTOR_COMMAND_SENSOR_BLOCKED;
    }
    /* A rejected command never overwrites the previous terminal result. */
    motor->counter++;
    motor->line = line;
    motor->turns = turns;
    motor->startedAt = motor->phaseStartedAt = now;
    motor->durationMs = (uint32_t)turns * 3000U + MOTOR_PAUSE_MS;
    motor->phase = 1U;
    motor->status = MOTOR_BUSY;
    motor->outputActive = true;
    return MOTOR_COMMAND_ACCEPTED;
}

void MotorControlStep(MotorControl *motor, uint32_t now, bool sensorEvent, uint32_t sensorTick,
                      bool sensorBlocked) {
    /* Output safety is checked even if a corrupt/stale status is terminal. */
    if (motor->status != MOTOR_BUSY) {
        motor->outputActive = false;
        motor->phase = 0U;
        return;
    }
    if (motor->line < 1U || motor->line > 10U || motor->turns == 0U || motor->phase < 1U ||
        motor->phase > 3U) {
        MotorFinish(motor, MOTOR_FAILED);
        return;
    }
    /* Honour an event captured before the deadline even if serviced later.
     * Keep the noise guard when energising the motor, not while it is off:
     * a real drop in the first pause milliseconds must prevent a retry.
     * Operation bounds still reject initial noise, stale and future edges. */
    const uint32_t sensorElapsed = (uint32_t)(sensorTick - motor->startedAt);
    const bool phaseGuardPassed = motor->phase == 2U ||
        ((uint32_t)(sensorTick - motor->phaseStartedAt) > SENSOR_START_GUARD_MS &&
         (uint32_t)(sensorTick - motor->phaseStartedAt) <= (uint32_t)(now - motor->phaseStartedAt));
    if (sensorEvent && sensorElapsed > SENSOR_START_GUARD_MS &&
        sensorElapsed < motor->durationMs + MOTOR_DROP_WAIT_MS &&
        sensorElapsed <= (uint32_t)(now - motor->startedAt) && phaseGuardPassed) {
        MotorFinish(motor, MOTOR_SUCCESS);
        return;
    }
    const uint32_t operationElapsed = (uint32_t)(now - motor->startedAt);
    if (operationElapsed >= motor->durationMs + MOTOR_DROP_WAIT_MS) {
        MotorFinish(motor, MOTOR_FAILED);
        return;
    }
    if (operationElapsed >= motor->durationMs) {
        /* Movement still ends at the original deadline. Reuse the existing
         * motor-off pause phase while waiting for a late drop; never retry. */
        motor->outputActive = false;
        motor->phase = 2U;
        return;
    }
    uint32_t elapsed = now - motor->phaseStartedAt;
    if (motor->phase == 1U && elapsed >= (uint32_t)motor->turns * 2000U) {
        motor->outputActive = false;
        motor->phase = 2U;
        motor->phaseStartedAt = now;
    } else if (motor->phase == 2U && elapsed >= MOTOR_PAUSE_MS) {
        /* Never energise a blocked tray while starting a retry. */
        if (sensorBlocked) {
            MotorFinish(motor, MOTOR_FAILED);
        } else {
            motor->phase = 3U;
            motor->phaseStartedAt = now;
            motor->outputActive = true;
        }
    }
}
