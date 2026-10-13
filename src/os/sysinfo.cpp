#include "os/sysinfo.h"

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

#include "kernel/kpark.h"
#include "kernel/ktime.h"
#include "os/fs.h"

static DRAM_ATTR sysinfo_t          s_info;
static volatile DRAM_ATTR uint32_t  s_seq;           // odd while being written
static int64_t                      s_next_us, s_next_fs_us;

static inline void barrier(void)
{
    __asm__ __volatile__("memw" ::: "memory");
}

void sysinfo_poll(void)
{
    const int64_t now = esp_timer_get_time();
    if (now < s_next_us) return;
    s_next_us = now + 500000;

    sysinfo_t n = s_info;
    const uint32_t in = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    n.ram_free    = (uint32_t)heap_caps_get_free_size(in);
    n.ram_total   = (uint32_t)heap_caps_get_total_size(in);
    n.ram_min     = (uint32_t)heap_caps_get_minimum_free_size(in);
    n.ram_largest = (uint32_t)heap_caps_get_largest_free_block(in);
    n.psram_free  = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    n.psram_total = (uint32_t)heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    n.flash_ops   = kpark_stats().flash_ops;
    n.uptime_ms   = k_millis();
    if (now >= s_next_fs_us && fs_mounted()) {
        s_next_fs_us = now + 10000000;
        size_t total = 0, used = 0;
        fs_info(&total, &used);
        n.fs_total = (uint32_t)total;
        n.fs_used  = (uint32_t)used;
    }

    s_seq = s_seq + 1;          // odd: writing
    barrier();
    s_info = n;
    barrier();
    s_seq = s_seq + 1;          // even: whole
}

IRAM_ATTR void sysinfo_get(sysinfo_t *out)
{
    for (;;) {
        const uint32_t a = s_seq;
        barrier();
        *out = s_info;
        barrier();
        if (!(a & 1) && s_seq == a) return;
    }
}
