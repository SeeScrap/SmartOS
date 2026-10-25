#pragma once
#include <stddef.h>
#include <stdint.h>

#include "smartos_config.h"

#include "arch/xtensa/karch_irq.h"
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

// K_MAX_TASKS, K_MAX_CPUS, K_STACK_SIZE, K_STACK_MIN, K_STACK_MAX,
// K_STACK_GUARD and K_PRIO_LEVELS are set in smartos_config.h, section 1,
// each with the reason it is the number it is. The checks at the bottom of
// this header say what they may not be.

typedef void (*k_task_fn)(void *arg);

// A task, as far as anything outside the kernel is concerned: a handle, not a
// structure. The fields are in kernel/internal/ktask_internal.h, which is for
// the scheduler, the ISRs and the tests - anything that reaches for them is
// either part of the kernel or reporting on it.
typedef struct k_tcb k_tcb_t;

// Which core a task may run on. Most tasks say ANY and let the scheduler
// decide; the ones that must stay put are the per-core idles.
#define K_CPU_ANY  0xFF

// Priority. Lower number wins. Round-robin still applies, but only among tasks
// of equal priority - which is what makes it useful: a display task can be told
// to always beat a background one without having to hand-tune yields.
#define K_PRIO_HIGH    0
#define K_PRIO_NORMAL  8
#define K_PRIO_LOW     16
#define K_PRIO_IDLE    (K_PRIO_LEVELS - 1)

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
void     k_task_reap(uint8_t id, uint32_t gen);        // ZOMBIE -> DEAD
uint32_t k_task_kills(void);                           // tasks killed, ever

// --- introspection ---------------------------------------------------------
uint32_t k_switch_count(void);
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
static inline K_IRAM uint32_t k_lock(void)
{
    const uint32_t ps = karch_irq_disable();   // this core's tasks and its own
    k_spin_lock_hard(&k_sched_lock);           // interrupts out, then the other core
    return ps;
}

static inline K_IRAM void k_unlock(uint32_t ps)
{
    k_spin_unlock(&k_sched_lock);
    karch_irq_restore(ps);
}

// --- what the settings in smartos_config.h may not be ----------------------
// A configuration that cannot work should say so here, by name, rather than
// crash on the board. ksync.h holds the other one: K_MAX_TASKS against the
// width of a wait set.
static_assert(K_MAX_CPUS == 2,
              "the kernel names core 1 by number in kcore1 and kpark; "
              "K_MAX_CPUS is a fact about the chip, not a dial");
static_assert(K_STACK_MIN > K_STACK_GUARD,
              "a stack smaller than its own guard area has no room to run in "
              "- see K_STACK_GUARD in smartos_config.h");
static_assert(K_STACK_SIZE >= K_STACK_MIN && K_STACK_SIZE <= K_STACK_MAX,
              "the default stack must be a size a task is allowed to ask for");
static_assert(K_PRIO_IDLE < K_PRIO_LEVELS && K_PRIO_HIGH < K_PRIO_LEVELS,
              "every named priority must fit the ready table K_PRIO_LEVELS wide");
