// ---------------------------------------------------------------------------
// Project S3 - stage 2.0: same bring-up, now on bare ESP-IDF
//
// Nothing about what this program checks has changed. What changed is who is
// underneath it: the Arduino core is gone, so sdkconfig is ours. FreeRTOS is
// pinned to core 0 (CONFIG_FREERTOS_UNICORE) and core 1 is never started,
// which is what stage 2.1 will claim for the kernel.
// ---------------------------------------------------------------------------
// Builds that replace the kernel's default task set with a test of their own.
// Apps are not started in these: several of the tests need core 1 to
// themselves, and none of them create the app manager.
#if defined(S3_PREEMPT_SOAK) || defined(S3_TEST_SLEEP_CLAIM) ||     defined(S3_TEST_MBOX_STRESS) || defined(S3_TEST_EVBUS) ||     defined(S3_TEST_PARK) || defined(S3_TEST_PARK_OFF)
#define S3_KERNEL_TEST_BUILD 1
#endif

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_chip_info.h"
#include "esp_cpu.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_partition.h"
#include "esp_psram.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_mac.h"
#include "soc/rtc_cntl_reg.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "board/pins.h"
#include "s3hal/hal_time.h"
#include "s3hal/input.h"
#include "s3hal/lcd_bus.h"
#include "gfx/fb.h"
#include "gfx/text.h"
#include "os/evbus.h"
#include "os/app.h"
#include "os/boot.h"
#include "apps/apps.h"
#include "os/dev.h"
#include "os/fs.h"
#include "os/screen.h"
#include "os/settings.h"
#include "system.h"
#include "tests/harness.h"
#include "tests/tests.h"
#include "os/shell.h"
#include "os/sysinfo.h"
#include "os/syslog.h"
#include "os/kfmt.h"
#include "os/krand.h"
#include "s3hal/lcd_panel.h"
#include "s3hal/lcd_probe.h"
#include "s3hal/log.h"

#include "kernel/kcore1.h"
#include "kernel/kfault.h"

extern "C" volatile uint32_t s3_chain_end_hit;
extern "C" volatile uint32_t s3_spin_stuck;
#include "kernel/kheap.h"
#include "kernel/kmbox.h"
#include "kernel/ksync.h"
#include "kernel/ktask.h"
#include "kernel/ktime.h"
#include "kernel/kpark.h"
#include "kernel/klog.h"

static uint16_t *s_framebuffer = nullptr;

// Why the chip last reset, in words. Used by the boot report on the console
// and, since stage 4.5, by the first line each boot writes to the system log.
static const char *reset_reason_name(void)
{
    const char *why = "?";
    switch (esp_reset_reason()) {
        case ESP_RST_POWERON:  why = "power-on";                       break;
        case ESP_RST_EXT:      why = "external reset pin";             break;
        case ESP_RST_SW:       why = "software restart";               break;
        case ESP_RST_PANIC:    why = "panic / exception";              break;
        case ESP_RST_INT_WDT:  why = "interrupt watchdog";             break;
        case ESP_RST_TASK_WDT: why = "task watchdog";                  break;
        case ESP_RST_WDT:      why = "other watchdog";                 break;
        case ESP_RST_BROWNOUT: why = "BROWNOUT - the supply sagged!";  break;
        case ESP_RST_SDIO:     why = "SDIO";                           break;
        case ESP_RST_DEEPSLEEP:why = "deep sleep wake";                break;
        // Stage 4.5 found these missing: the USB one is what an upload or a
        // serial monitor's reset looks like, and it read "unknown" in the log.
        case ESP_RST_USB:      why = "USB (upload or serial monitor)"; break;
        case ESP_RST_JTAG:     why = "JTAG";                           break;
        case ESP_RST_EFUSE:    why = "efuse error";                    break;
        case ESP_RST_PWR_GLITCH: why = "power glitch";                 break;
        case ESP_RST_CPU_LOCKUP: why = "CPU lock-up (double exception)"; break;
        default:               why = "unknown";                        break;
    }
    return why;
}

static void report_memory(void)
{
    esp_chip_info_t info;
    esp_chip_info(&info);

    uint32_t flash_sz = 0;
    esp_flash_get_size(NULL, &flash_sz);

    logln();
    logln("=== Project S3 / bring-up (ESP-IDF) ===");
    logf("chip      : ESP32-S3 rev %d, %d core(s) @ %d MHz\n",
         info.revision, info.cores, CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ);
    logf("flash     : %lu bytes\n", (unsigned long)flash_sz);
    logf("internal  : %lu bytes free\n", (unsigned long)esp_get_free_heap_size());

    size_t psram = esp_psram_get_size();
    logf("PSRAM     : %lu bytes\n", (unsigned long)psram);
    if (psram < 4u * 1024 * 1024) {
        logln("  !! expected ~8 MB. Check CONFIG_SPIRAM_MODE_OCT in sdkconfig.defaults");
    }

    // The whole point of stage 2.0: prove FreeRTOS is confined to one core.
    // Power-supply forensics. A rail that is sagging leaves fingerprints here
    // long before it stops the board outright: the brownout detector trips at
    // roughly 2.5 V on the 3.3 V rail and forces a reset, and flash and PSRAM
    // are the first things to misbehave when they are underfed.
    const char *why = reset_reason_name();
    logf("reset cause: %s\n", why);

    logf("running on: core %d\n", (int)xPortGetCoreID());
#if CONFIG_FREERTOS_UNICORE
    logln("core 1    : asleep, never started by IDF  <-- ours to claim in 2.1");
#else
    logln("core 1    : !! FreeRTOS owns it - CONFIG_FREERTOS_UNICORE is not set");
#endif
}

// --- stage 2.1/2.2/2.3: everything that runs on core 1 --------------------
//
// IRAM_ATTR throughout: core 1 must not depend on the flash cache, which
// core 0 can disable under it at any moment.
//
// klog() rather than logf() - see kernel/klog.h for why printf would hang.

// --- stage 2.3: what runs on core 1 once the kernel is up ------------------

// The point of this task is the local variable. If `i` still counts correctly
// after k_yield() has run other tasks in between, then each task really is on
// its own stack and the context switch preserved it.
static void IRAM_ATTR demo_task(void *arg)
{
    const char *name = (const char *)arg;
    for (int i = 0; i < 4; ++i) {
        klog("[%s] i=%d  sp=%p\n", name, i, (void *)&i);
        k_yield();
    }
    klog("[%s] finished with i intact\n", name);
}

// Yields from five frames down. Every one of those frames is live in the
// register window when the switch happens, so this is the test that says
// whether SPILL_ALL_WINDOWS is doing its job. The markers catch a spill that
// wrote to the wrong stack.
static int IRAM_ATTR deep_recurse(int depth)
{
    volatile int marker = depth * 1000 + 7;
    if (depth == 0) {
        k_yield();
    } else {
        deep_recurse(depth - 1);
    }
    if (marker != depth * 1000 + 7) {
        klog("[deep] !! frame at depth %d was corrupted (got %d)\n",
             depth, marker);
        return -1;
    }
    return 0;
}

static void IRAM_ATTR deep_task(void *arg)
{
    (void)arg;
    for (int round = 0; round < 3; ++round) {
        if (deep_recurse(5) == 0) {
            klog("[deep] round %d: yielded from 6 frames deep, all intact\n",
                 round);
        }
        k_yield();
    }
}


// --- stage 2.4/2.5/2.6a demo tasks ----------------------------------------

// Sleeps on the kernel's own clock. Nothing here polls or busy-waits: the task
// is marked K_SLEEPING and the tick ISR is what eventually makes it runnable.
static void IRAM_ATTR ticker_task(void *arg)
{
    (void)arg;
    for (int i = 0; i < 12; ++i) {
        klog("[ticker] tick=%u  ms=%u\n", (unsigned)k_ticks(), (unsigned)k_millis());
        k_sleep_ms(25);
    }
    klog("[ticker] done\n");
}

// Exercises the allocator the kernel now owns.
static void IRAM_ATTR heap_task(void *arg)
{
    (void)arg;
    void *a = kmalloc(1024);
    void *b = kmalloc(2048);
    void *c = kmalloc(512);
    klog("[heap] a=%p b=%p c=%p\n", a, b, c);
    kheap_dump();

    kfree(b);                      // leaves a hole between a and c
    klog("[heap] after freeing the middle block:\n");
    kheap_dump();

    void *d = kmalloc(2048);       // should land back in that hole
    klog("[heap] reallocated 2048 -> %p (%s)\n",
         d, d == b ? "reused the hole" : "went elsewhere");

    kfree(a); kfree(c); kfree(d);
    klog("[heap] after freeing everything (coalescing should give one block):\n");
    kheap_dump();
}

// Hands work to core 0 across the mailbox.
static DRAM_ATTR char s_msg[80];
static DRAM_ATTR uint32_t s_numbers[16];

