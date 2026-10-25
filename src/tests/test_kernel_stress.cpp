// The stress and regression builds: the three arrangements that each exist to
// reproduce one specific failure, and only compile when asked for.
//
//   -D SMARTOS_TEST_SLEEP_CLAIM   the scheduler race found while verifying -O2:
//                            two cores claiming the same sleeping task
//   -D SMARTOS_TEST_MBOX_STRESS   four posters hammering the cross-core mailbox
//   -D SMARTOS_TEST_EVBUS         the event bus under load, with a stalled
//                            subscriber and a timer watching for lateness
//
// They were in main.cpp until R2b of the SmartOS restructuring; nothing about
// them changed in the move. IRAM_ATTR throughout, like everything core 1 runs.

#include "tests/tests.h"

#include "esp_attr.h"
#include "esp_cpu.h"

#include "kernel/kheap.h"
#include "kernel/klog.h"
#include "kernel/kmbox.h"
#include "kernel/kspin.h"
#include "kernel/ksync.h"
#include "kernel/internal/ktask_internal.h"
#include "kernel/ktask.h"
#include "kernel/ktime.h"
#include "os/evbus.h"

#ifdef SMARTOS_TEST_SLEEP_CLAIM
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

#ifdef SMARTOS_TEST_MBOX_STRESS
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

#ifdef SMARTOS_TEST_EVBUS
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
        const uint32_t park_us = (uint32_t)((ct.park - park_prev) / K_CPU_MHZ);
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

// --- how core1_main starts each arrangement -------------------------------
// cpu0_idle_task stays in main.cpp and is created after these, exactly as
// before: task ids follow creation order.

#ifdef SMARTOS_TEST_SLEEP_CLAIM
// One sleeper free to run anywhere, alone on core 1, and core 0 inside the
// scheduler beside it - the exact arrangement the race needs.
void IRAM_ATTR kernel_claim_start(void)
{
    k_task_create("claim", claim_task, nullptr);
}
#endif

#ifdef SMARTOS_TEST_MBOX_STRESS
// Two posters pinned to each core. Left free, they all end up posting from
// core 1: core 0 only serves the mailbox while it is OUT of the kernel, so
// core 1 is always the one to see an answer land and post the next job.
void IRAM_ATTR kernel_mbox_stress_start(void)
{
    for (uint32_t i = 0; i < STRESS_POSTERS; ++i) {
        k_task_create_on("stress", mbox_stress_task, (void *)(uintptr_t)i,
                         (uint8_t)(i < STRESS_POSTERS / 2 ? 1 : 0));
    }
}
#endif

#ifdef SMARTOS_TEST_EVBUS
void IRAM_ATTR kernel_evbus_stress_start(void)
{
    for (int i = 0; i < 16; ++i) k_task_create("filler", bus_filler_task, nullptr);
    k_task_create_pri("evtimer", evbus_timer_task, nullptr, 1, K_PRIO_HIGH);
    const uint32_t bus_mask = EV_MASK(EV_APP) | EV_MASK(EV_SYS);
    s_bus_s1    = evbus_subscribe("s1",    bus_mask, EV_Q(s_bus_q1));
    s_bus_s2    = evbus_subscribe("s2",    bus_mask, EV_Q(s_bus_q2));
    s_bus_stall = evbus_subscribe("stall", bus_mask, EV_Q(s_bus_stall_q));
    k_task_create("bus_s1", bus_sub_task, (void *)1);
    k_task_create("bus_s2", bus_sub_task, (void *)2);
    k_task_create_on("bus_p0", bus_pub_task, (void *)0, 0);
    k_task_create_on("bus_p1", bus_pub_task, (void *)1, 1);
    k_task_create_on("bus_tmr", bus_timer_check_task, nullptr, 1);
}
#endif
