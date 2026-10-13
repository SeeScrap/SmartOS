#include "kernel/kheap.h"

#include <string.h>

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "kernel/klog.h"
#include "kernel/ktask.h"

#define KBLK_MAGIC 0x4B424C4Bu   // "KBLK"

// Blocks sit in address order and are walked, rather than threaded on a free
// list. Walking costs more per allocation but makes coalescing trivial and the
// dump honest - every byte in a region is accounted for by exactly one block.
// Exactly KHEAP_ALIGN bytes, so every payload inherits the region's alignment
// without any per-block padding arithmetic.
typedef struct kblk {
    struct kblk *next;    // next block by address, nullptr at the end
    uint32_t     size;    // payload bytes
    uint32_t     magic;
    uint16_t     free;
    uint16_t     owner;   // 0 = the kernel (stage 5.3c)
} kblk_t;

static_assert(sizeof(kblk_t) % KHEAP_ALIGN == 0,
              "block header must keep payloads aligned");

// The pools' bookkeeping is internal DRAM whatever pool it describes: the
// allocator must never fault on its own state with the cache off.
typedef struct {
    kblk_t  *head;
    uint8_t *base;
    size_t   size;
} kpool_t;

static DRAM_ATTR kpool_t s_pool[KHEAP_POOLS];

static inline IRAM_ATTR size_t align_up(size_t n)
{
    return (n + (KHEAP_ALIGN - 1)) & ~(size_t)(KHEAP_ALIGN - 1);
}

static inline IRAM_ATTR void *payload_of(kblk_t *b)
{
    return (void *)((uint8_t *)b + sizeof(kblk_t));
}

static inline IRAM_ATTR kblk_t *block_of(void *p)
{
    return (kblk_t *)((uint8_t *)p - sizeof(kblk_t));
}

// Where core 0 put the two regions. Written before core 1 starts, read by
// kheap_init on core 1.
static DRAM_ATTR uint8_t *s_region[KHEAP_POOLS];
static DRAM_ATTR size_t   s_region_size[KHEAP_POOLS];

static IRAM_ATTR void pool_init(kpool_t *pl, uint8_t *base, size_t size)
{
    pl->base = base;
    pl->size = size;
    pl->head = nullptr;
    if (!base || size < 2 * sizeof(kblk_t)) return;
    pl->head = (kblk_t *)base;
    pl->head->next  = nullptr;
    pl->head->size  = (uint32_t)(size - sizeof(kblk_t));
    pl->head->magic = KBLK_MAGIC;
    pl->head->free  = 1;
    pl->head->owner = 0;
}

// Core 0, in FreeRTOS context, BEFORE core 1 is started - never anywhere else.
//
// Stage 5.3c's first version took the regions inside kheap_init. But k_init,
// and so kheap_init, runs on core 1 - and heap_caps_malloc takes FreeRTOS and
// ESP-IDF locks that core 1 must never touch (the rule since stage 2.5). It
// broke FreeRTOS's critical-section state: core 0 then hung with interrupts
// masked and the interrupt watchdog reset the board, every boot. Bisected on
// hardware: a static pool still did it (the PSRAM pool was still taken from
// core 1), no PSRAM pool still did it (the internal one was), and only the old
// allocator - which never called ESP-IDF - did not.
// Stage 6.3: what is left for the kernel depends on what else has been linked
// in. Wi-Fi's own memory took the internal heap below 128 KB + what IDF needs
// to keep working, and the reservation - all or nothing until now - simply
// failed: no pool, no task stacks, nothing ran. So it asks for the full size,
// and settles for the largest block it can have while leaving IDF
// KHEAP_IDF_RESERVE, down to KHEAP_INTERNAL_MIN. Whatever it gets is what
// kheap_stats reports, and the boot line says so.
static size_t reserve_internal(void)
{
    const uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    size_t want = KHEAP_INTERNAL_SIZE;

    for (;;) {
        s_region[KHEAP_INTERNAL] = (uint8_t *)heap_caps_aligned_alloc(KHEAP_ALIGN, want, caps);
        if (s_region[KHEAP_INTERNAL]) return want;

        const size_t largest = heap_caps_get_largest_free_block(caps);
        size_t next = (largest > KHEAP_IDF_RESERVE) ? largest - KHEAP_IDF_RESERVE : 0;
        next &= ~(size_t)(KHEAP_ALIGN - 1);
        if (next < KHEAP_INTERNAL_MIN || next >= want) return 0;
        want = next;
    }
}

