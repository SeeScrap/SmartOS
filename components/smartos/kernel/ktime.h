#pragma once
#include <stdbool.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// Stage 2.4 - the kernel's own clock.
//
// The tick comes from CCOMPARE0, the timer built into the Xtensa core itself.
// That choice matters: CCOMPARE0 and INTENABLE are both per-core registers, so
// core 1 gets a timer interrupt that core 0 cannot see, cannot disable and does
// not have to be asked for. No interrupt matrix routing, no shared peripheral,
// no arbitration with IDF. The handler is _s3_int_entry in kvectors.S.
//
// PREEMPTION
//
// With k_preempt_enable() the tick does not merely ask for a reschedule, it
// performs one: k_int_dispatch calls k_yield() from inside the interrupt, on
// the interrupted task's own stack, and the task does not run again until the
// scheduler comes back to it. A task that never yields no longer owns the core.
//
// It is off until enabled so that the tick can be brought up and checked on its
// own - a handler that both counts wrong and switches wrong is no fun at all.
// ---------------------------------------------------------------------------

// Written by the ISR, read by everyone. Defined in kvectors.S.
extern "C" volatile uint32_t k_tick_count;
extern "C" volatile uint32_t k_need_resched;
extern "C" volatile uint32_t k_tick_cycles;

// Arms CCOMPARE0 and enables the timer interrupt on the calling core.
// hz is the tick rate; 1000 gives a 1 ms tick.
void     k_tick_start(uint32_t hz);
bool     k_tick_running(void);

uint32_t k_ticks(void);      // ticks since k_tick_start
// Interrupts that had to count more than one tick - core 1 was parked, or
// otherwise could not take the tick on time - and the most counted at once.
// The ticks themselves are never lost; these say how often it came up.
uint32_t k_tick_catchups(void);
uint32_t k_tick_max_step(void);
uint32_t k_millis(void);     // ticks converted to milliseconds
uint32_t k_tick_hz(void);

// True if the tick has asked for a scheduling decision since the last one.
bool     k_resched_pending(void);
void     k_resched_clear(void);

// Preemption. Off after k_tick_start; turn it on once tasks exist.
void     k_preempt_enable(void);
void     k_preempt_disable(void);
bool     k_preempt_enabled(void);
uint32_t k_preempt_count(void);   // ticks that reached the scheduler

// Holds off the scheduler without masking the tick, so time keeps being counted
// while a short critical region runs to completion. Nestable. This is what a
// print uses: masking interrupts across a whole console line would drop ticks,
// but letting a task be switched out mid-line would interleave two of them.
void     k_preempt_hold(void);
void     k_preempt_release(void);

// What the tick interrupted, recorded by the ISR for core 0 to print later.
typedef struct { uint32_t tick, pc, sp, task, hold, state; } k_trace_t;
extern "C" uint32_t k_trace_count(void);
extern "C" const k_trace_t *k_trace_at(uint32_t i);

// Blocks the calling task for at least ms milliseconds. Other tasks run in the
// meantime; if none of them are runnable the core idles in waiti until the tick
// wakes something.
void     k_sleep_ms(uint32_t ms);
