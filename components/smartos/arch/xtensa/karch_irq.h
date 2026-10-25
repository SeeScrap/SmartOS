#pragma once

// ===========================================================================
// Xtensa: shutting interrupts out, and letting them back in.
// Xtensa: การปิดกั้นอินเทอร์รัปต์ และการเปิดให้กลับเข้ามา
// ===========================================================================
//
// RSIL raises the processor's interrupt level and hands back the old PS in one
// instruction, which is what makes it safe: there is no window between reading
// the old level and setting the new one. Restoring writes PS back and RSYNC
// waits for it to take effect before the next instruction runs.
//
// LEVEL 3, NOT 15. The kernel's own tick and park interrupts are level 1, so
// level 3 is already more than enough to keep them out. Going higher would
// also mask the debug and NMI levels, which nothing here wants to hide.
//
// ระดับ 3 ไม่ใช่ 15 เพราะ tick กับ park ของเคอร์เนลเป็นระดับ 1 อยู่แล้ว
// การขึ้นไปสูงกว่านี้จะบังระดับ debug และ NMI ไปด้วย ซึ่งไม่มีใครอยากซ่อน
//
// These two say nothing about WHICH lock the scheduler takes - that is
// k_lock() in kernel/ktask.h, which pairs them with the scheduler's spinlock.
// On a port to another architecture, this file is replaced and nothing above
// it changes.
// ---------------------------------------------------------------------------

#include <stdint.h>

#include "port/kport.h"

// The level the kernel shuts out. Its own interrupts are level 1. Not a
// setting: raising it would mask debug and NMI, lowering it would let the tick
// through, so it is not in smartos_config.h.
#define KARCH_IRQ_LEVEL 3

// RSIL takes the level as part of the instruction, so it has to be spelled
// into the string rather than passed. These two are the usual two-step that
// turns a macro into its text.
#define KARCH_STR_(x) #x
#define KARCH_STR(x)  KARCH_STR_(x)

// Raises the interrupt level, returning the PS to hand back later.
static inline K_IRAM uint32_t karch_irq_disable(void)
{
    uint32_t ps;
    __asm__ __volatile__("rsil %0, " KARCH_STR(KARCH_IRQ_LEVEL) : "=a"(ps) :: "memory");
    return ps;
}

// Puts PS back exactly as it was, and waits for that to be true.
static inline K_IRAM void karch_irq_restore(uint32_t ps)
{
    __asm__ __volatile__("wsr %0, ps; rsync" :: "a"(ps) : "memory");
}