static void IRAM_ATTR mbox_task(void *arg)
{
    (void)arg;

    // printf is off limits on this core, so borrow core 0's.
    for (int i = 0; i < (int)sizeof(s_msg) - 1; ++i) s_msg[i] = 0;
    const char *src = "printed with a real printf, by the other core";
    int n = 0;
    while (src[n] && n < (int)sizeof(s_msg) - 1) { s_msg[n] = src[n]; n++; }
    s_msg[n] = 0;

    if (!kmbox_call(K_JOB_LOG, (uint32_t)(uintptr_t)s_msg, 0, 0, nullptr)) {
        klog("[mbox] log job was not accepted\n");
    }

    klog("[mbox] log job answered, posting the compute job\n");

    // And a compute job, to show the result path.
    uint32_t expect = 0;
    for (uint32_t i = 0; i < 16; ++i) { s_numbers[i] = i * i; expect += i * i; }

    uint32_t got = 0;
    if (kmbox_call(K_JOB_SUM, (uint32_t)(uintptr_t)s_numbers, 16, 0, &got)) {
        klog("[mbox] core 0 summed 16 squares -> %u (expected %u) %s\n",
             (unsigned)got, (unsigned)expect, got == expect ? "OK" : "MISMATCH");
    } else {
        klog("[mbox] sum job failed\n");
    }
}


// The whole point of preemption, in one task.
//
// It never calls k_yield and never sleeps. Under the cooperative scheduler it
// would own core 1 outright for as long as it wanted, and nothing else would
// run. If the ticker below keeps printing while this spins, the tick is taking
// the CPU away from it against its will - which is the definition.
static volatile DRAM_ATTR uint32_t s_hog_loops;

static void IRAM_ATTR hog_task(void *arg)
{
    (void)arg;
    klog("[hog] spinning for 200 ms and never yielding - watch the ticker\n");

    uint32_t start = k_ticks();
    while (k_ticks() - start < 200) {
        s_hog_loops = s_hog_loops + 1;
    }

    klog("[hog] done: %u iterations, and I never once gave up the core\n",
         (unsigned)s_hog_loops);
}

// Runs last: gives the others room to finish, then reports.
static void IRAM_ATTR idle_task(void *arg)
{
    (void)arg;
    k_sleep_ms(450);
    klog("[idle] everyone else should be done\n");
    ktop();
    kheap_dump();
    klog("[idle] kernel demo complete after %u ticks\n", (unsigned)k_ticks());
}


// A deliberately minimal pair for bisecting the preemption bring-up: no sleep,
// no recursion, no mailbox, no console lock in the hot path. Just two tasks
// counting on their own stacks while the tick throws them at each other.
static volatile DRAM_ATTR uint32_t s_soak_a, s_soak_b;

static void IRAM_ATTR soak_task(void *arg)
{
    volatile uint32_t *counter = (volatile uint32_t *)arg;
    uint32_t local = 0;
    for (uint32_t i = 0; i < 400000; ++i) {
        local++;
        *counter = local;          // visible to core 0 without any printing
    }
    klog("[soak] %p finished, local=%u\n", arg, (unsigned)local);
}


// --- stage 2.7: the synchronisation primitives, under real contention -------

// The mutex test is the one that matters. Two tasks, both cores, preemption on,
// each adding to the same counter 200 times. Without mutual exclusion the
// increments interleave and the total comes out short; with it the answer is
// exactly 400 every time. That is the whole claim, checked rather than asserted.
static DRAM_ATTR k_mutex_t s_counter_mutex;
static volatile DRAM_ATTR uint32_t s_shared_counter;

static void IRAM_ATTR mutex_task(void *arg)
{
    const char *name = (const char *)arg;
    for (int i = 0; i < 200; ++i) {
        k_mutex_lock(&s_counter_mutex);

        // Read-modify-write with a yield in the middle, which is the worst
        // case this is supposed to survive: the lock has to hold across a
        // context switch, not merely across a single instruction.
        uint32_t v = s_shared_counter;
        k_yield();
        s_shared_counter = v + 1;

        k_mutex_unlock(&s_counter_mutex);
    }
    klog("[%s] done, counter now %u\n", name, (unsigned)s_shared_counter);
}

// Producer and consumer over a blocking queue. The queue is deliberately
// smaller than the message count so the producer has to block on full and the
// consumer on empty - both wait paths get exercised.
#define Q_ITEMS 8
static DRAM_ATTR uint32_t  s_q_storage[Q_ITEMS];
static DRAM_ATTR k_queue_t s_queue;

#define Q_SENTINEL 0xFFFFFFFFu

static void IRAM_ATTR producer_task(void *arg)
{
    (void)arg;
    for (uint32_t i = 1; i <= 50; ++i) {
        k_queue_send(&s_queue, &i);
    }
    uint32_t end = Q_SENTINEL;
    k_queue_send(&s_queue, &end);
    klog("[producer] sent 1..50 through a queue of %d\n", Q_ITEMS);
}

static void IRAM_ATTR consumer_task(void *arg)
{
    (void)arg;
    uint32_t sum = 0, got = 0, item = 0;
    for (;;) {
        k_queue_recv(&s_queue, &item);
        if (item == Q_SENTINEL) break;
        sum += item;
        got++;
    }
    // 1 + 2 + ... + 50
    klog("[consumer] received %u items, sum %u (expected 1275) %s\n",
         (unsigned)got, (unsigned)sum, sum == 1275 ? "OK" : "MISMATCH");
}


#ifdef S3_TEST_STACK_OVERFLOW
// Overruns its stack by a measured amount, then reaches a scheduling point so
// the guard has somewhere to fire.
//
// THE SIZE IS NOT ARBITRARY. A task that overruns still has to be able to be
// switched out, and switching costs roughly 200 bytes BELOW the stack pointer:
// base save areas for each live window, the tick interrupt frame if the tick
// lands here, and k_yield's own frames. The first version of this test used a
// local big enough to leave ten bytes of headroom, so the context switch wrote
// straight past the allocation and into the next heap block's header - the heap
// chain then looped forever and core 0 wedged with its watchdog firing. The
// guard was never the problem; the test was.
//
// K_STACK_SIZE - 64 puts the stack pointer about 32 bytes past the end of the
// usable stack: far enough to clobber the guard word, with the rest of the
// 256-byte run-off area left for the switch to land in.
static void IRAM_ATTR overflow_task(void *arg)
{
    (void)arg;
    klog("[overflow] overrunning a %d byte stack on purpose\n",
         (int)K_STACK_SIZE);

    // Write the whole thing, the way a runaway recursion or an oversized local
    // actually would. Touching only the two ends missed the guard word - it
    // lives four bytes below the usable stack, which is pad[28..31] here - and
    // the first run of this test reported nothing for exactly that reason.
    volatile uint8_t pad[K_STACK_SIZE - 64];
    for (size_t i = 0; i < sizeof(pad); ++i) pad[i] = (uint8_t)i;

    k_yield();      // the guard is checked on the way out
    klog("[overflow] still running; guard should have flagged task %d\n",
         (int)(k_current() ? k_current()->id : -1));
}
#endif

#ifdef S3_TEST_SLEEP_CLAIM
// Regression test for a scheduler race found while verifying the -O2 switch.
//
// A task that sleeps with nothing else to run on its core does not switch
// away: k_yield idles in waiti inside the task's own context, with the
// scheduler lock dropped. When the deadline passes the task is marked READY -
// and the other core's pick_next could claim it, switching to the stack pointer
// saved at its LAST REAL SWITCH. That is a stale context: the same task would
// then be running on two cores at once.
//
// The flag is held from before each sleep until after it. One copy of this
// task can never find it taken; a second copy, resumed from a stale context,
// arrives while the first is still asleep holding it - and is counted.
static DRAM_ATTR k_spin_t          s_claim_flag;
static volatile DRAM_ATTR uint32_t s_claim_iters, s_claim_double, s_claim_cores[2];

static void IRAM_ATTR claim_task(void *arg)
{
    (void)arg;
    const uint32_t N = 3000;

    for (uint32_t i = 0; i < N; ++i) {
        if (!k_spin_try(&s_claim_flag)) {
            s_claim_double = s_claim_double + 1;
            k_sleep_ms(1);
            continue;
        }
        k_sleep_ms(1);
        uint32_t core = esp_cpu_get_core_id() & 1;
        s_claim_cores[core] = s_claim_cores[core] + 1;
        s_claim_iters = s_claim_iters + 1;
        k_spin_unlock(&s_claim_flag);
    }

    klog("[claim] %u sleeps, %u on core 0 / %u on core 1, %u times a second copy "
         "of this task was running %s\n",
         (unsigned)s_claim_iters, (unsigned)s_claim_cores[0],
         (unsigned)s_claim_cores[1], (unsigned)s_claim_double,
         s_claim_double ? "!! RACE" : "OK");
}
#endif

#ifdef S3_TEST_MBOX_STRESS
// Stage 4.1 - four posters, free to run on either core, all hammering the
// mailbox at once.
//
// Each call sums an array whose first element is unique to this poster and
// this iteration, so the answer identifies the payload it was computed from.
// If two posters ever end up in the same slot, one of them is handed the
// other's sum - a wrong answer, counted - or waits on a slot the other has
// already released, which surfaces as a failed call.
#define STRESS_POSTERS 4
#define STRESS_CALLS   250
#define STRESS_TIMEOUTS 10
#define STRESS_AFTER    50

static DRAM_ATTR uint32_t          s_stress_v[STRESS_POSTERS][4];
static volatile DRAM_ATTR uint32_t s_stress_finished;

