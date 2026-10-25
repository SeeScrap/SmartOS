#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "esp_attr.h"

#include "smartos_config.h"

// ---------------------------------------------------------------------------
// A real cross-core spinlock, built on the Xtensa S32C1I compare-and-swap.
//
// k_lock() in ktask.h only masks interrupts, which is enough to keep two tasks
// on the SAME core out of each other's way. It does nothing about the other
// core. The first place that mattered was the console: core 0 prints with
// printf while core 1 prints with esp_rom_printf, and both end up pushing bytes
// at the same USB-serial-JTAG FIFO. The ROM routine is not written for two
// cores at once.
//
// S32C1I stores a value only if the memory still equals SCOMPARE1, and hands
// back what was actually there - so a lock is taken when the value we displaced
// was zero.
//
// Every acquire is bounded. A core that dies holding the lock must not take the
// other one down with it - especially here, where the whole point of the other
// core is to be able to report that the first one crashed.
// ---------------------------------------------------------------------------

typedef volatile uint32_t k_spin_t;

// K_SPIN_TIMEOUT - how long k_spin_lock waits before giving up - is set in
// smartos_config.h, section 4, with the reason it is as generous as it is.

static inline IRAM_ATTR bool k_spin_try(k_spin_t *lock)
{
    uint32_t val     = 1;
    uint32_t compare = 0;
    __asm__ __volatile__(
        "wsr %2, scompare1 \n"
        "s32c1i %0, %1, 0  \n"
        : "+a"(val)
        : "a"(lock), "a"(compare)
        : "memory");
    return val == 0;              // we displaced a zero, so it is ours
}

// Returns false if the lock could not be taken in time. The caller should carry
// on anyway rather than hang - a garbled line beats a dead system.
// Atomically: if *p == expect, store desired. True if the store happened.
//
// The general form of k_spin_try, for state machines that hand a word between
// owners - the mailbox slots, first. S32C1I only works on internal SRAM, so
// *p must be DRAM, never PSRAM. The barrier on each side makes the swap a
// full fence: nothing written before it can be seen after the new state, and
// nothing read after it can come from before.
static inline IRAM_ATTR bool k_cas(volatile uint32_t *p, uint32_t expect,
                                   uint32_t desired)
{
    uint32_t val = desired;
    __asm__ __volatile__(
        "memw               \n"
        "wsr %2, scompare1  \n"
        "s32c1i %0, %1, 0   \n"
        "memw               \n"
        : "+a"(val)
        : "a"(p), "a"(expect)
        : "memory");
    return val == expect;         // S32C1I hands back what it found
}

static inline IRAM_ATTR bool k_spin_lock(k_spin_t *lock)
{
    for (uint32_t i = 0; i < K_SPIN_TIMEOUT; ++i) {
        if (k_spin_try(lock)) return true;
    }
    return false;
}

static inline IRAM_ATTR void k_spin_unlock(k_spin_t *lock)
{
    __asm__ __volatile__("memw" ::: "memory");
    *lock = 0;
    __asm__ __volatile__("memw" ::: "memory");
}

// Unbounded acquire, for locks that must not be given up on.
//
// The console lock can afford to time out - a garbled line beats a dead system.
// The scheduler lock cannot: proceeding without it would let two cores pick the
// same task. So this one spins until it wins, and leaves a breadcrumb if it has
// been spinning long enough that something is clearly wrong.
extern "C" volatile uint32_t k_spin_stuck;

static inline IRAM_ATTR void k_spin_lock_hard(k_spin_t *lock)
{
    uint32_t spins = 0;
    while (!k_spin_try(lock)) {
        if (++spins == K_SPIN_TIMEOUT) {     // the same patience as k_spin_lock
            k_spin_stuck = (uint32_t)(uintptr_t)lock;
        }
    }
}

// The console lock, shared by klog() on core 1 and logf() on core 0.
extern "C" k_spin_t k_console_lock;

// The scheduler lock. Guards the task table, the per-CPU run state and the
// heap. Held across a context switch on purpose: a task whose registers are
// still half-saved must not be picked up by the other core, so the lock is
// released by whichever context is switched IN, not by the one going out.
extern "C" k_spin_t k_sched_lock;
