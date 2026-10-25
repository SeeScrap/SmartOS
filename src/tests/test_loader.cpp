#include "tests/tests.h"

#ifdef SMARTOS_TEST_LOADER

#include <stdio.h>
#include <string.h>

#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include "esp_psram.h"
#include "soc/soc.h"

#include "kernel/klog.h"
#include "board/log.h"
#include "tests/harness.h"

// ---------------------------------------------------------------------------
// Stage 7, step a: can this board run code it was not built with?
//
// Everything stage 7 wants - installing an app as a file, without reflashing
// the operating system - rests on three things being true of THIS chip in
// THIS configuration. They are assumptions until they are measured, and the
// measurement is cheap, so it happens before anything is built on them.
//
//   1. Can the processor fetch instructions from memory the loader allocated?
//      Internal RAM can be marked executable. PSRAM on the ESP32-S3 is mapped
//      on the data bus; whether the instruction bus reaches it depends on the
//      build, and an app of any size will not fit in internal RAM.
//   2. Does code copied there actually run, caches and all?
//   3. Can that code call back into the operating system?
//
// If 1 and 2 fail for PSRAM, stage 7 is still possible but every installed
// app has to fit in internal RAM, which is a different design and a decision
// for the user rather than for this file.
//
// ขั้นนี้คือด่านตัดสินของ stage 7 ทุกอย่างที่เหลือวางอยู่บนสามข้อนี้ และถ้า
// ข้อ 1 กับ 2 ไม่ผ่านสำหรับ PSRAM แอปที่ติดตั้งจะต้องพอดีกับ RAM ภายใน
// ซึ่งเป็นการออกแบบคนละแบบ และเป็นการตัดสินใจของผู้ใช้ ไม่ใช่ของไฟล์นี้
// ---------------------------------------------------------------------------

// The function that gets copied.
//
// LITERAL-FREE AND CALL-FREE, ON PURPOSE. The first version of this returned
// `a * 1000 + b`, and the board died with "Cache disabled but cached memory
// region accessed". On Xtensa a constant that big goes in a literal pool
// beside the function and is read with l32r, which is PC-relative - so the
// moment the code is copied somewhere else, that read points at whatever
// happens to be there. The same is true of the address of a string, or of any
// direct call.
//
// That is the relocation problem, and it is real: it is why loading a program
// is a loader's job and not a memcpy. This file is not trying to solve it. It
// is answering the question underneath it - whether the processor will fetch
// instructions from memory we allocated at all - and for that the probe has
// to be something a copy does not break.
//
// โค้ดนี้จงใจไม่มีค่าคงที่ใน literal pool และไม่เรียกฟังก์ชันใด เพราะทั้งสอง
// อย่างอ้างตำแหน่งแบบอิง PC พอสำเนาไปที่อื่นก็ชี้ผิดทันที นั่นคือปัญหา
// relocation ตัวจริง ซึ่งเป็นงานของตัวโหลด ไฟล์นี้ตอบคำถามที่อยู่ใต้มันลงไป
// อีกชั้น: ตัวประมวลผลยอมดึงคำสั่งจากหน่วยความจำที่เราจองมาหรือเปล่า
__attribute__((noinline, aligned(4)))
static int probe_add(int a, int b)
{
    return (a << 8) + b;        // small immediates: no literal pool
}


// How big is a function? There is no portable answer, so take the distance to
// the next function and round up - both are aligned to 4, and 256 bytes is
// far more than either of these compiles to at -O2.
#define PROBE_BYTES 64

static const char *region_of(const void *p)
{
    const uintptr_t a = (uintptr_t)p;
    if (a >= 0x3FC80000 && a < 0x3FD00000) return "internal DRAM";
    if (a >= 0x40370000 && a < 0x403E0000) return "internal IRAM";
    if (a >= 0x3C000000 && a < 0x3E000000) return "PSRAM (data bus)";
    if (a >= 0x42000000 && a < 0x44000000) return "flash (instruction bus)";
    return "somewhere else";
}

// Copies the probe into `dst` and calls it there. Returns what it answered,
// or -1 if it was never reached.
// True for addresses that live behind the cache - PSRAM and flash. Internal
// RAM is not cached on this chip at all.
static bool is_external(const void *p)
{
    const uintptr_t a = (uintptr_t)p;
    return (a >= 0x3C000000 && a < 0x3E000000) || (a >= 0x42000000 && a < 0x44000000);
}