static void IRAM_ATTR mbox_stress_task(void *arg)
{
    const uint32_t id = (uint32_t)(uintptr_t)arg;
    uint32_t *v = s_stress_v[id];
    uint32_t ok = 0, wrong = 0, failed = 0;
    uint32_t cores[2] = { 0, 0 };

    for (uint32_t i = 0; i < STRESS_CALLS; ++i) {
        v[0] = id * 100000u + i;
        v[1] = 1; v[2] = 2; v[3] = 3;

        uint32_t got = 0;
        cores[esp_cpu_get_core_id() & 1]++;
        if (!kmbox_call(K_JOB_SUM, (uint32_t)(uintptr_t)v, 4, 0, &got)) {
            failed++;
            continue;
        }
        if (got == v[0] + 6) ok++;
        else                 wrong++;
    }

    // Timeouts, on purpose: a 20 ms job with a 5 ms deadline. Depending on
    // whether core 0 has picked it up yet, each one is withdrawn (POSTED ->
    // EMPTY) or abandoned mid-job (RUNNING -> ABANDONED, freed by core 0).
    uint32_t timed_out = 0;
    for (uint32_t i = 0; i < STRESS_TIMEOUTS; ++i) {
        if (!kmbox_call_timeout(K_JOB_SLEEP, 20, 0, 0, nullptr, 5)) timed_out++;
    }

    // And the mailbox must be fully healthy afterwards: an abandoned slot that
    // came back wrong would hand these calls someone else's answer.
    uint32_t after_ok = 0;
    for (uint32_t i = 0; i < STRESS_AFTER; ++i) {
        v[0] = id * 100000u + 50000u + i;
        uint32_t got = 0;
        if (kmbox_call(K_JOB_SUM, (uint32_t)(uintptr_t)v, 4, 0, &got) &&
            got == v[0] + 6) {
            after_ok++;
        }
    }

    klog("[stress%u] sums %u right / %u wrong / %u failed (from core %u); "
         "timeouts %u of %u; after them %u of %u right\n",
         (unsigned)id, (unsigned)ok, (unsigned)wrong, (unsigned)failed,
         (unsigned)(cores[1] ? 1 : 0), (unsigned)timed_out,
         (unsigned)STRESS_TIMEOUTS, (unsigned)after_ok, (unsigned)STRESS_AFTER);

    // The last one out checks the mailbox came back clean. The count itself
    // is bumped with k_cas: two posters on two cores finishing together would
    // lose an increment with a plain read-modify-write.
    uint32_t n;
    do { n = s_stress_finished; } while (!k_cas(&s_stress_finished, n, n + 1));
    if (n + 1 == STRESS_POSTERS) {
        k_sleep_ms(300);        // let core 0 finish any abandoned jobs
        uint32_t e, p, d, b;
        kmbox_census(&e, &p, &d, &b);
        klog("[stress] mailbox after: empty=%u posted=%u done=%u busy=%u; "
             "%u withdrawn + %u abandoned = %u of %u timeouts %s\n",
             (unsigned)e, (unsigned)p, (unsigned)d, (unsigned)b,
             (unsigned)kmbox_withdrawn_count(), (unsigned)kmbox_abandoned_count(),
             (unsigned)(kmbox_withdrawn_count() + kmbox_abandoned_count()),
             (unsigned)(STRESS_POSTERS * STRESS_TIMEOUTS),
             (e == KMBOX_SLOTS &&
              kmbox_withdrawn_count() + kmbox_abandoned_count() ==
                  STRESS_POSTERS * STRESS_TIMEOUTS) ? "OK" : "!! LEAK");
    }
}
#endif

#ifdef S3_TEST_EVBUS
// Stage 4.2b - the bus under real load, with the kernel running.
//
//   - one publisher pinned to each core, 2000 events each, at the same time
//   - two subscribers blocking in evbus_recv(), each checking that every
//     publisher's events arrive in order with nothing missing or repeated, and
//     that both saw exactly the same events (same ids)
//   - one subscriber that never reads at all: it must drop, and nobody else
//     may notice
//   - a 20 ms timer, checked for lateness against the kernel clock, and a 5 ms
//     timer whose service is deliberately held off for 50 ms - which must come
//     back as ONE event saying ~10 periods passed, not a burst of ten
#define BUS_PUBS      2
#define BUS_PER_PUB   2000

// Occupies a task id and leaves at once. Sixteen of these go first, so every
// task in this test lands on id 16 or above - the ids that only exist since
// the wait sets were widened from 16 bits to 32. Blocking in evbus_recv() is
// what exercises them: a waiter's bit is 1 << id.
static void IRAM_ATTR bus_filler_task(void *arg) { (void)arg; }
#define BUS_SUB_Q     64

static DRAM_ATTR ev_t s_bus_q1[BUS_SUB_Q], s_bus_q2[BUS_SUB_Q], s_bus_stall_q[8];
static DRAM_ATTR ev_t s_bus_tq[16];
static volatile DRAM_ATTR int s_bus_s1 = -1, s_bus_s2 = -1, s_bus_stall = -1;

static void IRAM_ATTR bus_pub_task(void *arg)
{
    const uint32_t id = (uint32_t)(uintptr_t)arg;
    k_sleep_ms(20);                 // let both subscribers get into evbus_recv
    for (uint32_t i = 0; i < BUS_PER_PUB; ++i) {
        evbus_publish(EV_APP, (uint8_t)(EV_SRC_TEST + id), (id << 16) | i, i * 7u + id);
        // Paced so that a subscriber that is keeping up never overflows: a
        // burst of 16 from each core is 32 events, half of a 64-slot queue.
        if ((i & 15) == 15) k_sleep_ms(1);
    }
    evbus_publish(EV_SYS, (uint8_t)(EV_SRC_TEST + id), 0xD0E, id);
}

static void IRAM_ATTR bus_sub_task(void *arg)
{
    const int which = (int)(uintptr_t)arg;
    const int sub   = which == 1 ? s_bus_s1 : s_bus_s2;

    uint32_t next[BUS_PUBS] = { 0, 0 };
    uint32_t got = 0, out_of_order = 0, bad_payload = 0, done = 0, id_sum = 0;
    uint32_t cores[2] = { 0, 0 };

    while (done < BUS_PUBS) {
        ev_t ev;
        evbus_recv(sub, &ev);
        cores[esp_cpu_get_core_id() & 1]++;
        if (ev.type == EV_SYS) { done++; continue; }

        const uint32_t pub = ev.a >> 16, i = ev.a & 0xFFFF;
        if (pub >= BUS_PUBS || i != next[pub]) out_of_order++;
        if (pub < BUS_PUBS) next[pub] = i + 1;
        if (ev.b != i * 7u + pub) bad_payload++;
        id_sum += ev.id;
        got++;
    }

    evbus_stats_t st = evbus_stats(sub);
    klog("[bus s%d] task id %u: %u events, %u out of order, %u bad payloads, %u dropped, "
         "id sum %u (ran %u on core 0, %u on core 1) %s\n",
         which, (unsigned)k_current()->id, (unsigned)got, (unsigned)out_of_order, (unsigned)bad_payload,
         (unsigned)st.dropped, (unsigned)id_sum, (unsigned)cores[0],
         (unsigned)cores[1],
         (got == BUS_PUBS * BUS_PER_PUB && !out_of_order && !bad_payload &&
          !st.dropped) ? "OK" : "!! FAIL");

    if (which == 2) {
        evbus_stats_t ss = evbus_stats(s_bus_stall);
        klog("[bus stall] never read: %u delivered, %u dropped (of %u) %s\n",
             (unsigned)ss.delivered, (unsigned)ss.dropped,
             (unsigned)(BUS_PUBS * BUS_PER_PUB + BUS_PUBS),
             (ss.delivered == 8 &&
              ss.delivered + ss.dropped == BUS_PUBS * BUS_PER_PUB + BUS_PUBS) ? "OK" : "!! FAIL");
    }
}

