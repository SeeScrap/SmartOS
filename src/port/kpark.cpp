#include "port/kpark.h"

#include "port/kport.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "soc/interrupts.h"
#include "soc/soc.h"
#include "soc/system_reg.h"

#include "port/kcore1.h"
#include "kernel/kfault.h"
#include "kernel/ktask.h"

// Everything core 1 reads while parked is here, in DRAM.
static volatile K_DRAM uint32_t s_park_req;   // core 0 -> core 1
static volatile K_DRAM uint32_t s_parked;     // core 1 -> core 0

// Core 0 only.
static uint32_t      s_depth;
static bool          s_holding;          // this begin() really parked core 1
static int64_t       s_park_start_us;
static kpark_stats_t s_st;

static inline K_IRAM void barrier(void)
{
    __asm__ __volatile__("memw" ::: "memory");
}

K_IRAM void kpark_init_core1(void)
{
    // Routed to core 1 only. kcore1_boot has already pointed every source on
    // core 1 at nothing; this is the one it listens to besides its own timer.
    esp_rom_route_intr_matrix(1, ETS_FROM_CPU_INTR3_SOURCE, KPARK_CPU_INT);
    REG_WRITE(SYSTEM_CPU_INTR_FROM_CPU_3_REG, 0);

    uint32_t ie;
    __asm__ __volatile__("rsr %0, intenable" : "=a"(ie));
    ie |= (1u << KPARK_CPU_INT);
    __asm__ __volatile__("wsr %0, intenable; rsync" :: "a"(ie));
}

K_IRAM void kpark_isr(void)
{
    // A level-triggered source: it stays asserted until written back to 0,
    // and would re-enter us the moment we returned.
    REG_WRITE(SYSTEM_CPU_INTR_FROM_CPU_3_REG, 0);

    if (!s_park_req) return;            // released before we got here

    uint32_t cc0;
    __asm__ __volatile__("rsr %0, ccount" : "=a"(cc0));

    barrier();
    s_parked = 1;
    barrier();

    // The whole point: from here until release, nothing on this core fetches
    // an instruction or a byte from flash or PSRAM. This loop and these two
    // words are IRAM and DRAM; the tick is masked, since we are at level 1.
    while (s_park_req) { }

    barrier();
    s_parked = 0;
    barrier();

    // Stage 5.3: this stretch was core 0's flash work, not the interrupted
    // task's - nor idle, if the park landed in the scheduler's wait.
    uint32_t cc1;
    __asm__ __volatile__("rsr %0, ccount" : "=a"(cc1));
    k_account_park(cc1 - cc0);
}

bool kpark_begin(uint32_t timeout_us)
{
#ifdef SMARTOS_TEST_PARK_OFF
    // The proof build: parking switched off everywhere, the global hook
    // included, so the test can show what happens without it.
    (void)timeout_us;
    return true;
#endif
    if (s_depth++ > 0) return true;     // nested: the outer call already parked

    s_holding = false;
    if (!kcore1_is_up() || kfault_pending()) {
        s_st.skipped++;
        return true;
    }

    const int64_t t0 = esp_timer_get_time();
    s_park_req = 1;
    barrier();
    REG_WRITE(SYSTEM_CPU_INTR_FROM_CPU_3_REG, 1);

    while (!s_parked) {
        if (esp_timer_get_time() - t0 > (int64_t)timeout_us) {
            // Withdraw cleanly. If core 1 takes the interrupt later it will
            // find the request gone and return at once.
            s_park_req = 0;
            barrier();
            REG_WRITE(SYSTEM_CPU_INTR_FROM_CPU_3_REG, 0);
            s_st.timeouts++;
            s_depth--;
            return false;
        }
    }

    const int64_t t1 = esp_timer_get_time();
    const uint32_t ack = (uint32_t)(t1 - t0);
    if (ack > s_st.ack_max_us) s_st.ack_max_us = ack;

    s_holding       = true;
    s_park_start_us = t1;
    return true;
}

void kpark_end(void)
{
#ifdef SMARTOS_TEST_PARK_OFF
    return;
#endif
    if (s_depth == 0) return;           // unbalanced; ignore rather than underflow
    if (--s_depth > 0) return;
    if (!s_holding) return;

    s_holding = false;
    barrier();
    s_park_req = 0;
    barrier();

    // Wait for core 1 to leave the loop, so a begin() straight after this one
    // cannot find it still flagged as parked from last time.
    const int64_t t0 = esp_timer_get_time();
    while (s_parked && esp_timer_get_time() - t0 < 1000) { }

    const uint32_t held = (uint32_t)(t0 - s_park_start_us);
    s_st.parks++;
    s_st.held_total_us += held;
    if (held > s_st.held_max_us) s_st.held_max_us = held;
}

kpark_stats_t kpark_stats(void)
{
    return s_st;
}

// --- the global hook -------------------------------------------------------------
// Linked in place of IDF's own functions by -Wl,--wrap (src/CMakeLists.txt);
// the __real_ names reach the originals.
extern "C" void __real_spi_flash_disable_interrupts_caches_and_other_cpu(void);
extern "C" void __real_spi_flash_enable_interrupts_caches_and_other_cpu(void);

// The cache is still on when this runs, so calling into the park code - which
// lives in flash - is fine. It has to happen HERE, before the real function
// turns the cache off.
extern "C" K_IRAM void __wrap_spi_flash_disable_interrupts_caches_and_other_cpu(void)
{
    s_st.flash_ops++;
    if (!kpark_begin(KPARK_FLASH_TIMEOUT_US)) s_st.unparked++;
    __real_spi_flash_disable_interrupts_caches_and_other_cpu();
}

// The mirror image: this is entered with the cache OFF, so it is IRAM and may
// touch nothing in flash until the real function has turned the cache back
// on. Only then is core 1 released.
extern "C" K_IRAM void __wrap_spi_flash_enable_interrupts_caches_and_other_cpu(void)
{
    __real_spi_flash_enable_interrupts_caches_and_other_cpu();
    kpark_end();
}
