#pragma once
#include <stdbool.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// Stage 4.3a - parking core 1 while core 0 touches flash.
//
// Whenever the SPI flash driver runs - a read, a write or an erase - it turns
// the cache off. On this chip that takes flash AND PSRAM out of reach at once,
// for as long as the operation lasts: a 4 KB sector erase is tens of
// milliseconds. IDF protects its own second core by stalling it first. Ours is
// not IDF's, so until now core 1 was protected only by a rule - every function
// and every byte it touches lives in IRAM or DRAM.
//
// That rule cannot survive apps. An app is ordinary code: it lives in flash,
// it draws into a framebuffer in PSRAM. So instead of forbidding core 1 the
// cache, core 0 asks it to step away from it:
//
//   1. core 0 raises FROM_CPU_INTR3, routed to CPU interrupt KPARK_CPU_INT on
//      core 1 only
//   2. core 1's handler - IRAM, touching nothing but DRAM - says so, and spins
//   3. core 0 sees the acknowledgement, runs the flash operation, and releases
//      core 1, which returns to whatever it was doing, flash-resident or not
//
// WHAT CAN HOLD A PARK UP
//
// The request is a level-1 interrupt, so core 1 takes it only with interrupts
// open. Every kernel critical section - k_lock(), the event bus lock - runs at
// level 3, which means core 1 is never parked while holding one of those, and
// a park simply waits for the section to end. kpark_begin() gives up after its
// timeout and says so: a flash operation that goes ahead without the park is
// the crash this exists to prevent, so the caller must not proceed.
//
// RULES FOR CALLERS (core 0)
//
// - Call from a core 0 FreeRTOS task, never from core 1 or an ISR.
// - Nothing between begin and end may log or take a lock core 1 might hold -
//   the console lock in particular. Core 1 can be parked holding it.
// - Keep the window to the flash operation itself. Core 1's whole scheduler is
//   stopped for the duration; its clock is not (see k_int_dispatch).
// ---------------------------------------------------------------------------

// Core 1's CPU interrupt for the park request. Level 1, level-triggered.
// Core 1's interrupt allocation is entirely ours - IDF never configures it.
#define KPARK_CPU_INT        2
#define KPARK_TIMEOUT_US  5000

// EVERY FLASH OPERATION PARKS, WITHOUT BEING ASKED (stage 4.4)
//
// Wrapping each flash call by hand stopped being enough the moment a
// filesystem arrived: LittleFS reads, programs and erases from deep inside its
// own code. So the link wraps the two functions ESP-IDF itself calls around
// every SPI flash operation - spi_flash_disable_ / _enable_
// interrupts_caches_and_other_cpu - and the wrappers park core 1 before the
// cache goes off and release it after it comes back. Anything that touches
// flash, now or later (NVS, OTA, a second filesystem), is covered. Explicit
// kpark_begin/end still work; they simply nest.
//
// There is no way to refuse from inside the hook: IDF is already committed to
// the operation. If core 1 does not acknowledge within KPARK_FLASH_TIMEOUT_US
// the operation goes ahead unparked, and is counted - it should always read 0.
#define KPARK_FLASH_TIMEOUT_US 50000

// Core 1, once, before interrupts are opened.
void kpark_init_core1(void);

// Core 1, from k_int_dispatch when KPARK_CPU_INT is pending.
void kpark_isr(void);

// Core 0. True when it is safe to touch flash: core 1 is parked, or is not
// running, or is sitting in the fault recorder (which is all IRAM). False on a
// timeout - do NOT touch flash then. Nests: only the outermost pair parks.
bool kpark_begin(uint32_t timeout_us);
void kpark_end(void);

typedef struct {
    uint32_t parks;         // completed parks
    uint32_t timeouts;      // requests core 1 never acknowledged
    uint32_t skipped;       // begin() with core 1 not running or faulted
    uint32_t ack_max_us;    // longest wait for core 1 to acknowledge
    uint32_t held_max_us;   // longest time core 1 stayed parked
    uint64_t held_total_us;
    uint32_t flash_ops;     // flash operations seen by the global hook
    uint32_t unparked;      // ...that went ahead without core 1 parked
} kpark_stats_t;

kpark_stats_t kpark_stats(void);