// Pinned to core 1 with the timer service: holding preemption here only
// keeps the service off the CPU if the two share a core.
static void IRAM_ATTR bus_timer_check_task(void *arg)
{
    (void)arg;
    const int sub = evbus_subscribe("tmr", 0, s_bus_tq, 16);   // mask 0: timers only

    // Accuracy: 100 periods of 20 ms against the kernel clock. Signed, because
    // t0 and the service's own reading of the clock can straddle a tick, which
    // makes -1 ms a real answer rather than an overflow.
    int tm = evbus_timer_start(sub, 20, 1);
    const uint32_t t0 = k_millis();
    uint32_t periods = 0, multi = 0;
    int32_t  late_min = 1000, late_max = -1000, late_sum = 0;
    // Stage 5.3: how long core 1 was parked (core 0 writing flash) in the
    // period before each event - the timer service cannot run while it is.
    k_cpu_time_t ct;
    k_cpu_time(1, &ct);
    //
    // Since stage 4.5 the system log writes flash every couple of seconds, and
    // one sector erase parks core 1 for ~30 ms; the timer service cannot run
    // then, and first runs of this after 5.3 saw events 13-17 ms late. So the
    // timer is held to lateness BEYOND the park before each event, and an event
    // standing for two periods is only accepted where a park that long explains
    // it. Both figures are printed.
    uint64_t park_prev = ct.park;
    uint32_t worst_park_us = 0, unexplained_multi = 0;
    int32_t  excess_max = -1000;
    for (int n = 0; n < 100; ++n) {
        ev_t ev;
        evbus_recv(sub, &ev);
        k_cpu_time(1, &ct);
        const uint32_t park_us = (uint32_t)((ct.park - park_prev) / 240);
        park_prev = ct.park;
        periods += ev.b;
        if (ev.b > 1) {
            multi++;
            if (park_us < 20000u * (ev.b - 1)) unexplained_multi++;
        }
        const int32_t late = (int32_t)(ev.t_ms - (t0 + 20 * periods));
        const int32_t excess = late - (int32_t)(park_us / 1000);
        late_sum += late;
        if (late < late_min) late_min = late;
        if (late > late_max) { late_max = late; worst_park_us = park_us; }
        if (excess > excess_max) excess_max = excess;
    }
    evbus_timer_stop(tm);
    klog("[bus timer] 100 x 20 ms: late by %d to %d ms (sum %d), core 1 parked %u us before "
         "the latest; beyond parks at most %d ms; %u events for 2+ periods, %u not explained "
         "by a park %s\n",
         (int)late_min, (int)late_max, (int)late_sum, (unsigned)worst_park_us, (int)excess_max,
         (unsigned)multi, (unsigned)unexplained_multi,
         (late_min >= -1 && excess_max <= 2 && unexplained_multi == 0) ? "OK" : "!! CHECK");

    // Coalescing: a 5 ms timer, and then this task sits on core 1 for 50 ms
    // with preemption held, so the timer service - also on core 1 - cannot run.
    { ev_t junk; while (evbus_try_recv(sub, &junk)) {} }
    tm = evbus_timer_start(sub, 5, 2);
    k_sleep_ms(12);                 // two normal ticks first
    k_preempt_hold();
    esp_rom_delay_us(50000);
    k_preempt_release();
    k_sleep_ms(3);
    evbus_timer_stop(tm);

    uint32_t events = 0, total = 0, biggest = 0;
    ev_t ev;
    while (evbus_try_recv(sub, &ev)) {
        events++;
        total += ev.b;
        if (ev.b > biggest) biggest = ev.b;
    }
    klog("[bus timer] 5 ms timer held off 50 ms: %u events for %u periods, the "
         "late one standing for %u %s\n",
         (unsigned)events, (unsigned)total, (unsigned)biggest,
         (biggest >= 9 && events <= 5) ? "OK" : "!! CHECK");
    evbus_unsubscribe(sub);
}
#endif

#if defined(S3_TEST_PARK) || defined(S3_TEST_PARK_OFF)
static void flashwork_task(void *arg);      // stage 4.3a, defined further down
#endif

// --- stage 2.6b: core 0 joins the same scheduler ---------------------------

// A kernel task pinned to core 0 whose entire job is to give the slice back.
//
// Core 0 still owes FreeRTOS its USB console, its flash driver and the mailbox
// worker. Donating the core to our scheduler outright would starve all three,
// so the kernel keeps one task there that hands the time straight back and then
// asks for it again. It is the price of borrowing a core that already has a job.
static volatile DRAM_ATTR uint32_t s_cpu0_loops;

static void IRAM_ATTR cpu0_idle_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(1);      // FreeRTOS gets core 0 for a tick
        s_cpu0_loops = s_cpu0_loops + 1;

        k_yield();          // then our scheduler gets first refusal again
    }
}

// CPU-bound on purpose, and that is the point.
//
// A task that sleeps is never in the ready queue when the other core comes
// looking, so the first version of this - which slept 7 ms between rounds -
// ran entirely on core 1 and proved nothing. Two cores only visibly share work
// when there is work sitting there to be shared.
static void IRAM_ATTR smp_task(void *arg)
{
    const char *name = (const char *)arg;
    uint32_t seen0 = 0, seen1 = 0;
    volatile uint32_t acc = 0;

    for (int round = 0; round < 200; ++round) {
        uint32_t core = esp_cpu_get_core_id();
        if (core == 0) seen0++; else seen1++;

        for (uint32_t i = 0; i < 100000; ++i) acc = acc + i;  // ~1 ms of real work
        k_yield();
    }
    klog("[%s] %u rounds on core 0, %u on core 1\n",
         name, (unsigned)seen0, (unsigned)seen1);
}

// The FreeRTOS task that donates core 0. Pinned there, and at a high priority
// so that the donation actually happens rather than waiting behind IDF's
// housekeeping.
static void smp_host_task(void *arg)
{
    (void)arg;
    logln("[core0] joining the kernel's scheduler");
    for (;;) {
        k_smp_join();       // returns only while there is nothing it may run
        vTaskDelay(1);
    }
}

static void IRAM_ATTR core1_main(void)
{
    klog("[core1] vector table is ours, faults are recorded not silent\n");

#ifdef S3_TEST_CORE1_FAULT
    klog("[core1] deliberately dereferencing NULL to test the fault path\n");
    volatile uint32_t *boom = (volatile uint32_t *)0;
    (void)*boom;
    klog("[core1] !! still running - the fault path did NOT trigger\n");
#endif

    klog("[core1] bringing up the kernel\n");
    k_init();                       // also brings up the kernel's own heap
    kpark_init_core1();             // before the tick: interrupts open with it
    k_tick_start(1000);             // CCOMPARE0, 1 ms, this core only

#ifdef S3_PREEMPT_SOAK
    // Bisection build: the smallest thing that can prove preemption works.
    k_task_create("soakA", soak_task, (void *)&s_soak_a);
    k_task_create("soakB", soak_task, (void *)&s_soak_b);
#elif defined(S3_TEST_PARK) || defined(S3_TEST_PARK_OFF)
    // Flash-resident code on core 1, pinned there, never yielding - the
    // workload that needs the park. Preemption keeps the kernel's own tasks
    // running around it.
    k_task_create_on("flashwork", flashwork_task, nullptr, 1);
#elif defined(S3_TEST_EVBUS)
    for (int i = 0; i < 16; ++i) k_task_create("filler", bus_filler_task, nullptr);
    k_task_create_pri("evtimer", evbus_timer_task, nullptr, 1, K_PRIO_HIGH);
    const uint32_t bus_mask = EV_MASK(EV_APP) | EV_MASK(EV_SYS);
    s_bus_s1    = evbus_subscribe("s1",    bus_mask, s_bus_q1, BUS_SUB_Q);
    s_bus_s2    = evbus_subscribe("s2",    bus_mask, s_bus_q2, BUS_SUB_Q);
    s_bus_stall = evbus_subscribe("stall", bus_mask, s_bus_stall_q, 8);
    k_task_create("bus_s1", bus_sub_task, (void *)1);
    k_task_create("bus_s2", bus_sub_task, (void *)2);
    k_task_create_on("bus_p0", bus_pub_task, (void *)0, 0);
    k_task_create_on("bus_p1", bus_pub_task, (void *)1, 1);
    k_task_create_on("bus_tmr", bus_timer_check_task, nullptr, 1);
#ifdef S3_ENABLE_SMP
    k_task_create_on("cpu0idle", cpu0_idle_task, nullptr, 0);
#endif
#elif defined(S3_TEST_MBOX_STRESS)
    // Two posters pinned to each core. Left free, they all end up posting from
    // core 1: core 0 only serves the mailbox while it is OUT of the kernel, so
    // core 1 is always the one to see an answer land and post the next job.
    for (uint32_t i = 0; i < STRESS_POSTERS; ++i) {
        k_task_create_on("stress", mbox_stress_task, (void *)(uintptr_t)i,
                         (uint8_t)(i < STRESS_POSTERS / 2 ? 1 : 0));
    }
#ifdef S3_ENABLE_SMP
    k_task_create_on("cpu0idle", cpu0_idle_task, nullptr, 0);
#endif
#elif defined(S3_TEST_SLEEP_CLAIM)
    // One sleeper free to run anywhere, alone on core 1, and core 0 inside the
    // scheduler beside it - the exact arrangement the race needs.
    k_task_create("claim", claim_task, nullptr);
#ifdef S3_ENABLE_SMP
    k_task_create_on("cpu0idle", cpu0_idle_task, nullptr, 0);
#endif
#else
    // Created first, on purpose. The table holds K_MAX_TASKS (16) and the demo
    // below already fills 15, so a debug flag that adds one more would silently
    // lose whichever task came last. Losing a demo task is a shorter log;
    // losing this one is a dead keypad.
    //
    // Pinned to core 1 - the kernel's own core, sampling on the kernel's own
    // tick - and HIGH, because it sleeps nearly all the time and has to run
    // on time when it wakes.
    if (k_task_create_pri("input", input_task, nullptr, 1, K_PRIO_HIGH) < 0) {
        klog("[core1] !! no task slot for the input sampler\n");
    }
    // The event bus's timers. Same reasoning: a system service, on the
    // kernel's core, and HIGH so a timer fires on time rather than when the
    // demo tasks happen to let it.
    if (k_task_create_pri("appmgr", app_manager_task, nullptr, 1, K_PRIO_HIGH) < 0) {
        klog("[core1] !! no task slot for the app manager\n");
    }
    if (k_task_create_pri("evtimer", evbus_timer_task, nullptr, 1, K_PRIO_HIGH) < 0) {
        klog("[core1] !! no task slot for the event bus timers\n");
    }
    k_task_create("alpha",  demo_task,   (void *)"alpha");
    k_task_create("beta",   demo_task,   (void *)"beta");
    k_task_create("deep",   deep_task,   nullptr);
    // High: it is the task whose whole purpose is to be responsive.
    k_task_create_pri("ticker", ticker_task, nullptr, K_CPU_ANY, K_PRIO_HIGH);
    k_task_create("heap",   heap_task,   nullptr);
    k_task_create("mbox",   mbox_task,   nullptr);
    k_task_create("hog",    hog_task,    nullptr);
    k_mutex_init(&s_counter_mutex, "counter");
    k_queue_init(&s_queue, s_q_storage, sizeof(uint32_t), Q_ITEMS, "demoq");
    k_task_create("muxA",     mutex_task,    (void *)"muxA");
    k_task_create("muxB",     mutex_task,    (void *)"muxB");
    k_task_create("producer", producer_task, nullptr);
    k_task_create("consumer", consumer_task, nullptr);

    // Low: the reporter should genuinely be last, not last by luck.
#ifdef S3_TEST_STACK_OVERFLOW
    k_task_create("overflow", overflow_task, nullptr);
#endif
    k_task_create_pri("idle", idle_task, nullptr, K_CPU_ANY, K_PRIO_LOW);
#ifdef S3_ENABLE_SMP
    // Pinned to core 0 so core 1 never picks it up - and deliberately at
    // NORMAL, not a low priority. It is the only thing handing core 0 back to
    // FreeRTOS, so starving it would take the USB console and the flash driver
    // down with it. "Idle" is its job, not its rank.
    k_task_create_on("cpu0idle", cpu0_idle_task, nullptr, 0);
    k_task_create("smpA", smp_task, (void *)"smpA");
    k_task_create("smpB", smp_task, (void *)"smpB");
#endif
#endif

    // From here the tick does not merely ask for a reschedule, it performs one.
    // On by default now. A task that never yields no longer owns the core: the
    // demo's hog task spins for 200 ms without a single yield while the ticker
    // keeps printing every 25 ms right through it.
#ifdef S3_ENABLE_PREEMPT
    k_preempt_enable();
#endif

    klog("[core1] starting the scheduler - nothing below this line is IDF\n");
    k_start();

    klog("[core1] scheduler returned (it should not)\n");
}

