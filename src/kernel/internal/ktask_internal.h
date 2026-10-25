#pragma once

// ===========================================================================
// The task table from the inside.
// ตารางงานเมื่อมองจากข้างใน
// ===========================================================================
//
// kernel/ktask.h is what a program written on this kernel sees: create a task,
// yield, sleep, end it, ask how it is doing. This is the rest - the structures
// the scheduler walks, the hooks the interrupt handlers call, and the lock
// they all take.
//
// Three kinds of caller have a reason to include this:
//   - the kernel itself (ksched, ksync, ktime, kmbox, kheap)
//   - the port layer, for the park interrupt's accounting
//   - the self-tests and the boot report, which exist to look inside
//
// An application has no business here, and from R6 it cannot reach it by
// accident: include/smartos_app.h does not lead to this file.
//
// แอปพลิเคชันไม่มีธุระในไฟล์นี้ และตั้งแต่ R6 เป็นต้นไปก็เอื้อมมาโดยบังเอิญไม่ได้
// เพราะ include/smartos_app.h ไม่ได้พาลงมาที่นี่
// ---------------------------------------------------------------------------

#include "kernel/ktask.h"
#include "kernel/kspin.h"

// --- the task, in full -----------------------------------------------------
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

// --- what each core is doing -----------------------------------------------
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

// --- for core 0's main loop ------------------------------------------------
size_t   k_free_dead_stacks(void);   // core 0 main loop: bytes handed back (stage 5.3c)

// --- for the interrupt handlers --------------------------------------------
void k_account_tick(void);        // the tick, on its own core: charge the runner
void k_account_park(uint32_t cycles);   // the park ISR: that long was park

// --- the live table --------------------------------------------------------
k_tcb_t    *k_current(void);
k_percpu_t *k_cpu(void);      // this core's run state
k_tcb_t    *k_task_by_id(uint8_t id);   // for the wait lists in ksync.cpp
size_t   k_stack_high_water(const k_tcb_t *t);   // bytes of stack ever used

