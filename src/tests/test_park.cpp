// Stage 4.3a - parking core 1 around flash operations, and the two tests that
// prove it works: 100 reads on every boot, and 40 erase/write/verify rounds
// under -D S3_TEST_PARK with core 1 running flash-resident code throughout.
//
// -D S3_TEST_PARK_OFF runs the same rounds with the park disabled, to show
// what it prevents. That build is expected to crash.
//
// Moved out of main.cpp in R2c of the SmartOS restructuring. The only change
// was turning two hand-written "OK" / "!! FAIL" strings into sdk_case()s.

#include "tests/tests.h"

#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_partition.h"
#include "esp_timer.h"

#include "kernel/klog.h"
#include "kernel/kpark.h"
#include "kernel/ktask.h"
#include "kernel/ktime.h"
#include "s3hal/hal_time.h"
#include "s3hal/log.h"
#include "tests/harness.h"

// ---------------------------------------------------------------------------
// Stage 4.3a - parking core 1 around flash operations.
// ---------------------------------------------------------------------------

// A park wraps every flash operation. The _OFF build skips it on purpose, to
// show what the park prevents.
#ifdef S3_TEST_PARK_OFF
#define PARK_BEGIN() (true)
#define PARK_END()   do { } while (0)
#else
#define PARK_BEGIN() kpark_begin(KPARK_TIMEOUT_US)
#define PARK_END()   kpark_end()
#endif

// The partition flash tests may erase: 'scratch', 64 KB set aside in stage
// 4.4. Before that these tests used the last sector of 'storage', which is now
// LittleFS's and must never be written behind its back.
static const esp_partition_t *storage_partition(void)
{
    return esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                    ESP_PARTITION_SUBTYPE_ANY, "scratch");
}

// Until R2c this printed its own "OK" / "!! DRIFT" at the end of a free-text
// line, so the check it really makes was invisible to tools/boot_check.py and
// was counted only because the line happened to end in OK. It is an
// sdk_case() now, and the report around it is just a report.
static void park_report(uint32_t kms, uint32_t ems)
{
    const kpark_stats_t st = kpark_stats();
    logf("  parks %lu, refused %lu, skipped %lu\n",
         (unsigned long)st.parks, (unsigned long)st.timeouts,
         (unsigned long)st.skipped);
    logf("  core 1 acknowledged within %lu us at worst\n",
         (unsigned long)st.ack_max_us);
    logf("  core 1 held parked %lu us at worst, %lu ms in total\n",
         (unsigned long)st.held_max_us,
         (unsigned long)(st.held_total_us / 1000));

    const long drift = (long)kms - (long)ems;
    char d[140];
    snprintf(d, sizeof(d), "%lu ms vs esp_timer %lu ms: %ld apart (%lu catch-ups, "
             "up to %lu ticks at once)", (unsigned long)kms, (unsigned long)ems, drift,
             (unsigned long)k_tick_catchups(), (unsigned long)k_tick_max_step());
    sdk_case("the kernel clock held", drift <= 2 && drift >= -2, d);
}

// Reads need a park as much as writes do: the SPI driver turns the cache off
// for any operation, not only the ones that change flash. Reads cost the
// flash no wear, so this runs on every boot.
void park_smoke_test(void)
{
    logln();
    logln("--- stage 4.3a: parking core 1 around 100 flash reads ---");

    const esp_partition_t *p = storage_partition();
    if (!p) { logln("  !! no 'scratch' partition"); return; }

    static uint8_t buf[4096];               // internal RAM: PSRAM is off too
    const uint32_t k0 = k_millis();
    const int64_t  e0 = esp_timer_get_time();
    int ok = 0, refused = 0, errs = 0;

    for (int i = 0; i < 100; ++i) {
        if (!PARK_BEGIN()) { refused++; continue; }
        esp_err_t err = esp_partition_read(p, (uint32_t)(i % (p->size / 4096u)) * 4096u, buf, sizeof(buf));
        PARK_END();
        if (err == ESP_OK) ok++; else errs++;
        hal_delay_ms(2);
    }

    const uint32_t kms = k_millis() - k0;
    const uint32_t ems = (uint32_t)((esp_timer_get_time() - e0) / 1000);
    char d[80];
    snprintf(d, sizeof(d), "%d read, %d failed, %d refused", ok, errs, refused);
    sdk_case("flash reads, parked", ok == 100 && errs == 0, d);
    park_report(kms, ems);
}

