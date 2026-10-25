// The ESP32-S3 side of the port seam. See port/kport.h for what it is for.

#include "port/kport.h"

#include <stdarg.h>
#include <stdio.h>

#include "esp_heap_caps.h"
#include "sdkconfig.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "kernel/kspin.h"

// The locks the kernel declares and the chip owns.
//
// k_console_lock is held by both cores - by kport_printf here and by klog() on
// core 1 - which is why it lives on this side of the seam rather than in any
// one core's code. The very first bring-up faulted inside the ROM printf with
// both cores in it, before this existed.
//
// k_sched_lock and k_spin_stuck are the scheduler's, and are defined beside it
// because all three were defined together in board/log.cpp until R4b and there
// is no reason to scatter them now.
extern "C" {
    k_spin_t          k_console_lock = 0;
    k_spin_t          k_sched_lock   = 0;
    volatile uint32_t k_spin_stuck   = 0;
}

extern "C" void kport_vprintf(const char *fmt, va_list ap)
{
    const bool locked = k_spin_lock(&k_console_lock);
    vprintf(fmt, ap);
    if (locked) k_spin_unlock(&k_console_lock);
}

extern "C" void kport_printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    kport_vprintf(fmt, ap);
    va_end(ap);
}

extern "C" void kport_println(const char *s)
{
    if (s && *s) fputs(s, stdout);
    fputs("\n", stdout);
}


// Core 0 only. hal_delay_ms() is the same call; the kernel reaches it through
// here so that kernel/kmbox.cpp does not have to include the HAL, which would
// pull FreeRTOS into the kernel directory.
extern "C" void kport_yield_ms(uint32_t ms)
{
    vTaskDelay(ms / portTICK_PERIOD_MS);
}


// --- memory, once, at boot -------------------------------------------------
extern "C" void *kport_region_alloc(size_t align, size_t bytes, bool psram)
{
    const uint32_t caps = (psram ? MALLOC_CAP_SPIRAM : MALLOC_CAP_INTERNAL) | MALLOC_CAP_8BIT;
    return heap_caps_aligned_alloc(align, bytes, caps);
}

extern "C" size_t kport_largest_free_internal(void)
{
    return heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

// ESP-IDF owns the CPU clock setting and the kernel keeps its own copy of it
// in smartos_config.h, because three places outside the kernel divide CCOUNT
// cycles by it. This is the one translation unit that should see both, so the
// check that they agree lives here. It was in kernel/ktime.cpp until R4d, and
// it was the last reason that file included sdkconfig.h.
static_assert(K_CPU_MHZ == CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
              "K_CPU_MHZ in smartos_config.h disagrees with "
              "CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ in sdkconfig - make them match");
