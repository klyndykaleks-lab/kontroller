#ifndef BUTTON_CONTROL_H
#define BUTTON_CONTROL_H
#include <stdbool.h>
#include <stdint.h>
typedef struct {
    uint16_t raw, stable;
    uint32_t changedAt, sequence;
    uint8_t key;
    bool armed;
} ButtonControl;
static inline void ButtonControlStep(ButtonControl *s, uint32_t now, uint16_t raw) {
    raw &= 0x3ffU;
    if (raw != s->raw) { s->raw = raw; s->changedAt = now; }
    if ((uint32_t)(now - s->changedAt) < 50U) return;
    s->stable = raw;
    if (!raw) { s->armed = true; return; }
    if (!s->armed) return;
    s->armed = false; /* Multiple keys require a full release, too. */
    if ((raw & (raw - 1U)) != 0U) return;
    s->key = 1; while ((raw >>= 1U) != 0U) ++s->key;
    ++s->sequence;
}
#endif