static void wake_core1(void)
{
    logln();
    logln("--- stage 2.1-2.3: core 1 and the kernel ---");

    kmbox_init();                   // core 0 owns setting this up
    // The kernel's memory, taken from ESP-IDF here - on core 0, before core 1
    // exists - never by the kernel itself (stage 5.3c; see kheap_reserve).
    if (!kheap_reserve()) {
        logln("[core0] !! could not reserve the kernel's memory pools");
    } else {
        // Stage 6.3: how much it got, because since the radio was linked in it
        // is not always the whole KHEAP_INTERNAL_SIZE.
        kheap_stats_t in, ps;
        kheap_stats_pool(KHEAP_INTERNAL, &in);
        kheap_stats_pool(KHEAP_PSRAM, &ps);
        logf("[core0] kernel pools: %u KB internal (asked %u), %u KB PSRAM\n",
             (unsigned)((in.total + in.blocks * 16) / 1024),
             (unsigned)(KHEAP_INTERNAL_SIZE / 1024),
             (unsigned)((ps.total + ps.blocks * 16) / 1024));
    }
    bool ok = kcore1_start(core1_main, 500);

    // Core 1 prints through the ROM console with no locking, so give its
    // output a moment to drain before core 0 talks over it.
    hal_delay_ms(20);

    if (ok) {
        logln("[core0] core 1 answered - it is running our code now");
#ifdef S3_ENABLE_SMP
        // Donate core 0 straight away, while there is still a demo to share.
        // Pinned to core 0 and high priority so the donation actually happens
        // instead of queueing behind IDF's housekeeping.
        xTaskCreatePinnedToCore(smp_host_task, "s3os_host", 4096, nullptr,
                                10, nullptr, 0);
#endif
        // Serve core 1's mailbox while its demo runs, and watch for a fault.
        for (int i = 0; i < 400; ++i) {
            kmbox_service();
            if (s3_chain_end_hit) {
                logln("[core0] !! core 1 unwound its register-window chain off the");
                logln("        bottom of a task stack - k_stack_init got it wrong");
                break;
            }
            if (kfault_pending()) { kfault_report(); break; }
            hal_delay_ms(2);
        }
#ifdef S3_ENABLE_SMP
        logf("[core0] cpu0idle asked the scheduler %lu times; spin stuck on 0x%08lx\n",
             (unsigned long)s_cpu0_loops, (unsigned long)s3_spin_stuck);
#endif
        logf("[core0] preemption trace (%lu entries):\n",
             (unsigned long)k_trace_count());
        for (uint32_t i = 0; i < k_trace_count(); ++i) {
            const k_trace_t *e = k_trace_at(i);
            if (!e) break;
            logf("   #%2lu t=%-4lu pc=0x%08lx sp=0x%08lx task=0x%08lx hold=%lu state=%lu\n",
                 (unsigned long)i, (unsigned long)e->tick,
                 (unsigned long)e->pc, (unsigned long)e->sp,
                 (unsigned long)e->task, (unsigned long)e->hold,
                 (unsigned long)e->state);
        }

        uint32_t mb_e = 0, mb_p = 0, mb_d = 0, mb_b = 0;
        kmbox_census(&mb_e, &mb_p, &mb_d, &mb_b);
        logf("[core0] served %lu job(s); slots now empty=%lu posted=%lu done=%lu busy=%lu\n",
             (unsigned long)kmbox_served_count(),
             (unsigned long)mb_e, (unsigned long)mb_p, (unsigned long)mb_d,
             (unsigned long)mb_b);
        // The tick lives entirely inside core 1, so the only proof it is
        // running is watching its counter move from over here.
        uint32_t t1 = s3_tick_count;
        hal_delay_ms(100);
        logf("[core0] core 1 tick counter: %lu -> %lu over 100 ms (%s)\n",
             (unsigned long)t1, (unsigned long)s3_tick_count,
             (s3_tick_count > t1) ? "CCOMPARE0 interrupt is firing"
                                  : "NOT ticking - the timer interrupt never fired");
    } else {
        logln("[core0] !! core 1 never reported in.");
        logln("        The wake sequence did not take - stop here rather than");
        logln("        building the kernel on a core that is not actually ours.");
    }
}

static fb_t s_fb;

fb_t *system_fb(void) { return &s_fb; }

