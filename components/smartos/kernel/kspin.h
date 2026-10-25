#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "arch/xtensa/karch_atomic.h"
#include "port/kport.h"

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
// The compare-and-swap itself is arch/xtensa/karch_atomic.h; what is here is
// everything above it - which lock, how long to wait, and what to do when the
// wait is clearly hopeless.
//
// Every acquire is bounded. A core that dies holding the lock must not take the
// other one down with it - especially here, where the whole point of the other
// core is to be able to report that the first one crashed.
//
// k_lock() ใน ktask.h แค่ปิดอินเทอร์รัปต์ ซึ่งพอสำหรับกันงานสองงานบนคอร์เดียวกัน
// แต่ไม่ได้ทำอะไรกับคอร์อีกตัวเลย ที่แรกที่เรื่องนี้สำคัญคือคอนโซล: คอร์ 0 พิมพ์ด้วย
// printf ส่วนคอร์ 1 พิมพ์ด้วย esp_rom_printf แล้วทั้งคู่ไปลง FIFO เดียวกัน
//
// ทุกการจับล็อกมีเพดานเวลา คอร์ที่ตายขณะถือล็อกต้องไม่ลากอีกคอร์ไปด้วย
// โดยเฉพาะที่นี่ ซึ่งเหตุผลทั้งหมดของการมีอีกคอร์คือการรายงานว่าคอร์แรกพัง
// ---------------------------------------------------------------------------

// K_SPIN_TIMEOUT - how long k_spin_lock waits before giving up - is set in
// smartos_config.h, section 4, with the reason it is as generous as it is.

// The three the architecture provides, under the kernel's own names.
static inline K_IRAM bool k_spin_try(k_spin_t *lock) { return karch_spin_try(lock); }
static inline K_IRAM void k_spin_unlock(k_spin_t *lock) { karch_spin_unlock(lock); }
static inline K_IRAM bool k_cas(volatile uint32_t *p, uint32_t expect, uint32_t desired)
{
    return karch_cas(p, expect, desired);
}

static inline K_IRAM bool k_spin_lock(k_spin_t *lock)
{
    for (uint32_t i = 0; i < K_SPIN_TIMEOUT; ++i) {
        if (k_spin_try(lock)) return true;
    }
    return false;
}

// Unbounded acquire, for locks that must not be given up on.
//
// The console lock can afford to time out - a garbled line beats a dead system.
// The scheduler lock cannot: proceeding without it would let two cores pick the
// same task. So this one spins until it wins, and leaves a breadcrumb if it has
// been spinning long enough that something is clearly wrong.
extern "C" volatile uint32_t k_spin_stuck;

static inline K_IRAM void k_spin_lock_hard(k_spin_t *lock)
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
