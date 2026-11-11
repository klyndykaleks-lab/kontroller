#ifndef BOOT_IDENTITY_H
#define BOOT_IDENTITY_H
#include <stdbool.h>
#include <stdint.h>
typedef struct {
    uint32_t recordsPerPage;
    uint32_t (*read)(unsigned page, uint32_t word);
    bool (*program)(unsigned page, uint32_t word, uint32_t value);
    bool (*erase)(unsigned page);
} BootIdentityStorage;
/* Reserves a never-before-published generation. Zero means fail closed. */
uint32_t BootIdentityReserve(const BootIdentityStorage *storage);
#endif
