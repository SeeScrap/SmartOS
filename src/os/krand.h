#pragma once
#include <stdint.h>

// ---------------------------------------------------------------------------
// Stage 4.3c - random numbers for apps. Games, mostly.
//
// xorshift32: fast, small, and good enough to place food on a snake board -
// not for anything that must be unpredictable to an adversary. Seeded from
// the hardware RNG at start-up, or explicitly with k_srand() when a test needs
// the same sequence twice.
//
// One shared state for every caller. Two apps drawing numbers at the same
// instant can interleave their updates and see related values; for games that
// is harmless, and it keeps this free of locks.
// ---------------------------------------------------------------------------

void     k_srand(uint32_t seed);          // 0 re-seeds from the hardware
uint32_t k_rand(void);
uint32_t k_rand_below(uint32_t n);        // uniform in [0, n); 0 if n is 0
