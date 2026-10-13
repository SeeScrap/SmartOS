#include "kernel/ktime.h"

#include "esp_attr.h"
#include "kernel/klog.h"
#include "kernel/kpark.h"
#include "kernel/ktask.h"
#include "sdkconfig.h"

// Everything here runs on core 1, so it all lives in IRAM for the same reason
// the scheduler does - see the note at the top of ksched.cpp.

static DRAM_ATTR uint32_t s_tick_hz   = 0;
static DRAM_ATTR bool     s_preempt   = false;
// s_preempt stays global on purpose: whether preemption is enabled at all is a
// system-wide policy. The hold depth and the counter are per core - see the
// note in k_percpu_t for what sharing them cost.
static DRAM_ATTR bool     s_running  = false;

// CCOUNT at the boundary of the last tick counted. Ticks are counted as whole
// periods elapsed since this, not as one per interrupt - see k_int_dispatch.
static DRAM_ATTR uint32_t s_tick_base = 0;
static DRAM_ATTR uint32_t s_tick_catchups = 0;   // interrupts that counted >1 tick
static DRAM_ATTR uint32_t s_tick_max_step = 0;   // most ticks counted at once

// How far ahead CCOMPARE0 must be when it is written. Closer than this, and
// CCOUNT can pass it before the write lands - after which the comparator does
// not match again until CCOUNT wraps, 17.9 seconds later.
#define K_TICK_MARGIN_CYCLES 2000

IRAM_ATTR void k_tick_start(uint32_t hz)
{
    if (hz == 0) hz = 1000;
    s_tick_hz = hz;

    // CCOUNT advances at the CPU clock, so a tick is simply cpu_hz / tick_hz.
    uint32_t cpu_hz = (uint32_t)CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ * 1000000u;
    s3_tick_cycles  = cpu_hz / hz;

    uint32_t ps = k_lock();

    // Arm the first tick.
    uint32_t ccount;
    __asm__ __volatile__("rsr %0, ccount" : "=a"(ccount));
    s_tick_base = ccount;
    uint32_t next = ccount + s3_tick_cycles;
    __asm__ __volatile__("wsr %0, ccompare0" :: "a"(next));

    // Enable CPU interrupt 6 on this core only. INTENABLE is a per-core
    // register, so this is invisible to core 0.
    uint32_t ie;
    __asm__ __volatile__("rsr %0, intenable" : "=a"(ie));
    ie |= (1u << 6);
    __asm__ __volatile__("wsr %0, intenable; rsync" :: "a"(ie));

    s_running = true;
    k_unlock(ps);

    klog("[kernel] tick armed: %u Hz, %u cycles, CPU int 6 (CCOMPARE0)\n",
         (unsigned)hz, (unsigned)s3_tick_cycles);
}

IRAM_ATTR bool k_tick_running(void) { return s_running; }

IRAM_ATTR void k_preempt_enable(void)  { s_preempt = true; }
IRAM_ATTR void k_preempt_disable(void) { s_preempt = false; }
IRAM_ATTR bool k_preempt_enabled(void) { return s_preempt; }
IRAM_ATTR uint32_t k_preempt_count(void) { return k_cpu()->preempts; }

IRAM_ATTR void k_preempt_hold(void)    { k_cpu()->hold++; }
IRAM_ATTR void k_preempt_release(void) { k_percpu_t *c = k_cpu(); if (c->hold) c->hold--; }

// ---------------------------------------------------------------------------
// The C half of the tick interrupt, called by _s3_int_entry once it has left
// exception mode. Running here rather than in assembly is what makes real
// preemption possible: k_yield() below switches stacks from inside the
// interrupt, and this function simply does not return until the scheduler
// brings this task back.
// ---------------------------------------------------------------------------
// Word offsets into the frame _s3_int_entry builds. Mirrors the IF_* defines
// in kvectors.S - change both together.
#define IFW_A0   0
#define IFW_A1   1
#define IFW_PS   16
#define IFW_PC   17

// A ring of what the tick actually interrupted, filled in by the ISR and read
// out by core 0. Printing from in here under preemption is a good way to turn
// one bug into two, so the evidence is collected now and reported later.
// k_trace_t itself is declared in ktime.h; the ring lives here.
#define K_TRACE_N 24
static DRAM_ATTR k_trace_t s_trace[K_TRACE_N];
static DRAM_ATTR uint32_t  s_trace_used;

extern "C" IRAM_ATTR uint32_t k_trace_count(void) { return s_trace_used; }
extern "C" IRAM_ATTR const k_trace_t *k_trace_at(uint32_t i)
{
    return (i < K_TRACE_N && i < s_trace_used) ? &s_trace[i] : nullptr;
}