static void sync_code(void *dst, size_t bytes)
{
    // ONLY FOR EXTERNAL MEMORY. The first version of this called
    // esp_cache_msync on an internal RAM pointer and the board died with
    // "Cache error / MMU entry fault", EXCVADDR pointing straight at the
    // buffer: that call is for memory the cache fronts, and internal RAM on
    // this chip is not behind the cache. Nothing is needed for it - the
    // instruction fetch and the data write reach the same RAM.
    if (is_external(dst)) {
        esp_cache_msync(dst, bytes,
                        ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_TYPE_INST);
    }
}

// The address to CALL code at, given the address it was WRITTEN at.
//
// On this chip the internal SRAM is one block of memory with two addresses:
// writes go through 0x3FC88000 and up, instruction fetches through 0x40378000
// and up, and SOC_I_D_OFFSET is the distance between them. Writing the bytes
// through the data address and then calling that same address is a cache
// error with EXCVADDR pointing straight at the buffer - which is exactly how
// this was found. PSRAM has no second address and needs no conversion.
static void *entry_for(void *written_at)
{
    if (esp_ptr_in_diram_dram(written_at)) return esp_ptr_diram_dram_to_iram(written_at);
    return written_at;
}

static int run_from(void *dst, const void *src, size_t bytes)
{
    memcpy(dst, src, bytes);
    sync_code(dst, bytes);
    int (*fn)(int, int) = (int (*)(int, int))entry_for(dst);
    return fn(7, 42);
}

void loader_probe_test(void)
{
    logln();
    logln("--- stage 7a: can this board run code it was not built with? ---");
    char d[160];

    // 0. Where the probe lives when it is built in, for comparison.
    snprintf(d, sizeof(d), "probe_add at %p, in %s", (void *)&probe_add,
             region_of((const void *)&probe_add));
    sdk_case("the probe, as built", probe_add(7, 42) == 1834, d);

    // 1. Executable memory: what does the allocator give, and where is it?
    //
    // NOT MALLOC_CAP_EXEC. That capability is behind CONFIG_HEAP_HAS_EXEC_HEAP,
    // which is off on the ESP32-S3 and does not exist to be turned on: this
    // chip reaches its internal RAM from the instruction bus and the data bus
    // at the same address, so there is no separate executable heap to ask for.
    // Ordinary internal RAM is executable, and that is the first thing stage 7
    // needed to know.
    void *iram = heap_caps_malloc(PROBE_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    snprintf(d, sizeof(d), "written at %p (%s), called at %p", iram,
             iram ? region_of(iram) : "-", iram ? entry_for(iram) : nullptr);
    sdk_case("executable memory exists", iram != nullptr, d);

    // 2. Code copied into it, and run.
    if (iram) {
        const int got = run_from(iram, (const void *)&probe_add, PROBE_BYTES);
        snprintf(d, sizeof(d), "copied %d bytes, called it, got %d (want 1834)",
                 PROBE_BYTES, got);
        sdk_case("a copy of it runs", got == 1834, d);
    }

    // 3. NOT TESTED HERE: a copy calling back into the operating system.
    //    It would need the address of something - a string, a function - and
    //    every one of those is reached in a way that a copy breaks. That is
    //    the loader's job, and the loader is the next step, not this one.

    if (iram) heap_caps_free(iram);

    // 4. THE ONE THAT DECIDES THE DESIGN. Internal RAM is small; an installed
    //    app of any size has to live in PSRAM. Whether the instruction bus
    //    reaches PSRAM on this chip in this build is what this asks.
    void *ps = heap_caps_malloc(PROBE_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ps) {
        sdk_case("PSRAM can hold code", false, "no PSRAM allocation");
    } else {
        snprintf(d, sizeof(d), "%p, %s - see the log for whether it ran",
                 ps, region_of(ps));
        logf("  trying to execute from PSRAM at %p; if the board reboots here,\n", ps);
        logf("  the instruction bus does not reach PSRAM and stage 7 needs a\n");
        logf("  different plan. That is the answer this test exists to get.\n");
        const int got = run_from(ps, (const void *)&probe_add, PROBE_BYTES);
        snprintf(d, sizeof(d), "%s, called it, got %d (want 1834)", region_of(ps), got);
        sdk_case("PSRAM can hold code", got == 1834, d);
        heap_caps_free(ps);
    }

    // 5. How much room there is, which bounds an app if PSRAM cannot be used.
    {
        const size_t exec = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        const size_t ps_free = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
        snprintf(d, sizeof(d), "largest executable block %u KB, largest PSRAM block %u KB",
                 (unsigned)(exec / 1024), (unsigned)(ps_free / 1024));
        sdk_case("room for an app", exec > 4096, d);
    }
}

#endif // SMARTOS_TEST_LOADER
