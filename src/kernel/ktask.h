#pragma once
#include <stddef.h>
#include <stdint.h>

#include "kernel/kspin.h"

// ---------------------------------------------------------------------------
// Stage 2.3 - the kernel core.
//
// Task control blocks, per-CPU run state, a round-robin scheduler and a real
// Xtensa context switch. Cooperative only: a task runs until it calls k_yield()
// or k_exit(). Stage 2.4 adds the timer interrupt that takes the CPU away
// without being asked.
//
// SMP-READY ON PURPOSE
//
// Every piece of run state lives in k_percpu_t, indexed by core, even though
// K_MAX_CPUS is 1 today. Retrofitting that later would mean rewriting the
// scheduler; carrying it now costs about twenty lines. Same reason k_lock() is
// a function and not an open-coded rsil - stage 2.6b turns it into a spinlock
// without touching a single caller.
// ---------------------------------------------------------------------------

// 32 is the ceiling, not a guess: the wait sets in ksync.h are one uint32_t
// per object, one bit per task (a static_assert there holds the two together).
// It was 16 until stage 4.2, when the stage 2 demo plus the input sampler had
// filled every slot and the event bus's timer service had nowhere to go.
#define K_MAX_TASKS   32
#define K_MAX_CPUS    2
// 2 KB per task. Measured high-water across the whole demo was 704 bytes, so
// this is still 3x headroom, and it keeps twelve tasks inside a 48 KB heap.
#define K_STACK_SIZE  2048

// Stage 5.4d - a task may ask for another size. Apps declare theirs in
// app_t.stack: a screen that keeps a line buffer on the stack needs more than
// 2 KB, and an app that does almost nothing can have less and leave the pool
// for something else. The floor is what a context switch alone needs below the
// stack pointer (see K_STACK_GUARD) with room to do anything at all; the
// ceiling only keeps a typo from swallowing the pool.
#define K_STACK_MIN   1024
#define K_STACK_MAX   16384

// Dead space allocated below every stack.
//
// A canary at the bottom of a stack only detects an overrun at the next switch
// out, by which time the overrunning task has already written over whatever was
// underneath - which, since stacks come from one heap, is another task's stack.
// The first deliberate overrun test proved that the hard way: an interrupt
// watchdog timeout on core 0 and a boot loop, with the guard never getting a
// chance to speak. Giving each stack a run-off area means a modest overrun
// lands in memory nobody else owns, and the canary lives to report it.
//
// 256 is a floor, not a preference. A task that has overrun still has to be
// switched out, and switching writes roughly 200 bytes BELOW the stack pointer:
// a base save area per live window, k_yield's own frames, and the 112-byte tick
// interrupt frame if the tick happens to land there. A run-off area smaller
// than that just moves the corruption into the next heap block's header.
#define K_STACK_GUARD 256

typedef void (*k_task_fn)(void *arg);

// Which core a task may run on. Most tasks say ANY and let the scheduler
// decide; the ones that must stay put are the per-core idles.
#define K_CPU_ANY  0xFF

// Priority. Lower number wins. Round-robin still applies, but only among tasks
// of equal priority - which is what makes it useful: a display task can be told
// to always beat a background one without having to hand-tune yields.
#define K_PRIO_HIGH    0
#define K_PRIO_NORMAL  8
#define K_PRIO_LOW     16
#define K_PRIO_IDLE    31
#define K_PRIO_LEVELS  32

typedef enum {
    K_UNUSED = 0,
    K_READY,
    K_RUNNING,
    K_SLEEPING,      // waiting for a tick deadline; see ktime.h
    K_BLOCKED,       // waiting on a mutex, semaphore or queue; see ksync.h
    K_DEAD,
    K_ZOMBIE,        // killed (stage 5.2): never runs again, but its stack is
                     // not reusable until k_task_reap - see k_task_kill
} k_state_t;

typedef struct k_tcb {
    uint32_t   *sp;          // saved stack pointer while not running
    uint8_t    *stack;       // base of this task's stack
    size_t      stack_size;
    k_task_fn   entry;
    void       *arg;
    k_state_t   state;
    uint32_t    switches;    // times this task has been switched in
    uint32_t    wake_tick;   // when a K_SLEEPING task becomes runnable
    const char *name;
    uint8_t     id;
    uint8_t     cpu;         // core it last ran on
    uint8_t     affinity;    // K_CPU_ANY, or a core number
    uint8_t     prio;        // lower runs first; see K_PRIO_*

    // Stage 5.2 - so a task can be killed from outside.
    uint32_t    gen;         // bumped at every create: (id, gen) names ONE task,
                             // even after its slot is reused
    volatile uint32_t *blocked_on;   // the wait set a K_BLOCKED task is in
    volatile uint8_t   kill;         // set while it runs; the next switch-out ends it

    // Stage 5.3 - CPU cycles spent running, ever. See k_cpu_time.
    uint64_t    cycles;
} k_tcb_t;