#if defined(S3_TEST_PARK) || defined(S3_TEST_PARK_OFF)
// The workload the park exists for: core 1 running code that lives in flash
// and writing memory that lives in PSRAM, flat out, while core 0 erases and
// writes flash underneath it.
//
// NOT IRAM_ATTR, on purpose - both functions below must be fetched through the
// cache. The table is read through a volatile pointer so that -O2 cannot fold
// the sum into a constant and quietly stop touching flash at all.
struct flash_table_t {
    uint32_t v[256];
    constexpr flash_table_t() : v() {
        for (int i = 0; i < 256; ++i) v[i] = (uint32_t)i * 2654435761u;
    }
};
static constexpr flash_table_t s_flash_table{};

static uint32_t          *s_park_psram;
static volatile DRAM_ATTR uint32_t s_fw_iters, s_fw_bad;

__attribute__((noinline)) static uint32_t flash_sum(void)
{
    const volatile uint32_t *p = s_flash_table.v;
    uint32_t s = 0;
    for (int i = 0; i < 256; ++i) s += p[i];
    return s;
}

static void flashwork_task(void *arg)
{
    (void)arg;
    const uint32_t expect = flash_sum();
    for (uint32_t i = 0;; ++i) {
        const uint32_t s = flash_sum();
        volatile uint32_t *slot = &s_park_psram[i & 1023];
        *slot = s;
        if (s != expect || *slot != s) s_fw_bad = s_fw_bad + 1;
        s_fw_iters = s_fw_iters + 1;
    }
}

void park_flash_test(void)
{
    logln();
#ifdef S3_TEST_PARK_OFF
    logln("--- stage 4.3a: 40 erase/write/verify rounds, park DISABLED ---");
    logln("  core 1 is running flash-resident code; this is expected to crash");
#else
    logln("--- stage 4.3a: 40 erase/write/verify rounds under a park ---");
#endif
    logf("  core 1 workload runs from %p (flash text is 0x42xxxxxx), "
         "table at %p, PSRAM at %p\n",
         (void *)&flash_sum, (void *)s_flash_table.v, (void *)s_park_psram);

    const esp_partition_t *p = storage_partition();
    if (!p) { logln("  !! no 'scratch' partition"); return; }
    const uint32_t off = p->size - 4096u;   // the partition's last sector

    static uint8_t wbuf[4096], rbuf[4096];
    const uint32_t k0 = k_millis();
    const int64_t  e0 = esp_timer_get_time();
    const uint32_t it0 = s_fw_iters;
    int ok = 0, bad = 0, refused = 0;

    for (int round = 0; round < 40; ++round) {
        for (int i = 0; i < 4096; ++i) wbuf[i] = (uint8_t)(i * 7 + round * 13);

        if (!PARK_BEGIN()) { refused++; continue; }
        esp_err_t e1 = esp_partition_erase_range(p, off, 4096);
        esp_err_t e2 = esp_partition_write(p, off, wbuf, 4096);
        esp_err_t e3 = esp_partition_read(p, off, rbuf, 4096);
        PARK_END();

        if (e1 == ESP_OK && e2 == ESP_OK && e3 == ESP_OK &&
            memcmp(wbuf, rbuf, sizeof(wbuf)) == 0) ok++;
        else bad++;
        hal_delay_ms(10);                   // let core 1 run between rounds
    }

    const uint32_t kms = k_millis() - k0;
    const uint32_t ems = (uint32_t)((esp_timer_get_time() - e0) / 1000);
    char d[140];
    snprintf(d, sizeof(d), "%d rounds verified, %d bad, %d refused; core 1 ran %lu passes, %lu wrong",
             ok, bad, refused, (unsigned long)(s_fw_iters - it0), (unsigned long)s_fw_bad);
    // At least 1000 passes, not merely "some": with the park disabled, core 1
    // managed 41 passes in the whole run - stalled on a cache that was
    // switched off - and "made progress" would have passed.
    sdk_case("erase, write and verify under a park",
             ok == 40 && s_fw_bad == 0 && s_fw_iters - it0 >= 1000, d);
    park_report(kms, ems);
}
#endif

#if defined(S3_TEST_PARK) || defined(S3_TEST_PARK_OFF)
// Core 0, at boot, before core 1 starts: the PSRAM the workload writes to.
void park_psram_alloc(void)
{
    s_park_psram = (uint32_t *)heap_caps_malloc(4096, MALLOC_CAP_SPIRAM);
}

// Flash-resident code on core 1, pinned there, never yielding - the workload
// that needs the park. Preemption keeps the kernel's own tasks running around
// it.
void IRAM_ATTR kernel_park_work_start(void)
{
    k_task_create_on("flashwork", flashwork_task, nullptr, 1);
}
#endif
