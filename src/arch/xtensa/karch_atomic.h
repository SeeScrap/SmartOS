#pragma once

// ===========================================================================
// Xtensa: compare-and-swap, and the memory ordering around it.
// Xtensa: การเปรียบเทียบ-แล้ว-สลับค่า และลำดับหน่วยความจำรอบๆ มัน
// ===========================================================================
//
// Three instructions' worth of architecture, and the only three the kernel's
// locks are built on. Everything above them - which lock, how long to wait,
// what to do when the wait is clearly hopeless - is kernel policy and lives in
// kernel/kspin.h.
//
// S32C1I IS THE WHOLE STORY. It writes a register to memory only if what was
// already there matches SCOMPARE1, and hands back what it found either way.
// That one instruction is what makes a lock possible between two cores here.
//
// IT DOES NOT WORK ON PSRAM. Not slowly - at all. Every lock, every mailbox
// slot and every heap header the kernel touches with these has to be in
// internal SRAM, which is why kernel/kheap.h has two pools and says so, and
// why kernel/kmbox.h puts its slots in DRAM.
//
// S32C1I ใช้กับ PSRAM ไม่ได้ ไม่ใช่ช้า แต่ใช้ไม่ได้เลย ทุกล็อก ทุกช่องกล่อง
// ข้อความ และทุกหัวบล็อกของ heap ที่เคอร์เนลแตะด้วยคำสั่งนี้ต้องอยู่ใน SRAM
// ภายในเท่านั้น
//
// On a port to another architecture, this file is replaced and nothing above
// it changes.
// ---------------------------------------------------------------------------

#include <stdbool.h>
#include <stdint.h>

#include "port/kport.h"

// A lock is one word. Zero is free.
typedef volatile uint32_t k_spin_t;

// Takes the lock if it was free. One attempt; no waiting.
static inline K_IRAM bool karch_spin_try(k_spin_t *lock)
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

// The general form, with a fence either side: the mailbox hands ownership of a
// slot over with this, and both cores have to see the slot's contents in the
// right order relative to the hand-over.
static inline K_IRAM bool karch_cas(volatile uint32_t *p, uint32_t expect,
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

// Releasing needs no atomic - only the owner writes it - but it does need the
// fences, so that what was done under the lock is visible before the lock is.
static inline K_IRAM void karch_spin_unlock(k_spin_t *lock)
{
    __asm__ __volatile__("memw" ::: "memory");
    *lock = 0;
    __asm__ __volatile__("memw" ::: "memory");
}
