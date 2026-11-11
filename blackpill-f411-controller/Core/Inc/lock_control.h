#ifndef LOCK_CONTROL_H
#define LOCK_CONTROL_H
#include <stdbool.h>
#include <stdint.h>
typedef struct {
    uint32_t phaseAt, changedAt;
    uint8_t phase;
    bool raw, stable, output;
} LockControl;
/* No sleeps, allocation, I2C or RTOS calls. One bounded step per tick. */
static inline void LockControlStep(LockControl *s, uint32_t now, bool pressed, bool opened) {
    if (pressed != s->raw) { s->raw = pressed; s->changedAt = now; }
    bool edge = false;
    if ((uint32_t)(now - s->changedAt) >= 50U && s->stable != pressed) {
        s->stable = pressed; edge = pressed;
    }
    if (opened) { s->phase = 0; s->output = false; return; }
    /* Pressing again must never extend an energised pulse. */
    if (edge && s->phase == 0) { s->phase = 1; s->phaseAt = now; s->output = true; }
    if (s->phase == 1 && (uint32_t)(now - s->phaseAt) >= 1000U) {
        s->phase = 2; s->phaseAt = now; s->output = false;
    } else if (s->phase == 2 && (uint32_t)(now - s->phaseAt) >= 5000U) {
        s->phase = 1; s->phaseAt = now; s->output = true;
    }
}
#endif