bool kheap_reserve(void)
{
    s_region_size[KHEAP_INTERNAL] = reserve_internal();
    s_region[KHEAP_PSRAM] = (uint8_t *)heap_caps_aligned_alloc(
        KHEAP_ALIGN, KHEAP_PSRAM_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_region_size[KHEAP_PSRAM] = s_region[KHEAP_PSRAM] ? KHEAP_PSRAM_SIZE : 0;
    // Laid out here too, not on core 1: the PSRAM pool's first header is in
    // PSRAM, and core 1 cannot yet be parked for a flash write at the point
    // k_init runs.
    for (int i = 0; i < KHEAP_POOLS; ++i) pool_init(&s_pool[i], s_region[i], s_region_size[i]);
    return s_region[KHEAP_INTERNAL] && s_region[KHEAP_PSRAM];
}

// Core 1, from k_init. Nothing left to do: kheap_reserve has done it all on
// core 0. Kept so k_init reads as it always has.
IRAM_ATTR void kheap_init(void)
{
}

static inline IRAM_ATTR kpool_t *pool_of(const void *p)
{
    for (int i = 0; i < KHEAP_POOLS; ++i) {
        const kpool_t *pl = &s_pool[i];
        if (pl->base && (const uint8_t *)p >= pl->base && (const uint8_t *)p < pl->base + pl->size) {
            return &s_pool[i];
        }
    }
    return nullptr;
}

IRAM_ATTR void *kmalloc_in(kheap_pool_t pool, size_t bytes, uint16_t owner)
{
    if (bytes == 0 || pool >= KHEAP_POOLS) return nullptr;
    size_t want = align_up(bytes);

    uint32_t ps = k_lock();
    for (kblk_t *b = s_pool[pool].head; b; b = b->next) {
        if (!b->free || b->size < want) continue;

        // Split only if the tail can hold a header plus a useful payload;
        // otherwise hand over the slack rather than create an unusable sliver.
        size_t leftover = b->size - want;
        if (leftover >= sizeof(kblk_t) + KHEAP_ALIGN) {
            kblk_t *tail = (kblk_t *)((uint8_t *)payload_of(b) + want);
            tail->next  = b->next;
            tail->size  = (uint32_t)(leftover - sizeof(kblk_t));
            tail->magic = KBLK_MAGIC;
            tail->free  = 1;
            tail->owner = 0;

            b->next = tail;
            b->size = (uint32_t)want;
        }

        b->free  = 0;
        b->owner = owner;
        k_unlock(ps);
        return payload_of(b);
    }

    k_unlock(ps);
    return nullptr;
}

IRAM_ATTR void *kmalloc(size_t bytes)
{
    return kmalloc_in(KHEAP_INTERNAL, bytes, 0);
}

IRAM_ATTR void *kcalloc(size_t n, size_t size)
{
    size_t total = n * size;
    void *p = kmalloc(total);
    if (p) memset(p, 0, total);
    return p;
}

// Frees b, which must be in pl and in use. Lock held.
static IRAM_ATTR void free_block(kpool_t *pl, kblk_t *b)
{
    b->free  = 1;
    b->owner = 0;

    // Merge forward as far as possible.
    while (b->next && b->next->free) {
        b->size += sizeof(kblk_t) + b->next->size;
        b->next  = b->next->next;
    }

    // Merge backward: find our predecessor by walking, then let it absorb us.
    kblk_t *prev = nullptr;
    for (kblk_t *c = pl->head; c && c != b; c = c->next) prev = c;
    if (prev && prev->free) {
        prev->size += sizeof(kblk_t) + b->size;
        prev->next  = b->next;
    }
}

IRAM_ATTR void kfree(void *p)
{
    if (!p) return;

    kpool_t *pl = pool_of(p);
    kblk_t  *b  = block_of(p);

    // Validate under the lock: checking first and then locking would let two
    // callers both pass the "not already free" test for the same block.
    uint32_t ps = k_lock();
    if (!pl || b->magic != KBLK_MAGIC) {
        k_unlock(ps);
        klog("[kheap] kfree(%p): not ours, or overrun\n", p);
        return;
    }
    if (b->free) {
        k_unlock(ps);
        klog("[kheap] kfree(%p): already free\n", p);
        return;
    }
    free_block(pl, b);
    k_unlock(ps);
}

IRAM_ATTR size_t kheap_free_owner(kheap_pool_t pool, uint16_t owner)
{
    if (pool >= KHEAP_POOLS || owner == 0) return 0;     // the kernel's are never swept
    size_t freed = 0;
    uint32_t ps = k_lock();
    kpool_t *pl = &s_pool[pool];
    // Restart after each free: merging can remove the block we would step to.
    for (bool again = true; again;) {
        again = false;
        for (kblk_t *b = pl->head; b; b = b->next) {
            if (!b->free && b->owner == owner) {
                freed += b->size;
                free_block(pl, b);
                again = true;
                break;
            }
        }
    }
    k_unlock(ps);
    return freed;
}

IRAM_ATTR size_t kheap_owner_bytes(kheap_pool_t pool, uint16_t owner)
{
    if (pool >= KHEAP_POOLS) return 0;
    size_t n = 0;
    uint32_t ps = k_lock();
    for (kblk_t *b = s_pool[pool].head; b; b = b->next) {
        if (!b->free && b->owner == owner) n += b->size;
    }
    k_unlock(ps);
    return n;
}

IRAM_ATTR void kheap_stats_pool(kheap_pool_t pool, kheap_stats_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (pool >= KHEAP_POOLS) return;

    uint32_t ps = k_lock();
    for (kblk_t *b = s_pool[pool].head; b; b = b->next) {
        out->blocks++;
        out->total += b->size;
        if (b->free) {
            out->free_blocks++;
            out->free_bytes += b->size;
            if (b->size > out->largest_free) out->largest_free = b->size;
        } else {
            out->used += b->size;
        }
    }
    k_unlock(ps);
}

IRAM_ATTR void kheap_stats(kheap_stats_t *out)
{
    kheap_stats_pool(KHEAP_INTERNAL, out);
}

IRAM_ATTR void kheap_dump(void)
{
    for (int i = 0; i < KHEAP_POOLS; ++i) {
        kheap_stats_t s;
        kheap_stats_pool((kheap_pool_t)i, &s);
        klog("[kheap] %s: %u/%u bytes used, largest free run %u, %u blocks (%u free)\n",
             i == KHEAP_INTERNAL ? "internal" : "psram", (unsigned)s.used, (unsigned)s.total,
             (unsigned)s.largest_free, (unsigned)s.blocks, (unsigned)s.free_blocks);
    }
}