typedef struct {
    k_tcb_t *current;
    uint32_t switches;       // total switches performed by this core
    // Where round-robin resumes, one place per priority level (stage 5.3).
    // It was one cursor for all levels, and a HIGH task being picked moved it
    // for everyone - see pick_next.
    uint8_t  rr_at[K_PRIO_LEVELS];
    bool     running;

    // Preemption state, per core and not shared.
    //
    // hold was global until SMP arrived, and that was a real bug: klog() on
    // core 0 raised it, which quietly stopped core 1 being preempted for the
    // length of a line printed by the other core entirely. A critical section
    // belongs to the core that entered it.
    uint32_t hold;           // depth of k_preempt_hold on this core
    uint32_t preempts;       // ticks that reached the scheduler on this core

    // Stage 5.3 - where this core's time went, in CCOUNT cycles. Each core
    // reads only its own CCOUNT, so these are only ever touched by their core.
    uint32_t in_cc;          // CCOUNT when current was last charged up to
    uint32_t idle_from;      // CCOUNT when the idle wait began
    bool     idling;         // in waiti inside the scheduler right now
    uint64_t busy_cycles;    // charged to some task
    uint64_t idle_cycles;    // waiti: nothing to run
    uint64_t park_cycles;    // held in the park interrupt while core 0 had flash
} k_percpu_t;

// --- lifecycle -------------------------------------------------------------
void     k_init(void);
int      k_task_create(const char *name, k_task_fn fn, void *arg);
int      k_task_create_on(const char *name, k_task_fn fn, void *arg, uint8_t cpu);
int      k_task_create_pri(const char *name, k_task_fn fn, void *arg,
                           uint8_t cpu, uint8_t prio);

// Id of a task caught overrunning its stack, or 0xFF if none has been. The
// check runs on every switch-out, so a task that blows its stack is stopped at
// the next scheduling point rather than corrupting whatever is below it.
uint8_t  k_stack_overflow_id(void);

// Stage 2.6b. Brings the calling core into the same scheduler, sharing one
// ready queue with core 1. Call it from a context that core can afford to lose
// - it switches into a kernel task and does not come back.
void     k_smp_join(void);
void     k_start(void);      // switches into the first task; does not return
void     k_yield(void);
void     k_exit(void);       // ends the calling task

// --- stage 5.2: ending a task from outside ----------------------------------
//
// k_exit only works from inside: a task that never gets there - an app stuck
// in a loop - could not be stopped at all. k_task_kill ends one from outside.
//
// ZOMBIE, NOT DEAD
//
// A DEAD task's stack is handed back to the heap by the next k_task_create
// that takes its slot. A killed task cannot be treated that way: it may have
// been waiting on a mailbox call whose request - and whose answer buffer - is
// on that stack, and core 0 may yet run the job and write into it. So a killed
// task becomes K_ZOMBIE, which k_task_create never reuses. Whoever killed it
// clears its mailbox slots (kmbox_forget), and only then k_task_reap turns it
// into an ordinary DEAD task.
//
// WHERE IT IS WHEN KILLED
//
// - READY, SLEEPING or BLOCKED, and on no core: ZOMBIE at once. If it was
//   blocked, its bit comes out of that wait set - left there, it would soak up
//   a wake-up meant for a real waiter.
// - Running on some core right now: flagged. The next time that core switches
//   it out - at the latest the next tick - it goes to ZOMBIE instead of READY.
//   A task that masks interrupts or holds preemption off forever cannot be
//   killed; nothing in the app SDK does either.
//
// A mutex the task held stays held: the kernel does not know which ones it
// took. Nothing an app can reach takes a kernel mutex.
typedef enum {
    K_KILL_GONE = 0,     // no such task any more (already ended, or slot reused)
    K_KILL_DONE,         // ZOMBIE now
    K_KILL_PENDING,      // running somewhere; ZOMBIE at its next switch-out
    K_KILL_REFUSED,      // it is the caller
} k_kill_t;