extern "C" IRAM_ATTR void k_int_dispatch(uint32_t *frame)
{
    // Every level-1 interrupt on this core arrives here. Until stage 4.3a
    // there was only the tick, and this function assumed it; now the park
    // request shares the entry, so each source is checked for itself.
    uint32_t pending, enabled;
    __asm__ __volatile__("rsr %0, interrupt" : "=a"(pending));
    __asm__ __volatile__("rsr %0, intenable" : "=a"(enabled));
    pending &= enabled;

    // Park first. If core 0 wants flash, it is waiting on us right now, and
    // any tick that fires meanwhile is still pending when we come back out -
    // counted below, not lost.
    if (pending & (1u << KPARK_CPU_INT)) kpark_isr();

    if (!(pending & (1u << 6))) {
        // A park on its own. Re-read: the tick may well have come due while
        // we were parked, and it would otherwise wait for the next interrupt.
        __asm__ __volatile__("rsr %0, interrupt" : "=a"(pending));
        if (!(pending & (1u << 6))) return;
    }

    // Count ticks as whole periods of CCOUNT since the last counted boundary,
    // not one per interrupt. One per interrupt loses every tick that fell
    // while this core could not take the interrupt - a park lasts as long as
    // a flash erase, tens of milliseconds - and it also let a little latency
    // creep in on every re-arm, so the kernel clock has always run slightly
    // slow. Counting from CCOUNT makes both impossible: however late we are,
    // the ticks that elapsed are the ticks that get counted.
    uint32_t ccount;
    __asm__ __volatile__("rsr %0, ccount" : "=a"(ccount));
    uint32_t n = (ccount - s_tick_base) / s3_tick_cycles;
    if (n == 0) n = 1;                     // cannot happen; never count zero
    s_tick_base += n * s3_tick_cycles;

    // Re-arming CCOMPARE0 is also how this interrupt is acknowledged. The
    // next boundary may be only a few cycles away if we were late by nearly a
    // whole period; count it now rather than risk writing a compare value that
    // CCOUNT has already passed.
    uint32_t next = s_tick_base + s3_tick_cycles;
    if ((int32_t)(next - ccount) < K_TICK_MARGIN_CYCLES) {
        n++;
        s_tick_base += s3_tick_cycles;
        next        += s3_tick_cycles;
    }
    __asm__ __volatile__("wsr %0, ccompare0" :: "a"(next));

    if (n > 1) {
        s_tick_catchups = s_tick_catchups + 1;
        if (n > s_tick_max_step) s_tick_max_step = n;
    }

    s3_tick_count   = s3_tick_count + n;   // read-modify-write, not +=,
    s3_need_resched = 1;                   // which C++20 deprecates on volatile

    // Record every tick, not just the ones that switch: the interesting one is
    // usually the tick right before things go wrong.
    if (s_trace_used < K_TRACE_N) {
        k_tcb_t *cur = k_current();
        k_trace_t *e = &s_trace[s_trace_used++];
        e->tick  = s3_tick_count;
        e->pc    = frame[IFW_PC];
        e->sp    = frame[IFW_A1];
        e->task  = (uint32_t)(uintptr_t)cur;
        e->hold  = k_cpu()->hold;
        e->state = cur ? (uint32_t)cur->state : 0xFF;
    }

    // Stage 5.3: charge whoever has been running. A task alone on this core is
    // never switched out, so without this it would never be charged at all.
    k_account_tick();

    // Never reschedule on behalf of a task that has already exited: its stack
    // may already have been handed back to the allocator.
    k_tcb_t *running = k_current();
    bool schedulable = running && running->state != K_DEAD && running->state != K_ZOMBIE;

    k_percpu_t *self = k_cpu();
    if (s_preempt && self->hold == 0 && schedulable) {
        self->preempts++;
        k_yield();      // may not return for a very long time
    }
}

IRAM_ATTR uint32_t k_ticks(void) { return s3_tick_count; }
IRAM_ATTR uint32_t k_tick_catchups(void) { return s_tick_catchups; }
IRAM_ATTR uint32_t k_tick_max_step(void) { return s_tick_max_step; }

IRAM_ATTR uint32_t k_tick_hz(void) { return s_tick_hz; }

IRAM_ATTR uint32_t k_millis(void)
{
    if (s_tick_hz == 0) return 0;
    return (uint32_t)((uint64_t)s3_tick_count * 1000u / s_tick_hz);
}

IRAM_ATTR bool k_resched_pending(void) { return s3_need_resched != 0; }

IRAM_ATTR void k_resched_clear(void) { s3_need_resched = 0; }

IRAM_ATTR void k_sleep_ms(uint32_t ms)
{
    if (!s_running) {
        // No clock yet: the best we can honestly do is give others a turn.
        k_yield();
        return;
    }

    uint32_t ticks = (uint32_t)((uint64_t)ms * s_tick_hz / 1000u);
    if (ticks == 0) ticks = 1;

    k_tcb_t *t = k_current();
    if (!t) return;

    uint32_t ps = k_lock();
    t->wake_tick = s3_tick_count + ticks;
    t->state     = K_SLEEPING;
    k_unlock(ps);

    // k_yield picks someone else and, on the way, wakes any sleeper whose
    // deadline has passed - including this one, eventually.
    while (t->state == K_SLEEPING) {
        k_yield();
    }
}
