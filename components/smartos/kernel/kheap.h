#pragma once
#include <stddef.h>
#include <stdint.h>

#include "smartos_config.h"

// ---------------------------------------------------------------------------
// Stage 2.5 - the kernel's own allocator. Two pools since stage 5.3c.
//
// heap_caps_malloc() takes a FreeRTOS lock that core 1 has no business
// touching, so the kernel manages memory of its own: first fit, boundary tags,
// coalescing on free. Nothing clever - the point is ownership and
// predictability, not peak allocator performance. The code is IRAM so it keeps
// working while the flash cache is off.
//
// TWO POOLS
//
//   KHEAP_INTERNAL  128 KB of internal RAM. Task stacks, app state, anything
//                   the kernel itself touches - and anything that must work
//                   with the flash cache off, or be the target of an atomic
//                   (S32C1I does not work on PSRAM). kmalloc() is this pool.
//   KHEAP_PSRAM     4 MB of PSRAM, for apps' large data: sample buffers, maps,
//                   images (app_alloc in os/app.h). Not usable while core 0
//                   writes flash - core 1 is parked then anyway - and never
//                   for stacks: core 1 parks on the interrupted task's stack.
//
// Until 5.3c this was one 64 KB array fixed at link time, sized for the stage
// 2 demos and never revisited. Now core 0 takes both regions from ESP-IDF at
// boot (kheap_reserve), before core 1 starts, and core 1 lays them out
// (kheap_init); from then on they are the kernel's alone.
//
// OWNERS
//
// Every block carries an owner tag: 0 for the kernel, or whatever the caller
// passes. kheap_free_owner frees everything a tag holds in one go - how an
// app's PSRAM is returned when it ends, whether it closed or was killed.
//
// heap_caps_malloc() ของ IDF จับล็อกของ FreeRTOS ซึ่งคอร์ 1 ไม่ควรไปแตะ
// เคอร์เนลจึงจัดการหน่วยความจำเอง: first fit, boundary tag, รวมบล็อกตอน free
// ไม่มีอะไรฉลาด เพราะสิ่งที่ต้องการคือความเป็นเจ้าของและความคาดเดาได้
// ไม่ใช่ความเร็วสูงสุดของตัวจัดสรร โค้ดอยู่ใน IRAM จึงยังทำงานได้ตอนแคชแฟลชปิด
//
// สองพูล: KHEAP_INTERNAL (RAM ภายใน 128 KB) สำหรับ stack ของงาน สถานะแอป
// และทุกอย่างที่ต้องทำงานได้ตอนแคชปิด หรือเป็นเป้าของคำสั่ง atomic
// กับ KHEAP_PSRAM (4 MB) สำหรับข้อมูลก้อนใหญ่ของแอป และ **ห้ามใช้ทำ stack**
// ---------------------------------------------------------------------------

// The two pool sizes, what the internal one settles for, what is left to
// ESP-IDF, and the alignment are set in smartos_config.h, section 2.

typedef enum { KHEAP_INTERNAL = 0, KHEAP_PSRAM, KHEAP_POOLS } kheap_pool_t;

bool   kheap_reserve(void);              // core 0, at boot, BEFORE core 1 starts
void   kheap_init(void);                 // core 1, from k_init: no ESP-IDF calls
void  *kmalloc(size_t bytes);            // internal pool, owner 0
void  *kcalloc(size_t n, size_t size);
void  *kmalloc_in(kheap_pool_t pool, size_t bytes, uint16_t owner);
void   kfree(void *p);                   // either pool
size_t kheap_free_owner(kheap_pool_t pool, uint16_t owner);   // bytes returned
size_t kheap_owner_bytes(kheap_pool_t pool, uint16_t owner);

typedef struct {
    size_t total;        // bytes of payload the region can hold
    size_t used;
    size_t free_bytes;
    size_t largest_free; // the biggest single allocation still possible
    size_t blocks;
    size_t free_blocks;
} kheap_stats_t;

void kheap_stats(kheap_stats_t *out);                        // the internal pool
void kheap_stats_pool(kheap_pool_t pool, kheap_stats_t *out);
void kheap_dump(void);

static_assert(KHEAP_INTERNAL_MIN <= KHEAP_INTERNAL_SIZE,
              "the internal pool cannot settle for more than it asked for "
              "- see section 2 of smartos_config.h");
static_assert(KHEAP_ALIGN >= 16 && (KHEAP_ALIGN & (KHEAP_ALIGN - 1)) == 0,
              "KHEAP_ALIGN must be a power of two, and Xtensa stacks want 16");
