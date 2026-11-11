#ifndef MOTOR_JOURNAL_H
#define MOTOR_JOURNAL_H
#include <stdbool.h>
#include <stdint.h>
typedef struct {
    uint32_t recordsPerPage;
    uint32_t (*read)(unsigned page, uint32_t word);
    bool (*program)(unsigned page, uint32_t word, uint32_t value);
    bool (*erase)(unsigned page);
} MotorJournalStorage;
typedef struct {
    uint32_t sequence, bootIdentity, operationId, directOperationId;
    uint8_t line, turns, counter, status;
    bool recovered, isDirect;
} MotorRecord;
typedef struct {
    MotorJournalStorage storage;
    MotorRecord record;
    uint32_t occupied[2];
    unsigned page;
    bool valid, corruptCommitted;
} MotorJournal;
void MotorJournalLoad(MotorJournal *journal, const MotorJournalStorage *storage);
bool MotorJournalSave(MotorJournal *journal, const MotorRecord *record);
bool MotorJournalIsNewOperation(const MotorJournal *journal, uint32_t boot, uint32_t operation);
#endif
