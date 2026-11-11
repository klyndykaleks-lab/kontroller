#ifndef WIRE_CODEC_H
#define WIRE_CODEC_H
#include <stddef.h>
#include <stdint.h>
static inline uint32_t WireRead32(const uint8_t *p) {
    return p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline void WireWrite32(uint8_t *p, uint32_t n) {
    for (unsigned i = 0; i < 4; i++) {
        p[i] = (uint8_t)(n >> (8 * i));
    }
}
static inline uint32_t WireCrc32(const uint8_t *p, size_t n) {
    uint32_t c = UINT32_MAX;
    while (n--) {
        c ^= *p++;
        for (unsigned i = 0; i < 8; i++) {
            c = (c >> 1) ^ ((0U - (c & 1U)) & 0xEDB88320U);
        }
    }
    return ~c;
}
#endif
