#pragma once
#include <stdint.h>

// ---------------------------------------------------------------------------
// Stage 5.3b - the system's figures, readable from core 1.
//
// Free internal RAM, PSRAM and the filesystem's size can only be asked of IDF,
// and only core 0 may call IDF. So core 0's main loop asks, twice a second
// (the filesystem every 10 s: working out its usage reads flash, and every
// flash read parks core 1), and keeps the answers in DRAM. Anyone, on either
// core, gets a consistent copy: the writer bumps a sequence number before and
// after, and a reader that sees it change - or odd - reads again.
// ---------------------------------------------------------------------------

typedef struct {
    uint32_t ram_free, ram_total, ram_min, ram_largest;   // internal, bytes
    uint32_t psram_free, psram_total;
    uint32_t fs_total, fs_used;
    uint32_t flash_ops;                                   // SPI flash operations, ever
    uint32_t uptime_ms;
} sysinfo_t;

void sysinfo_poll(void);               // core 0 main loop
void sysinfo_get(sysinfo_t *out);      // anywhere
