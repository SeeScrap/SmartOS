#include "kernel/kcore1.h"

#include <stdint.h>

#include "esp_attr.h"
#include "esp_cpu.h"
#include "esp_rom_sys.h"
#include "hal/cpu_utility_ll.h"
#include "kernel/klog.h"
#include "soc/interrupts.h"
#include "soc/soc.h"

// The ROM routine that tells the app-cpu boot stub where to jump. Declared by
// hand rather than pulled in from esp32s3/rom/ets_sys.h so this file does not
// depend on which of IDF's per-target include directories happens to be on the
// path.
extern "C" void ets_set_appcpu_boot_addr(uint32_t start);

// Our own vector table (kernel/kvectors.S).
//
// Stage 2.1 pointed core 1 at IDF's table; stage 2.2 replaces it, because IDF's
// exception dispatch indexes its handler table by core id against a table that
// a unicore build only sizes for one core. A fault on core 1 would run whatever
// handler sits in the next slot. See the header comment in kvectors.S.
extern "C" void s3_vector_table(void);

static volatile bool s_core1_up = false;
static kcore1_entry_t s_entry   = nullptr;

// First instruction of ours that core 1 ever executes.
//
// It arrives here from the ROM app-cpu stub with a stack the ROM picked, no
// interrupts configured, and VECBASE at its reset value. IRAM_ATTR because the
// core has to be able to run this whether or not the flash cache is available.
extern "C" void IRAM_ATTR kcore1_boot(void)
{
    // Before anything else: a vector table that is ours and that can service a
    // window overflow. Until this line runs, the first function call core 1
    // makes could vector into a handler meant for core 0.
    esp_cpu_intr_set_ivt_addr((const void *)&s3_vector_table);

    // Stop the ROM stub from re-entering us if the core is ever reset again.
    ets_set_appcpu_boot_addr(0);

    // Core 1 has its own interrupt matrix, and nothing has ever configured it.
    // Park every peripheral source on the invalid line so that a stray
    // peripheral cannot vector into us before we are ready for it in 2.2.
    for (int src = 0; src < ETS_MAX_INTR_SOURCE; ++src) {
        esp_rom_route_intr_matrix(1, src, ETS_INVALID_INUM);
    }

    cpu_utility_ll_enable_debug(1);
    cpu_utility_ll_enable_record(1);

    s_core1_up = true;

    if (s_entry) s_entry();

    // Whatever we were given has finished. Park rather than run off the end of
    // the function into whatever bytes follow it.
    for (;;) {
        __asm__ __volatile__("waiti 0");
    }
}

bool kcore1_start(kcore1_entry_t entry, uint32_t timeout_ms)
{
    if (s_core1_up) return true;

    s_entry    = entry;
    s_core1_up = false;

    // This is the same sequence IDF's start_other_core() performs; the
    // difference is only that under unicore IDF never runs it.
    esp_cpu_unstall(1);
    cpu_utility_ll_enable_clock_and_reset_app_cpu();
    cpu_utility_ll_enable_clock_and_reset_app_cpu_int_matrix();

    ets_set_appcpu_boot_addr((uint32_t)&kcore1_boot);

    for (uint32_t waited = 0; waited < timeout_ms * 10; ++waited) {
        if (s_core1_up) return true;
        esp_rom_delay_us(100);
    }
    return false;
}

bool kcore1_is_up(void)
{
    return s_core1_up;
}

// ---------------------------------------------------------------------------
// Stage 4.4 - putting core 1 to sleep before the chip restarts.
//
// ESP-IDF's restart - after a panic, or esp_restart() - resets and stalls the
// other core only in a dual-core build:
//
//     #if !CONFIG_ESP_SYSTEM_SINGLE_CORE_MODE
//         esp_rom_software_reset_cpu(other_core_id);
//         esp_cpu_stall(other_core_id);
//     #endif
//
// This build is single-core as far as IDF knows, so core 1 - ours - was left
// RUNNING through the restart: still executing the old kernel while the
// bootloader loaded the new image over the memory it was running from. The
// next boot then met a core 1 faulting on garbage and a kernel whose state it
// had trampled - IllegalInstruction, a stack pointer of 1, a clock stuck at 0.
// It had been happening after every crash; stage 4.4's stack overflow was the
// first time the aftermath was looked at closely enough to see why.
//
// So the restart is wrapped (-Wl,--wrap=esp_restart_noos) to do for core 1
// what IDF does for its own second core: nothing to jump to, reset, stall. The
// stall outlives core 0's reset, so core 1 sleeps through the whole next boot
// until kcore1_start() wakes it on purpose.
// ---------------------------------------------------------------------------
extern "C" void __real_esp_restart_noos(void) __attribute__((noreturn));

extern "C" IRAM_ATTR __attribute__((noreturn)) void __wrap_esp_restart_noos(void)
{
    ets_set_appcpu_boot_addr(0);            // if it wakes, there is nowhere to go
    esp_rom_software_reset_cpu(1);          // IDF's order: reset before stall,
    esp_cpu_stall(1);                       // in case it was mid-s32c1i
    __real_esp_restart_noos();
}