// Like k_task_create_pri, and also gives the new task's generation.
int      k_task_create_gen(const char *name, k_task_fn fn, void *arg,
                           uint8_t cpu, uint8_t prio, uint32_t *gen);

// The same with a stack of its own size, rounded up to 16 bytes and held to
// K_STACK_MIN..K_STACK_MAX (stage 5.4d). 0 means K_STACK_SIZE.
int      k_task_create_stack(const char *name, k_task_fn fn, void *arg,
                             uint8_t cpu, uint8_t prio, size_t stack_bytes,
                             uint32_t *gen);

// What that request turns into: the same rounding and clamping, so whoever
// has to find the memory first can ask for the true figure.
size_t   k_stack_bytes_for(size_t asked);
k_kill_t k_task_kill(uint8_t id, uint32_t gen);
typedef enum {
    K_TASK_ALIVE = 0,    // still runs (a pending kill included)
    K_TASK_ENDED,        // returned or k_exit-ed, or its slot is someone else's now
    K_TASK_ZOMBIE,       // killed and on no core: clear its mailbox slots, then reap
    K_TASK_DYING,        // killed, but some core is still on its stack: look again
} k_task_status_t;
k_task_status_t k_task_status(uint8_t id, uint32_t gen);

// --- stage 5.3: where the time goes -------------------------------------------
//
// Each core charges the task it is running whenever it switches it out and on
// every tick - the tick matters, because a task alone on a core is never
// switched out and would otherwise never be charged at all. Time a core spends
// in waiti with nothing to run is idle, not the waiting task's; time core 1
// spends held in the park interrupt while core 0 writes flash is park, not the
// interrupted task's. So for core 1, busy + idle + park is all of its time.
//
// Core 0 is shared with FreeRTOS, which switches tasks the kernel never sees:
// its figures here describe only the kernel's own use of it.
typedef struct {
    uint8_t     id;
    uint8_t     cpu;          // core it last ran on
    uint8_t     affinity;
    uint8_t     prio;
    k_state_t   state;
    uint32_t    gen;
    const char *name;
    uint64_t    cycles;       // CPU cycles run, ever
    uint32_t    stack_used;   // high-water, bytes
    uint32_t    stack_size;
} k_task_info_t;

typedef struct {
    uint64_t busy, idle, park;
} k_cpu_time_t;

int  k_task_snapshot(k_task_info_t *out, int max);   // every task not UNUSED
void k_cpu_time(int core, k_cpu_time_t *out);
void k_account_tick(void);        // the tick, on its own core: charge the runner
void k_account_park(uint32_t cycles);   // the park ISR: that long was park
void     k_task_reap(uint8_t id, uint32_t gen);        // ZOMBIE -> DEAD
uint32_t k_task_kills(void);                           // tasks killed, ever
size_t   k_free_dead_stacks(void);   // core 0 main loop: bytes handed back (stage 5.3c)

// --- introspection ---------------------------------------------------------
k_tcb_t    *k_current(void);
k_percpu_t *k_cpu(void);      // this core's run state
k_tcb_t    *k_task_by_id(uint8_t id);   // for the wait lists in ksync.cpp
uint32_t k_switch_count(void);
size_t   k_stack_high_water(const k_tcb_t *t);   // bytes of stack ever used
void     ktop(void);                             // dump the task table

// --- critical sections -----------------------------------------------------
// Two things at once, and both are needed: masking interrupts keeps the tick
// from re-entering the scheduler on THIS core, and the spinlock keeps the other
// core out of the task table. Callers go through these rather than writing rsil
// themselves precisely so that adding the second half did not touch a single
// call site.
//
// NOT RECURSIVE. Nothing may take this lock while already holding it, and
// nothing may call klog() while holding it either - klog takes the console
// lock, and a core holding the console lock and wanting this one would close
// the cycle.
static inline IRAM_ATTR uint32_t k_lock(void)
{
    uint32_t ps;
    __asm__ __volatile__("rsil %0, 3" : "=a"(ps) :: "memory");
    k_spin_lock_hard(&s3_sched_lock);
    return ps;
}

static inline IRAM_ATTR void k_unlock(uint32_t ps)
{
    k_spin_unlock(&s3_sched_lock);
    __asm__ __volatile__("wsr %0, ps; rsync" :: "a"(ps) : "memory");
}
