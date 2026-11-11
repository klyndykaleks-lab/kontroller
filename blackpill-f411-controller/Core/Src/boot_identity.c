#include "boot_identity.h"
#include <stddef.h>
uint32_t BootIdentityReserve(const BootIdentityStorage *s) {
    if (!s || !s->read || !s->program || !s->erase || !s->recordsPerPage) {
        return 0;
    }
    uint32_t highest = 0, occupied[2] = {0, 0};
    unsigned active = 0;
    bool dirty = false;
    for (unsigned page = 0; page < 2; page++) {
        for (uint32_t i = 0; i < s->recordsPerPage; i++) {
            uint32_t n = s->read(page, 2 * i), inv = s->read(page, 2 * i + 1);
            if (n != UINT32_MAX || inv != UINT32_MAX) {
                occupied[page] = i + 1;
                dirty = true;
            }
            if (n != 0 && n != UINT32_MAX && n > highest) {
                highest = n;
                active = page;
            }
        }
    }
    /* Completely damaged history cannot safely be treated as a factory blank. */
    if ((dirty && !highest) || highest >= UINT32_MAX - 1U) {
        return 0;
    }
    unsigned target = active;
    if (occupied[target] >= s->recordsPerPage) {
        target ^= 1U;
        if (!s->erase(target)) {
            return 0;
        }
        occupied[target] = 0;
    }
    uint32_t word = 2 * occupied[target], next = highest + 1U;
    if (!s->program(target, word, next) || !s->program(target, word + 1, ~next)) {
        return 0;
    }
    if (s->read(target, word) != next || s->read(target, word + 1) != ~next) {
        return 0;
    }
    return next;
}
