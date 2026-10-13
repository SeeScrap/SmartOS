#pragma once
#include <stdbool.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// Stage 2.1 - claiming core 1.
//
// With CONFIG_FREERTOS_UNICORE the IDF startup never touches the second core:
// it stays stalled and held in reset for the whole life of the program. That
// makes it ours to take, which is the foundation the whole L4 plan rests on.
// ---------------------------------------------------------------------------

typedef void (*kcore1_entry_t)(void);

// Brings core 1 out of stall and reset and jumps it into `entry`.
// Blocks until core 1 reports that it is alive, or `timeout_ms` elapses.
// Returns false on timeout, which means the wake sequence did not take.
bool kcore1_start(kcore1_entry_t entry, uint32_t timeout_ms);

// True once core 1 has actually executed our code.
bool kcore1_is_up(void);
