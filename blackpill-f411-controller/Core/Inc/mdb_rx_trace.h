#ifndef MDB_RX_TRACE_H
#define MDB_RX_TRACE_H

#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>

/* One MDB-task producer, one manager-task consumer; no ISR writes this ring.
 * Drop new records on saturation, never overwrite a record being exported. */
#define MDB_RX_TRACE_CAPACITY 64U
typedef struct {
    uint32_t sequence, tick, cycles, transactionId;
    uint16_t word;
    uint8_t event, phase, length, cacheFlags;
    uint16_t reserved;
} MdbRxTraceRecord;
_Static_assert(sizeof(MdbRxTraceRecord) == 24U, "RX trace record size");
_Static_assert(ATOMIC_INT_LOCK_FREE == 2, "RX trace requires lock-free counters");
typedef struct {
    MdbRxTraceRecord records[MDB_RX_TRACE_CAPACITY];
    _Atomic uint32_t head, tail, dropped, polls;
    uint32_t sequence;
} MdbRxTrace;

static inline void MdbRxTracePush(MdbRxTrace *trace, MdbRxTraceRecord record) {
    uint32_t head = atomic_load_explicit(&trace->head, memory_order_relaxed);
    record.sequence = ++trace->sequence;
    if ((uint32_t)(head - atomic_load_explicit(&trace->tail,
            memory_order_acquire)) >= MDB_RX_TRACE_CAPACITY) {
        uint32_t dropped = atomic_load_explicit(&trace->dropped, memory_order_relaxed);
        atomic_store_explicit(&trace->dropped, dropped + 1U, memory_order_relaxed);
        return;
    }
    trace->records[head % MDB_RX_TRACE_CAPACITY] = record;
    atomic_store_explicit(&trace->head, head + 1U, memory_order_release);
}

static inline bool MdbRxTraceTake(MdbRxTrace *trace, MdbRxTraceRecord *record) {
    uint32_t tail = atomic_load_explicit(&trace->tail, memory_order_relaxed);
    if (tail == atomic_load_explicit(&trace->head, memory_order_acquire)) return false;
    *record = trace->records[tail % MDB_RX_TRACE_CAPACITY];
    atomic_store_explicit(&trace->tail, tail + 1U, memory_order_release);
    return true;
}

static inline void MdbRxTracePoll(MdbRxTrace *trace) {
    uint32_t polls = atomic_load_explicit(&trace->polls, memory_order_relaxed);
    atomic_store_explicit(&trace->polls, polls + 1U, memory_order_relaxed);
}
#endif
