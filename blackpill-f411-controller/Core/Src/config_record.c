#include "config_record.h"
#include <string.h>
static uint32_t read32(const uint8_t *p) {
    return p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void write32(uint8_t *p, uint32_t n) {
    for (unsigned i = 0; i < 4; i++) {
        p[i] = (uint8_t)(n >> (8 * i));
    }
}
static uint32_t crc(const uint8_t *p, unsigned n) {
    uint32_t c = 0xFFFFFFFFU;
    while (n--) {
        c ^= *p++;
        for (unsigned i = 0; i < 8; i++) {
            c = (c >> 1) ^ ((0U - (c & 1U)) & 0xEDB88320U);
        }
    }
    return ~c;
}
static bool valid(const VendingConfig_t *c) {
    for (unsigned i = 0; i < 10; i++) {
        const ItemConfig_t *p = &c->items[i];
        if (p->price_x > 9 || p->price_y > 9 || p->price_z > 9 || p->price_p > 9 ||
            (p->selection_id > 999 && p->selection_id != 0xFFFFU)) {
            return false;
        }
    }
    return true;
}
bool ConfigRecordEncode(uint8_t out[CONFIG_RECORD_SIZE], const VendingConfig_t *c, uint32_t seq) {
    if (!out || !c || !valid(c)) {
        return false;
    }
    memset(out, 0, CONFIG_RECORD_SIZE);
    memcpy(out, "VMC2", 4);
    out[4] = 1;
    out[6] = 60;
    write32(out + 8, seq);
    for (unsigned i = 0; i < 10; i++) {
        const ItemConfig_t *p = &c->items[i];
        uint8_t *b = out + 12 + 6 * i;
        b[0] = p->price_x;
        b[1] = p->price_y;
        b[2] = p->price_z;
        b[3] = p->price_p;
        b[4] = (uint8_t)p->selection_id;
        b[5] = (uint8_t)(p->selection_id >> 8);
    }
    write32(out + 72, crc(out, 72));
    return true;
}
bool ConfigRecordDecode(const uint8_t in[CONFIG_RECORD_SIZE], VendingConfig_t *c, uint32_t *seq) {
    if (!in || !c || !seq || memcmp(in, "VMC2", 4) != 0 || in[4] != 1 || in[5] != 0 ||
        in[6] != 60 || in[7] != 0 || read32(in + 72) != crc(in, 72)) {
        return false;
    }
    VendingConfig_t decoded = {0};
    for (unsigned i = 0; i < 10; i++) {
        ItemConfig_t *p = &decoded.items[i];
        const uint8_t *b = in + 12 + 6 * i;
        p->price_x = b[0];
        p->price_y = b[1];
        p->price_z = b[2];
        p->price_p = b[3];
        p->selection_id = b[4] | ((uint16_t)b[5] << 8);
    }
    if (!valid(&decoded)) {
        return false;
    }
    *c = decoded;
    *seq = read32(in + 8);
    return true;
}
