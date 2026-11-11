#include "motor_journal.h"
#include "../../../common/wire_codec.h"
#include <string.h>
#define RECORD_MAGIC 0x4D4F5432U
#define RECORD_COMMIT 0xC0DEC0DEU
static bool decode(const uint8_t b[32], MotorRecord *r) {
    if (WireRead32(b) != RECORD_MAGIC || WireRead32(b + 28) != RECORD_COMMIT ||
        WireRead32(b + 24) != WireCrc32(b, 24)) {
        return false;
    }
    r->sequence = WireRead32(b + 4);
    r->bootIdentity = WireRead32(b + 8);
    r->operationId = WireRead32(b + 12);
    r->line = b[16];
    r->turns = b[17];
    r->counter = b[18];
    r->status = b[19] & 0x3FU;
    r->recovered = (b[19] & 0x80U) != 0;
    r->isDirect = (b[19] & 0x40U) != 0;
    r->directOperationId = WireRead32(b + 20);
    return r->sequence && r->bootIdentity && r->operationId && r->line >= 1 && r->line <= 10 &&
           r->turns && r->status >= 1 && r->status <= 3 &&
           (!r->isDirect || r->directOperationId != 0);
}
void MotorJournalLoad(MotorJournal *j, const MotorJournalStorage *s) {
    memset(j, 0, sizeof(*j));
    j->storage = *s;
    for (unsigned p = 0; p < 2; p++) {
        for (uint32_t i = 0; i < s->recordsPerPage; i++) {
            uint8_t b[32];
            bool occupied = false;
            for (unsigned w = 0; w < 8; w++) {
                uint32_t n = s->read(p, 8 * i + w);
                WireWrite32(b + 4 * w, n);
                if (n != UINT32_MAX) {
                    occupied = true;
                }
            }
            if (occupied) {
                j->occupied[p] = i + 1;
            }
            MotorRecord r;
            bool decoded = decode(b, &r);
            if (!decoded && WireRead32(b) == RECORD_MAGIC && WireRead32(b + 28) == RECORD_COMMIT)
                j->corruptCommitted = true;
            if (decoded && (!j->valid || r.sequence > j->record.sequence)) {
                j->record = r;
                j->valid = true;
                j->page = p;
            }
        }
    }
}
bool MotorJournalIsNewOperation(const MotorJournal *j, uint32_t boot, uint32_t op) {
    if (!boot || !op) {
        return false;
    }
    return !j->valid || boot > j->record.bootIdentity ||
           (boot == j->record.bootIdentity && op > j->record.operationId);
}
bool MotorJournalSave(MotorJournal *j, const MotorRecord *record) {
    if (j->valid && j->record.sequence == UINT32_MAX) {
        return false;
    }
    uint8_t b[32] = {0};
    MotorRecord checked;
    WireWrite32(b, RECORD_MAGIC);
    WireWrite32(b + 4, j->valid ? j->record.sequence + 1U : 1U);
    WireWrite32(b + 8, record->bootIdentity);
    WireWrite32(b + 12, record->operationId);
    b[16] = record->line;
    b[17] = record->turns;
    b[18] = record->counter;
    b[19] = record->status | (record->recovered ? 0x80U : 0U) | (record->isDirect ? 0x40U : 0U);
    WireWrite32(b + 20, record->directOperationId);
    WireWrite32(b + 24, WireCrc32(b, 24));
    WireWrite32(b + 28, RECORD_COMMIT);
    if (!decode(b, &checked)) {
        return false;
    }
    unsigned p = j->page;
    if (j->occupied[p] >= j->storage.recordsPerPage) {
        p ^= 1U;
        if (!j->storage.erase(p)) {
            return false;
        }
        j->occupied[p] = 0;
    }
    uint32_t first = 8 * j->occupied[p]++;
    for (unsigned w = 0; w < 8; w++) {
        uint32_t n = WireRead32(b + 4 * w);
        if (!j->storage.program(p, first + w, n) || j->storage.read(p, first + w) != n) {
            return false;
        }
    }
    j->record = checked;
    j->valid = true;
    j->page = p;
    return true;
}