static void alloc_framebuffer(void)
{
    if (!fb_init(&s_fb, LCD_WIDTH, LCD_HEIGHT)) return;
    logf("framebuffer: %ux%u RGB565 in PSRAM, %u bytes at %p\n",
         s_fb.w, s_fb.h, (unsigned)s_fb.bytes, (void *)s_fb.px);
    logf("             PSRAM left %lu bytes\n",
         (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

// --- stage 3.2: is PSRAM fast enough to draw into? -------------------------
//
// Moving the framebuffer off-chip only helps if filling it is cheaper than
// sending it. If a full-screen fill in PSRAM costs more than the 20 ms the SPI
// transfer takes, the bottleneck has simply moved and the compositor would be
// waiting on memory instead of on the panel.
static void gfx_bringup(void)
{
    if (!s_fb.px) return;

    logln();
    logln("--- stage 3.2: framebuffer in PSRAM ---");

    int64_t t0 = hal_micros();
    fb_clear(&s_fb, FB_BLACK);
    double memset_ms = (hal_micros() - t0) / 1000.0;

    t0 = hal_micros();
    fb_clear(&s_fb, FB_BLUE);
    double write_ms = (hal_micros() - t0) / 1000.0;

    t0 = hal_micros();
    fb_fill_rect(&s_fb, 20, 20, 200, 120, FB_RED);
    double rect_ms = (hal_micros() - t0) / 1000.0;

    logf("  clear (memset path) : %6.2f ms  %5.1f MB/s\n",
         memset_ms, (s_fb.bytes / 1e6) / (memset_ms / 1000.0));
    logf("  clear (pixel loop)  : %6.2f ms  %5.1f MB/s   <- the real cost\n",
         write_ms, (s_fb.bytes / 1e6) / (write_ms / 1000.0));
    logf("  fill 200x120        : %6.2f ms\n", rect_ms);

    // Now the way out. Staged copies PSRAM into internal RAM first; direct asks
    // the DMA engine to read PSRAM itself, which is worth a great deal if it
    // works at all.
    t0 = hal_micros();
    fb_present(&s_fb);
    double staged_ms = (hal_micros() - t0) / 1000.0;

    t0 = hal_micros();
    lcd_blit_wire_direct(0, 0, s_fb.w, s_fb.h, (const uint8_t *)s_fb.px,
                         (size_t)s_fb.w * 2);
    double direct_ms = (hal_micros() - t0) / 1000.0;

    logf("  present, staged     : %6.2f ms  %5.1f FPS\n",
         staged_ms, 1000.0 / staged_ms);
    logf("  present, DMA from PSRAM: %6.2f ms  %5.1f FPS\n",
         direct_ms, 1000.0 / direct_ms);
    logln("  (if the screen is right after this line, DMA reads PSRAM fine)");
}


// A screen designed to be readable off a bad photograph.
//
// The demo scene could not settle what was wrong, because every candidate
// explanation - inversion, swapped colour order, a rotation problem - would
// change several things at once and the picture was too dim to tell them apart.
// This draws four unmistakable things instead, each of which isolates exactly
// one question.
static void gfx_selftest(void)
{
    if (!s_fb.px) return;

    const int bw = 70, bh = 50;

    fb_clear(&s_fb, FB_BLACK);
    fb_fill_rect(&s_fb, 0,                0,                bw, bh, FB_RED);
    fb_fill_rect(&s_fb, s_fb.w - bw,      0,                bw, bh, FB_GREEN);
    fb_fill_rect(&s_fb, 0,                s_fb.h - bh,      bw, bh, FB_BLUE);
    fb_fill_rect(&s_fb, s_fb.w - bw,      s_fb.h - bh,      bw, bh, FB_WHITE);

    // A bar across the middle that is obviously not square, so a rotation or a
    // stride problem cannot hide as "it looks about right".
    fb_fill_rect(&s_fb, 0, s_fb.h / 2 - 8, s_fb.w, 16, FB_YELLOW);

    fb_present(&s_fb);

    logln();
    logln("--- gfx self-test: what is on the glass? ---");
    logln("  background        : BLACK");
    logln("  top-left  70x50   : RED");
    logln("  top-right 70x50   : GREEN");
    logln("  bottom-left       : BLUE");
    logln("  bottom-right      : WHITE");
    logln("  wide bar, middle  : YELLOW, full width, 16 px tall");
    logln("Anything different tells us which of these is wrong:");
    logln("  background white       -> inversion (drop 0x21 from init_st7789)");
    logln("  red and blue swapped   -> colour order (s_bgr)");
    logln("  corners in wrong spots -> rotation / MADCTL");
    logln("  bar is tall and narrow -> rotation, width and height transposed");
}

// Draws something with every primitive, so the clipping and the colour order
// can be judged by eye rather than asserted.
static void gfx_demo_scene(void)
{
    if (!s_fb.px) return;

    fb_clear(&s_fb, fb_rgb(16, 20, 32));

    fb_fill_rect(&s_fb, 0, 0, s_fb.w, 24, fb_rgb(40, 48, 72));
    fb_hline(&s_fb, 0, 24, s_fb.w, FB_CYAN);

    const uint16_t swatch[] = { FB_RED, FB_GREEN, FB_BLUE, FB_YELLOW,
                                FB_CYAN, FB_MAGENTA, FB_WHITE, FB_GREY };
    for (int i = 0; i < 8; ++i) {
        fb_fill_rect(&s_fb, 10 + i * 38, 40, 30, 40, swatch[i]);
        fb_rect(&s_fb, 10 + i * 38, 40, 30, 40, FB_WHITE);
    }

    // Deliberately off the edges: if clipping is wrong this corrupts memory
    // rather than drawing a partial box, so it is worth having in the scene.
    fb_fill_rect(&s_fb, -40, 100, 100, 50, fb_rgb(200, 80, 0));
    fb_fill_rect(&s_fb, s_fb.w - 60, 100, 100, 50, fb_rgb(0, 160, 120));
    fb_fill_rect(&s_fb, 120, s_fb.h - 20, 80, 100, fb_rgb(120, 0, 160));

    for (int i = 0; i < 16; ++i) {
        fb_vline(&s_fb, 20 + i * 6, 170, 10 + i * 3, FB_WHITE);
    }

    fb_rect(&s_fb, 0, 0, s_fb.w, s_fb.h, FB_RED);   // 1px border, both extremes

    fb_present(&s_fb);
    logln("  scene drawn: 8 swatches, 3 clipped rects, 16 bars, 1px border");
}

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

static void park_report(const char *what, uint32_t kms, uint32_t ems)
{
    const kpark_stats_t st = kpark_stats();
    logf("  %s\n", what);
    logf("  parks %lu, refused %lu, skipped %lu\n",
         (unsigned long)st.parks, (unsigned long)st.timeouts,
         (unsigned long)st.skipped);
    logf("  core 1 acknowledged within %lu us at worst\n",
         (unsigned long)st.ack_max_us);
    logf("  core 1 held parked %lu us at worst, %lu ms in total\n",
         (unsigned long)st.held_max_us,
         (unsigned long)(st.held_total_us / 1000));
    logf("  kernel clock %lu ms vs esp_timer %lu ms: %ld ms apart "
         "(%lu catch-ups, up to %lu ticks at once) %s\n",
         (unsigned long)kms, (unsigned long)ems, (long)kms - (long)ems,
         (unsigned long)k_tick_catchups(), (unsigned long)k_tick_max_step(),
         ((long)kms - (long)ems <= 2 && (long)ems - (long)kms <= 2) ? "OK" : "!! DRIFT");
}

// Reads need a park as much as writes do: the SPI driver turns the cache off
// for any operation, not only the ones that change flash. Reads cost the
// flash no wear, so this runs on every boot.
static void park_smoke_test(void)
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
    char what[80];
    snprintf(what, sizeof(what), "%d reads ok, %d failed, %d refused %s",
             ok, errs, refused, (ok == 100) ? "OK" : "!! FAIL");
    park_report(what, kms, ems);
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

static void park_flash_test(void)
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
    char what[120];
    snprintf(what, sizeof(what),
             "%d rounds verified, %d bad, %d refused; core 1 ran %lu passes, %lu wrong %s",
             ok, bad, refused, (unsigned long)(s_fw_iters - it0),
             (unsigned long)s_fw_bad,
             // At least 1000 passes, not merely "some": with the park disabled,
             // core 1 managed 41 passes in the whole run - stalled on a cache
             // that was switched off - and "made progress" would have passed.
             (ok == 40 && s_fw_bad == 0 && s_fw_iters - it0 >= 1000) ? "OK" : "!! FAIL");
    park_report(what, kms, ems);
}
#endif

// ---------------------------------------------------------------------------
// Stage 4.3b - core 0's main loop: the system around the apps.
//
// Core 0 owns the panel, so it does the two jobs that need SPI: drawing the
// status bar, and flushing the frames apps hand over. It also keeps serving
// core 1's mailbox and watching for a fault, as before.
// ---------------------------------------------------------------------------
static int s_status_sub = -1;

static void status_draw(void)
{
    // Stage 6.1: while the shell's "bootscreen" preview is up, the bar stays
    // off it - otherwise the clock repaints over the top of the picture a
    // second later, and a screenshot of the boot screen is not one.
    if (boot_preview_active()) return;

    const uint16_t bar = fb_rgb(40, 48, 72);
    fb_fill_rect(&s_fb, 0, 0, s_fb.w, APP_AREA_Y - 1, bar);
    fb_hline(&s_fb, 0, APP_AREA_Y - 1, s_fb.w, FB_CYAN);

    const int f = app_focused();
    const app_ctx_t *c = (f != APP_NONE) ? app_get(f) : nullptr;
    fb_text(&s_fb, 6, 2, c ? c->app->name : "-", FB_WHITE, 2);

    // Stage 5.4: how long it has been up, and how much room is left on the
    // filesystem - the two things worth a glance from any app.
    const uint32_t s = k_millis() / 1000;
    sysinfo_t si;
    sysinfo_get(&si);
    char right[64];
    snprintf(right, sizeof(right), "apps %d   %lu.%lu MB free   %lu:%02lu:%02lu", app_count(),
             (unsigned long)((si.fs_total - si.fs_used) / (1024 * 1024)),
             (unsigned long)((si.fs_total - si.fs_used) % (1024 * 1024) / (104858)),
             (unsigned long)(s / 3600), (unsigned long)(s / 60 % 60), (unsigned long)(s % 60));
    fb_text(&s_fb, s_fb.w - fb_text_width(right, 1) - 6, 8, right, FB_GREY, 1);
}

// One pass of the loop. The scripted test below runs these too, so that apps
// get their frames flushed while the script is waiting on them.
void system_pass(void)
{
    kmbox_service();
    app_poll();                     // stage 5.2: apps that ended or were killed
    k_free_dead_stacks();           // stage 5.3c: ended tasks' stacks, back to the heap
    if (kfault_pending()) kfault_report();

    bool redraw = false;
    ev_t ev;
    while (evbus_try_recv(s_status_sub, &ev)) redraw = true;   // focus or clock
    // Stage 6.4d: and not while the panel is dark. Stage 5.4e stopped the app
    // area going out to a screen nobody can see, but the status bar has its own
    // way to the panel and kept taking it - its clock ticks once a second
    // whatever the backlight is doing, so a board left on a desk was still
    // pushing the bar down the SPI bus all night. The bar is redrawn by the
    // next tick after the screen comes back.
    if (redraw && !screen_is_off()) { status_draw(); fb_flush(&s_fb); }

    while (app_compositor_poll()) { }
    settings_poll();                // a settled change reaches flash
    slog_poll();                    // log lines that have waited long enough
    sysinfo_poll();                 // stage 5.3b: RAM and storage figures, for core 1
    screen_poll();                  // stage 5.4: the backlight, after a quiet spell
}

static void system_loop(void)
{
    static ev_t status_q[16];
    s_status_sub = evbus_subscribe("status", EV_MASK(EV_SYS), status_q, 16);
    evbus_timer_start(s_status_sub, 1000, 1);   // the clock in the corner

#ifndef S3_KERNEL_TEST_BUILD
    // Stage 6.1: the boot screen has been up since the panel was lit. It is
    // held to its minimum HERE, before the status bar is drawn - the first
    // version cleared the screen and drew the bar first, so the mark was wiped
    // and the last second of boot was a spinner floating on an empty bar.
    boot_step("the launcher");
    boot_screen_hold();
    boot_screen_end();
#endif

    fb_clear(&s_fb, fb_rgb(16, 20, 32));
    status_draw();
    fb_present(&s_fb);

#ifndef S3_KERNEL_TEST_BUILD
    // At boot only the system starts: Home. Every app waits for the user to
    // open it. (Until stage 4.3d every registered app was started here.)
    for (int i = 0; i < app_registry_count(); ++i) {
        const app_t *a = app_registry_at(i);
        if (a->flags & APP_FLAG_SYSTEM) {
            logf("[apps] %s (system) -> %d\n", a->name, app_start(a));
        }
    }
    pump(300);
    // Stage 6.1: the boot screen was up, and was still whole when it ended.
    {
        char d[112];
        snprintf(d, sizeof(d), "up %lu ms, %lu steps, %lu frames, %lu px of the mark at the end, duty %lu",
                 (unsigned long)boot_screen_ms(), (unsigned long)boot_steps(),
                 (unsigned long)boot_frames(), (unsigned long)boot_mark_px(),
                 (unsigned long)lcd_backlight_duty());
        logln();
        logln("--- stage 6.1: the boot screen ---");
        app_case("boot screen", boot_screen_shown() && boot_screen_ms() >= BOOT_MIN_MS &&
                 boot_steps() >= 8 && boot_frames() > boot_steps() &&
                 boot_mark_px() > 200 && lcd_backlight_duty() > 0, d);
    }

    {
        char d[80];
        snprintf(d, sizeof(d), "%d apps running, %d registered, focus %s",
                 app_user_count(), app_registry_count() - 1,
                 app_focused() == app_home_id() ? "Home" : "NOT Home");
        logln();
        logln("--- stage 4.3d: nothing runs until the user opens it ---");
        app_case("boot starts no app", app_user_count() == 0 &&
                 app_focused() == app_home_id() && app_home_id() != APP_NONE, d);
    }

    // Stage 5.3c: the kernel's two pools, taken from ESP-IDF at boot, and no
    // ended task still sitting on its stack. Before 5.3c the heap was 64 KB and
    // thirteen finished self-test tasks held 29,952 bytes of it for good.
    {
        pump(50);
        kheap_stats_t in, ps;
        kheap_stats_pool(KHEAP_INTERNAL, &in);
        kheap_stats_pool(KHEAP_PSRAM, &ps);
        int held = 0;
        for (int i = 0; i < K_MAX_TASKS; ++i) {
            const k_tcb_t *t = k_task_by_id((uint8_t)i);
            if (t && t->state == K_DEAD && t->stack) held++;
        }
        char d[120];
        snprintf(d, sizeof(d), "internal %u of %u B used (asked %u KB), psram %u KB; %d ended tasks hold stacks",
                 (unsigned)in.used, (unsigned)in.total, (unsigned)(KHEAP_INTERNAL_SIZE / 1024),
                 (unsigned)(ps.total / 1024), held);
        // Exact: every byte of each region is a block header (16 B) or a
        // payload. The internal pool's size is what it was given, which since
        // stage 6.3 may be less than it asked for - the radio's memory comes
        // out of the same heap - so the claim is that the arithmetic adds up
        // and that what it got is enough to work in, not that it is 128 KB.
        const size_t in_region = in.total + in.blocks * 16;
        app_case("kernel memory", in_region >= KHEAP_INTERNAL_MIN &&
                 in_region <= KHEAP_INTERNAL_SIZE &&
                 ps.total + ps.blocks * 16 == KHEAP_PSRAM_SIZE && held == 0 && in.used < 20000, d);
    }

    // The tests below open apps, which only the user should do - so they are
    // a test build now, not part of every boot.
#ifdef S3_TEST_APPS
    launcher_test();
    {
        const int in = app_start(&app_input);
        const int ct = app_start(&app_counter);
        const int bn = app_start(&app_bounce);
        const int bn2 = app_model_test(in, ct, bn);
#ifdef S3_TEST_APPHOG
        app_hog_test(bn2);
#else
        (void)bn2;
#endif
    }
    sdk_live_test();
    fs_live_test();
    sys45_live_test();
    close_user_apps();
    sys52_kill_test();
    sys53_tasks_test();
    sys55_minimise_test();
    sys54_polish_test();
    sys54d_limits_test();
    sys54e_quiet_test();
    sys62_wallpaper_test();
    sys63_radio_test();
    sys64_keyboard_test();
#ifdef S3_TEST_FSFULL
    sys54d_fsfull_test();
#endif
#endif
#endif

    uint32_t next_alive = k_millis() + 15000;
    shell_set_screen(&s_fb);
    shell_init();
    for (;;) {
        system_pass();
        shell_poll();               // stage 5.1: only now, after the boot tests

        // A heartbeat, because a quiet serial log cannot tell "nobody pressed
        // anything" from "something hung". It runs off the clock directly, not
        // off a bus timer: a heartbeat must not depend on the thing it watches.
        const uint32_t now = k_millis();
        if ((int32_t)(now - next_alive) >= 0) {
            logf("[alive] kernel %lu ms, sampler passes %lu, frames flushed %lu, "
                 "focus %d, %d apps open\n",
                 (unsigned long)now, (unsigned long)input_samples(),
                 (unsigned long)app_frames_flushed(), app_focused(), app_user_count());
            next_alive = now + 15000;
        }

        hal_delay_ms(2);
    }
}

// Counts while the display is being written to.
//
// Wall-clock frame time alone cannot tell polling from queued - both push the
// same bytes at the same clock. What changes is whether the core is available
// during those milliseconds, and the only way to see that is to give it
// something else to do and count how much of it got done.
//
// It sits below the benchmark in priority, so it advances only when the
// benchmark is genuinely blocked rather than merely sharing.
static volatile DRAM_ATTR uint32_t s_cpu_probe;
static volatile DRAM_ATTR bool     s_cpu_probe_run = true;

static void cpu_probe_task(void *arg)
{
    (void)arg;
    while (s_cpu_probe_run) s_cpu_probe = s_cpu_probe + 1;
    vTaskDelete(NULL);
}

static void benchmark_mode(const char *label, lcd_xfer_mode_t mode)
{
    static const uint16_t colors[] = {
        COLOR_RED, COLOR_GREEN, COLOR_BLUE, COLOR_WHITE,
        COLOR_YELLOW, COLOR_CYAN, COLOR_MAGENTA, COLOR_BLACK
    };
    const int frames = sizeof(colors) / sizeof(colors[0]);

    lcd_bus_set_mode(mode);

    uint32_t spin0 = s_cpu_probe;
    int64_t  t0    = hal_micros();
    for (int i = 0; i < frames; ++i) lcd_fill_screen(colors[i]);
    int64_t  dt    = hal_micros() - t0;
    uint32_t spins = s_cpu_probe - spin0;

    double per_frame_ms = (dt / 1000.0) / frames;
    size_t frame_bytes  = (size_t)lcd_width() * lcd_height() * lcd_bytes_per_pixel();
    double mbps         = (frame_bytes / (per_frame_ms / 1000.0)) / 1e6;

    logf("  %-8s %6.2f ms/frame  %5.1f FPS  %4.2f MB/s   other work done: %lu\n",
         label, per_frame_ms, 1000.0 / per_frame_ms, mbps,
         (unsigned long)spins);
}

static void benchmark(void)
{
    logln();
    logln("--- full-screen fill: polling vs queued ---");
    logf("frame is %ux%u at %u bytes/pixel = %u bytes on the wire\n",
         lcd_width(), lcd_height(), lcd_bytes_per_pixel(),
         (unsigned)((size_t)lcd_width() * lcd_height() * lcd_bytes_per_pixel()));

    // A task to soak up whatever core 0 is not using, and a priority above it
    // so it only runs when we are actually blocked.
    s_cpu_probe_run = true;
    xTaskCreatePinnedToCore(cpu_probe_task, "cpuprobe", 2048, nullptr, 1, nullptr, 0);
    UBaseType_t was = uxTaskPriorityGet(NULL);
    vTaskPrioritySet(NULL, 3);

    benchmark_mode("polling", LCD_XFER_POLLING);
    benchmark_mode("queued",  LCD_XFER_QUEUED);

    vTaskPrioritySet(NULL, was);
    s_cpu_probe_run = false;
    hal_delay_ms(5);            // let the probe task retire

    logln("'other work done' is the point: the bytes go out at the same rate");
    logln("either way, but polling spends the whole transfer watching a FIFO.");
    logln("Leaving the bus in queued mode from here.");
    lcd_bus_set_mode(LCD_XFER_QUEUED);
}

static void draw_test_pattern(void)
{
    lcd_fill_screen(COLOR_BLACK);

    const uint16_t bars[] = {
        COLOR_RED, COLOR_GREEN, COLOR_BLUE, COLOR_WHITE,
        COLOR_YELLOW, COLOR_CYAN, COLOR_MAGENTA
    };
    const int n = sizeof(bars) / sizeof(bars[0]);
    uint16_t bw = lcd_width() / n;
    for (int i = 0; i < n; ++i) {
        lcd_fill_rect(i * bw, 0, bw, 60, bars[i]);
    }

    // Corner markers - if these land in the right corners, rotation is right.
    lcd_fill_rect(0, 0, 16, 16, COLOR_WHITE);
    lcd_fill_rect(lcd_width() - 16, lcd_height() - 16, 16, 16, COLOR_WHITE);
}

// Lights up a box for each button so the wiring can be checked on the panel
// itself, without watching the serial log.
static void draw_button_state(uint32_t held)
{
    struct { uint32_t bit; uint16_t x, y; } slots[] = {
        { BTN_UP,     140, 140 }, { BTN_DOWN,  140, 220 },
        { BTN_LEFT,    80, 180 }, { BTN_RIGHT, 200, 180 },
        { BTN_OK,     320, 180 }, { BTN_BACK,  390, 180 },
    };
    for (auto &s : slots) {
        lcd_fill_rect(s.x, s.y, 56, 56, (held & s.bit) ? COLOR_GREEN : RGB565(40, 40, 40));
    }
}

extern "C" void app_main(void)
{
    log_init();
    hal_delay_ms(1000);                // let the USB console attach

    report_memory();
    alloc_framebuffer();

    logln();
    lcd_ctrl_t ctrl = lcd_probe_run();
#ifdef LCD_FORCE_CTRL
    ctrl = LCD_FORCE_CTRL;   // set in platformio.ini when the panel cannot be read
#endif
    logf("[lcd] controller: %s\n", lcd_ctrl_name(ctrl));

    esp_err_t err = lcd_bus_init(LCD_SPI_HZ);
    if (err != ESP_OK) {
        logf("[lcd] SPI bus init failed: %d\n", (int)err);
        return;
    }
    if (!lcd_panel_init(ctrl)) return;

    lcd_set_rotation(1);               // landscape 480x320
    // Stage 6.1: the backlight stays down until the boot screen. The
    // benchmark and the graphics self-tests below still draw and still flush
    // over the same SPI at the same cost - there is just nobody watching.
    lcd_backlight_pct(0);
    logf("[lcd] up: %ux%u, %u bytes/pixel\n",
         lcd_width(), lcd_height(), lcd_bytes_per_pixel());

    benchmark();
    gfx_bringup();
#ifdef S3_GFX_SELFTEST
    // Four differently-coloured corners and one wide bar: the smallest picture
    // that separates inversion, colour order and rotation from each other.
    // Keep it - the next panel will need it too.
    gfx_selftest();
#else
    gfx_demo_scene();
    gfx_damage_demo();
    gfx_chunk_demo();
    gfx_text_demo();
#endif

    // Stage 6.1: from here the panel shows one screen, and each step below
    // moves its spinner on.
    boot_screen_begin(&s_fb);

    // Pins and the event queue exist before core 1 does - its first task is
    // the sampler, and it must not find either half-built. The scripted test
    // runs here too, while nothing else is feeding the queue.
    boot_step("the event bus");
    evbus_init();
    app_system_init(&s_fb);         // the manager's queue exists before core 1
    boot_step("the device service");
    dev_init();                     // hardware service for apps, on this core
    {
        // Mounted before core 1 exists, so the first mount - which formats an
        // empty partition - cannot catch anything of core 1's mid-flight.
        boot_step("the filesystem");
        const int64_t t0 = esp_timer_get_time();
        const bool ok = fs_init();
        size_t total = 0, used = 0;
        fs_info(&total, &used);
        logf("[fs] LittleFS on 'storage': %s in %lu ms, %lu KB total, %lu KB used\n",
             ok ? "mounted" : "NOT mounted",
             (unsigned long)((esp_timer_get_time() - t0) / 1000),
             (unsigned long)(total / 1024), (unsigned long)(used / 1024));
    }

    // Stage 4.5: the system log and settings, as early as the filesystem
    // allows, so the first thing each boot records is itself - its number and
    // why the previous one ended. A crash overnight is then a line on flash,
    // not something that happened to a console nobody was watching.
    {
        boot_step("the system log and settings");
        const uint32_t boot = fs_mounted() ? fs_boot_count() : 0;
        slog_init(boot);
        slog("sys", "boot #%lu, last reset: %s, firmware %s %s", (unsigned long)boot,
             reset_reason_name(), __DATE__, __TIME__);
        kfault_set_sink([](unsigned cause, const char *name, unsigned pc, unsigned addr,
                           bool dbl) {
            slog("fault", "core 1 fault %u (%s) at pc 0x%08x, address 0x%08x%s", cause,
                 name, pc, addr, dbl ? ", DOUBLE fault" : "");
        });
        settings_define("backlight", 100, 5, 100,
                        [](int32_t v) { lcd_backlight_pct((uint8_t)v); });
        settings_define("screenoff", 60, 0, 600, nullptr);   // seconds; 0 = never (5.4)
        settings_define("wallpaper", 1, 0, 1, nullptr);      // Home's background (6.2)
        settings_load();
        screen_init();                  // stage 5.4: the idle timer starts now
    }
    boot_step("the buttons");
    input_init();
    boot_step("checking itself");
    input_logic_test();
    evbus_logic_test();
    sdk_logic_test();
    ui_logic_test();

    // The kernel starts only now. Measuring the display first keeps core 0 to
    // ourselves for the benchmark - once k_smp_join runs, core 0 is shared with
    // whatever the scheduler puts there and the numbers stop meaning anything.
#if defined(S3_TEST_PARK) || defined(S3_TEST_PARK_OFF)
    s_park_psram = (uint32_t *)heap_caps_malloc(4096, MALLOC_CAP_SPIRAM);
#endif

    boot_step("the kernel on core 1");
    wake_core1();

#if defined(S3_TEST_PARK) || defined(S3_TEST_PARK_OFF)
    park_flash_test();
#endif
    park_smoke_test();

    // Stage 4.4, every boot: bump the boot counter kept on flash. Core 1 is
    // running now, so this write goes through the global park hook - no
    // explicit kpark call anywhere near it - and the stats must show it parked.
    {
        logln();
        logln("--- stage 4.4 / 4.5: files, the system log, settings ---");
        // Core 1 is running now, so this log line - a flash write - goes
        // through the global park hook with no explicit kpark call near it,
        // and the stats must show it parked. (Until 4.5 the boot counter was
        // written here for the same purpose; it moved to the start of boot.)
        const kpark_stats_t p0 = kpark_stats();
        slog("sys", "kernel up on core 1");
        const kpark_stats_t p1 = kpark_stats();
        char d[128];
        snprintf(d, sizeof(d), "boot #%lu; %lu flash ops, %lu parks, %lu unparked",
                 (unsigned long)fs_boot_number(), (unsigned long)(p1.flash_ops - p0.flash_ops),
                 (unsigned long)(p1.parks - p0.parks),
                 (unsigned long)(p1.unparked - p0.unparked));
        sdk_case("log write, parked", fs_boot_number() >= 1 && p1.flash_ops > p0.flash_ops &&
                 p1.parks > p0.parks && p1.unparked == p0.unparked, d);

        // This boot's first line must be in the file, read back off flash.
        char want[24];
        snprintf(want, sizeof(want), "boot #%lu,", (unsigned long)fs_boot_number());
        const bool found = log_has(want);
        snprintf(d, sizeof(d), "\"%s\" in the log: %s; log %lu bytes", want,
                 found ? "yes" : "NO", (unsigned long)slog_size());
        sdk_case("boot recorded", found, d);

        // The brightness setting, loaded from flash, is what the PWM is doing.
        const int32_t bl = settings_get("backlight");
        const uint32_t duty = lcd_backlight_duty();
        snprintf(d, sizeof(d), "backlight %ld %%, PWM duty %lu of 1023 (want %lu)", (long)bl,
                 (unsigned long)duty, (unsigned long)(bl * 1023 / 100));
        sdk_case("settings applied", duty == (uint32_t)(bl * 1023 / 100), d);

        logln("  files kept by apps:");
        fs_print_tree();
    }

#ifdef S3_TEST_PANIC_REBOOT
    // Crash core 0 on purpose while core 1 is running, and check the boot
    // after it. A marker file makes it crash exactly once: written just before
    // the crash, found - and removed - by the boot that follows. Without the
    // restart wrap in kcore1.cpp, that boot met a core 1 still running the old
    // kernel over the new one's memory.
    {
        logln();
        logln("--- stage 4.4: a crash while core 1 runs, then the next boot ---");
        struct stat st;
        if (stat("/fs/sys/panic_test", &st) != 0) {
            FILE *f = fopen("/fs/sys/panic_test", "w");
            if (f) { fputs("crash once\n", f); fclose(f); }
            logln("  core 1 is running; crashing core 0 on purpose now");
            hal_delay_ms(50);
            abort();
        }
        unlink("/fs/sys/panic_test");
        const uint32_t t0 = s3_tick_count;
        hal_delay_ms(100);
        const uint32_t ticks = s3_tick_count - t0;
        char d[96];
        snprintf(d, sizeof(d), "reset: %s; core 1 faults %u; %lu ticks in 100 ms",
                 esp_reset_reason() == ESP_RST_PANIC ? "panic" : "other",
                 kfault_reported(), (unsigned long)ticks);
        sdk_case("clean boot after a crash", esp_reset_reason() == ESP_RST_PANIC &&
                 kfault_reported() == 0 && ticks >= 95 && ticks <= 105, d);
    }
#endif

    system_loop();
}
