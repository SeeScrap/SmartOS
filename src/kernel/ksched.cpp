#include "kernel/internal/ktask_internal.h"
#include "kernel/ktask.h"

#include <string.h>

#include "port/kport.h"
#include "kernel/klog.h"
#include "kernel/kheap.h"
#include "kernel/ktime.h"

// Every function here carries K_IRAM. Core 0 disables the shared flash
// cache whenever it writes flash, and a kernel that lives in flash would stop
// mid-instruction when that happens. Only the klog format strings still sit in
// flash .rodata - harmless while core 0 never writes flash, and something
// stage 2.5 can move into DRAM once we own the allocator.
extern "C" void k_ctx_switch(uint32_t **old_sp, uint32_t *new_sp);
extern "C" void k_task_trampoline(void);
extern "C" void k_chain_end(void);
extern "C" volatile uint32_t k_chain_end_hit;

// Stage 2.5 moved task stacks onto kmalloc. Before that they came from a fixed
// array, because the alternative was heap_caps_malloc() and that takes a
// FreeRTOS lock core 1 must never touch. Now the memory is the kernel's own.
static K_DRAM k_tcb_t  s_tcb[K_MAX_TASKS];
static K_DRAM k_percpu_t s_cpu[K_MAX_CPUS];
static K_DRAM uint32_t s_next_gen;   // scheduler lock
static K_DRAM uint32_t s_kills;

static void k_end(k_state_t final);    // k_exit, and a killed task's last switch

// Is some core standing on this task's stack right now? Scheduler lock held.
static inline K_IRAM bool is_anyones_current(const k_tcb_t *t)
{
    for (int k = 0; k < K_MAX_CPUS; ++k) {
        if (s_cpu[k].current == t) return true;
    }
    return false;
}

// Written over the whole stack at creation so k_stack_high_water() can find the
// deepest point the task ever reached by looking for where the pattern stops.
#define K_STACK_FILL 0xA5A5A5A5u

// The lowest word of every stack. Checked each time a task is switched out,
// which is the cheapest place to catch an overrun that is still recoverable -
// one word compared per switch, against silently corrupting whatever the
// allocator put underneath.
static K_DRAM uint8_t s_overflow_id = 0xFF;

K_IRAM k_percpu_t *k_cpu(void)
{
    uint32_t id = kport_core_id();
    if (id >= K_MAX_CPUS) id = 0;   // stage 2.6b widens the array instead
    return &s_cpu[id];
}

// ---------------------------------------------------------------------------
// Build a stack that k_ctx_switch() can return onto for the very first time.
//
// The layout has to match what retw expects:
//   sp[-4..-1]  the base save area, which the window-underflow handler reloads
//               into a0..a3 of the frame we land in
//   sp[0]       the resume address k_ctx_switch() loads into a0
// ---------------------------------------------------------------------------
static K_IRAM uint32_t *k_stack_init(uint8_t *base, size_t size)
{
    uintptr_t top = ((uintptr_t)base + size) & ~(uintptr_t)0xF;

    // Zeroed, not fill-patterned, at the top. Under preemption the window
    // machinery walks further down a fresh stack than the cooperative path ever
    // did, and 0xA5A5A5A5 read as a return address is what the first bring-up
    // ended up jumping to.
    for (uint32_t *p = (uint32_t *)(top - 192); p < (uint32_t *)top; ++p) *p = 0;

    // Three dummy frames, each linking to real writable stack above it. A zero
    // in an a1 link means a spill writes to address 0; a zero in an a0 link is
    // an illegal retw encoding. Neither is allowed to appear here.
    uint32_t *guard = (uint32_t *)(top - 32);
    uint32_t *outer = (uint32_t *)(top - 64);
    uint32_t *sp    = (uint32_t *)(top - 96);

    guard[-4] = (uint32_t)(uintptr_t)&k_chain_end;
    guard[-3] = (uint32_t)top;

    outer[-4] = (uint32_t)(uintptr_t)&k_chain_end;
    outer[-3] = (uint32_t)(uintptr_t)guard;

    sp[-4] = (uint32_t)(uintptr_t)&k_chain_end;   // a0 of the frame we land in
    sp[-3] = (uint32_t)(uintptr_t)outer;          // a1 of that frame

    // a0[31:30] of a code address on this chip is already 0b01, the call4
    // encoding retw wants. See the note in kswitch.S.
    sp[0] = (uint32_t)(uintptr_t)&k_task_trampoline;

    return sp;
}

K_IRAM void k_init(void)
{
    memset(s_tcb, 0, sizeof(s_tcb));
    memset(s_cpu, 0, sizeof(s_cpu));
    kheap_init();
}

K_IRAM int k_task_create(const char *name, k_task_fn fn, void *arg)
{
    return k_task_create_on(name, fn, arg, K_CPU_ANY);
}

K_IRAM int k_task_create_on(const char *name, k_task_fn fn, void *arg,
                               uint8_t cpu)
{
    return k_task_create_pri(name, fn, arg, cpu, K_PRIO_NORMAL);
}

K_IRAM int k_task_create_pri(const char *name, k_task_fn fn, void *arg,
                                uint8_t cpu, uint8_t prio)
{
    return k_task_create_gen(name, fn, arg, cpu, prio, nullptr);
}

K_IRAM size_t k_stack_bytes_for(size_t asked)
{
    size_t n = asked ? asked : K_STACK_SIZE;
    if (n < K_STACK_MIN) n = K_STACK_MIN;
    if (n > K_STACK_MAX) n = K_STACK_MAX;
    return (n + 15u) & ~(size_t)15u;          // stacks stay 16-byte aligned
}

K_IRAM int k_task_create_gen(const char *name, k_task_fn fn, void *arg,
                                uint8_t cpu, uint8_t prio, uint32_t *gen)
{
    return k_task_create_stack(name, fn, arg, cpu, prio, K_STACK_SIZE, gen);
}

K_IRAM int k_task_create_stack(const char *name, k_task_fn fn, void *arg,
                                  uint8_t cpu, uint8_t prio, size_t stack_bytes,
                                  uint32_t *gen)
{
    const size_t size = k_stack_bytes_for(stack_bytes);

    // The stack is taken before the lock: kmalloc locks for itself, and this
    // kernel does not do recursive locks.
    uint8_t *stack = (uint8_t *)kmalloc(K_STACK_GUARD + size);
    if (!stack) {
        klog("[kernel] no memory for a %u byte stack\n", (unsigned)size);
        return -1;
    }

    uint32_t ps = k_lock();

    // A task cannot free the stack it is standing on when it exits, so the
    // next user of its slot returns it instead.
    //
    // Two rules, both learned the hard way in stage 4.3b, the first time tasks
    // were ever created after others had died:
    //
    // - Reclaim only the stack of the slot actually chosen. This loop used to
    //   note the first DEAD slot's stack for freeing and then carry on - and
    //   if an UNUSED slot turned up later it took that one instead, still
    //   freeing the dead slot's stack. The dead slot kept its pointer, so the
    //   next create freed the same block AGAIN, by which time kmalloc had
    //   handed it to someone else: two apps ended up sharing one state block.
    //
    // - Never reclaim a dead task that is still some core's current task. A
    //   task that has just called k_exit can be idling in waiti on its own
    //   stack until something else is ready to run; freeing that stack would
    //   let the next interrupt frame land in whoever gets the memory next.
    //
    // Dead slots are preferred, so memory comes back as soon as it can.
    int unused = -1, dead = -1;
    for (int i = 0; i < K_MAX_TASKS; ++i) {
        if (s_tcb[i].state == K_UNUSED && unused < 0) unused = i;
        if (s_tcb[i].state == K_DEAD && dead < 0 && !is_anyones_current(&s_tcb[i])) {
            dead = i;
        }
    }
    const int slot    = (dead >= 0) ? dead : unused;
    uint8_t  *reclaim = (dead >= 0) ? s_tcb[dead].stack : nullptr;
    if (slot < 0) {
        k_unlock(ps);
        kfree(stack);
        return -1;
    }

    k_tcb_t *t   = &s_tcb[slot];
    t->stack      = stack;
    t->stack_size = size;
    t->entry      = fn;
    t->arg        = arg;
    t->name       = name;
    t->id         = (uint8_t)slot;
    t->cpu        = (uint8_t)kport_core_id();
    t->affinity   = cpu;
    t->prio       = (prio < K_PRIO_LEVELS) ? prio : K_PRIO_IDLE;   // indexes rr_at
    t->switches   = 0;
    t->cycles     = 0;
    t->gen        = ++s_next_gen;
    t->blocked_on = nullptr;
    t->kill       = 0;
    if (gen) *gen = t->gen;

    // t->stack is the whole allocation: guard region first, then the usable
    // stack. Both get the fill pattern, so the canary and the high-water mark
    // read the same way.
    uint32_t *w = (uint32_t *)t->stack;
    for (size_t i = 0; i < (K_STACK_GUARD + size) / 4; ++i) {
        w[i] = K_STACK_FILL;
    }

    t->sp    = k_stack_init(t->stack + K_STACK_GUARD, t->stack_size);
    t->state = K_READY;

    k_unlock(ps);

    if (reclaim) kfree(reclaim);   // outside the lock: kfree takes it itself
    return slot;
}

// Promote any sleeper whose deadline has passed. The subtraction is done on
// signed values so that a tick counter wrapping past 2^32 does not strand a
// sleeping task forever.
static K_IRAM void wake_due_sleepers(void)
{
    uint32_t now = k_tick_count;
    for (int i = 0; i < K_MAX_TASKS; ++i) {
        if (s_tcb[i].state == K_SLEEPING &&
            (int32_t)(now - s_tcb[i].wake_tick) >= 0) {
            s_tcb[i].state = K_READY;
        }
    }
}

K_IRAM k_tcb_t *k_task_by_id(uint8_t id)
{
    return (id < K_MAX_TASKS) ? &s_tcb[id] : nullptr;
}

// Is anything going to become runnable if we simply wait?
//
// A sleeper will, when the tick says so. A blocked task will, when whoever
// holds what it wants lets go - which needs some other core to be running, so
// waiting on that is only useful in an SMP build, but it is never wrong.
static K_IRAM bool any_waiting(void)
{
    for (int i = 0; i < K_MAX_TASKS; ++i) {
        if (s_tcb[i].state == K_SLEEPING || s_tcb[i].state == K_BLOCKED) {
            return true;
        }
    }
    return false;
}

// Is this task some OTHER core's current task?
//
// Its state cannot answer that. A task that sleeps with nothing else to run
// does not switch away: k_yield idles in waiti inside the task's own context,
// on its own stack, with the scheduler lock dropped - and wake_due_sleepers
// marks it READY the moment its deadline passes. Until 2026-09-18 that was all
// the other core needed to claim it and switch to the stack pointer saved at
// its last real switch: a stale context, and the same task running on both
// cores at once. SMARTOS_TEST_SLEEP_CLAIM reproduced it as a double exception on
// core 0 within the first few sleeps.
//
// A core's current task is exactly the one whose stack that core is on - once
// core 0 joins it never switches back to its host stack - so this is the test
// that means "free to be resumed", whatever the state field says.
static K_IRAM bool on_other_core(const k_tcb_t *t, uint8_t me)
{
    for (int k = 0; k < K_MAX_CPUS; ++k) {
        if (k != me && s_cpu[k].current == t) return true;
    }
    return false;
}

// --- stage 5.3: accounting ------------------------------------------------------
// Everything below runs on the core whose numbers it touches, with that core's
// interrupts masked (the scheduler lock, or inside the tick or park ISR), so
// none of it races the rest.

static inline K_IRAM uint32_t ccount(void)
{
    uint32_t cc;
    __asm__ __volatile__("rsr %0, ccount" : "=a"(cc));
    return cc;
}

// Charges t with everything since this core last charged anyone.
static inline K_IRAM void charge(k_percpu_t *c, k_tcb_t *t, uint32_t now)
{
    const uint32_t d = now - c->in_cc;
    if (t) t->cycles += d;
    c->busy_cycles += d;
    c->in_cc = now;
}

// The scheduler's "nothing to run until an interrupt" wait. Called with the
// lock held; returns with it held again. Interrupts have to be open for waiti
// to ever end, so the tick can land in here - inside the scheduler. Holding
// preemption lets it count the tick without calling us again from within.
// The wait itself is idle time, not the waiting task's.
static K_IRAM void idle_wait(k_percpu_t *c, k_tcb_t *cur, uint32_t *ps)
{
    const uint32_t now = ccount();
    charge(c, cur, now);
    c->idle_from = now;
    c->idling    = true;

    k_preempt_hold();
    k_unlock(*ps);
    __asm__ __volatile__("waiti 0");
    *ps = k_lock();
    k_preempt_release();

    const uint32_t after = ccount();
    c->idle_cycles += after - c->idle_from;
    c->idling = false;
    c->in_cc  = after;
}

K_IRAM void k_account_tick(void)
{
    k_percpu_t *c = k_cpu();
    if (c->running && !c->idling) charge(c, c->current, ccount());
}

K_IRAM void k_account_park(uint32_t cycles)
{
    // Moved forward past the park, so neither the interrupted task nor the
    // idle wait it may have interrupted is charged for it.
    k_percpu_t *c = k_cpu();
    c->park_cycles += cycles;
    c->in_cc       += cycles;
    c->idle_from   += cycles;
}

K_IRAM int k_task_snapshot(k_task_info_t *out, int max)
{
    int n = 0;
    uint32_t ps = k_lock();
    for (int i = 0; i < K_MAX_TASKS && n < max; ++i) {
        const k_tcb_t *t = &s_tcb[i];
        if (t->state == K_UNUSED) continue;
        k_task_info_t *o = &out[n++];
        o->id         = t->id;
        o->cpu        = t->cpu;
        o->affinity   = t->affinity;
        o->prio       = t->prio;
        o->state      = t->state;
        o->gen        = t->gen;
        o->name       = t->name;
        o->cycles     = t->cycles;
        o->stack_size = (uint32_t)t->stack_size;
        o->stack_used = 0;
    }
    k_unlock(ps);
    // The high-water scan reads a whole stack: outside the lock. A task that
    // ends meanwhile just reads as its last value.
    for (int i = 0; i < n; ++i) {
        const k_tcb_t *t = &s_tcb[out[i].id];
        if (t->gen == out[i].gen && t->stack) out[i].stack_used = (uint32_t)k_stack_high_water(t);
    }
    return n;
}

K_IRAM void k_cpu_time(int core, k_cpu_time_t *out)
{
    if (core < 0 || core >= K_MAX_CPUS) { *out = {}; return; }
    uint32_t ps = k_lock();
    out->busy = s_cpu[core].busy_cycles;
    out->idle = s_cpu[core].idle_cycles;
    out->park = s_cpu[core].park_cycles;
    k_unlock(ps);
}

// Could this core run t right now?
static inline K_IRAM bool runnable_here(const k_tcb_t *t, uint8_t me)
{
    return t->state == K_READY &&
           (t->affinity == K_CPU_ANY || t->affinity == me) &&
           !on_other_core(t, me);
}

// The best priority wins; within it, round robin.
//
// One ready queue, two cores picking from it. A task is claimed by being moved
// out of K_READY under the scheduler lock, which is what stops both cores
// walking away with the same one.
//
// ROUND ROBIN IS KEPT PER PRIORITY LEVEL
//
// Until stage 5.3 there was one cursor per core: scan from it, keep the best
// priority seen, and move the cursor to whoever won. The comment here said
// that kept round robin fair within each level. It did not. The input sampler
// is HIGH and wakes every 2 ms; each time it won, the cursor moved to its own
// low slot, and the next scan for a NORMAL task started from there - so the
// first two NORMAL tasks in the table took turns and a third never ran at all.
// Measured: three apps spinning, 49.7 %, 49.9 % and 0.0 % of core 1; the task
// manager, as that third app, got 0 ms in three seconds. Now each level has its
// own place in the rotation, and a higher level being served leaves it alone.
static K_IRAM k_tcb_t *pick_next(k_percpu_t *c)
{
    wake_due_sleepers();
    const uint8_t me = (uint8_t)kport_core_id();

    // The best level anything runnable is at.
    int level = K_PRIO_LEVELS;
    for (int i = 0; i < K_MAX_TASKS; ++i) {
        const k_tcb_t *t = &s_tcb[i];
        if (t->prio < level && runnable_here(t, me)) level = t->prio;
    }
    if (level >= K_PRIO_LEVELS) return nullptr;

    // The next one at that level after the last one it ran.
    uint8_t *at = &c->rr_at[level];
    for (int n = 1; n <= K_MAX_TASKS; ++n) {
        const int i = (*at + n) % K_MAX_TASKS;
        k_tcb_t *t = &s_tcb[i];
        if (t->prio == level && runnable_here(t, me)) {
            *at = (uint8_t)i;
            return t;
        }
    }
    return nullptr;                  // cannot happen: pass one found one
}

K_IRAM void k_yield(void)
{
    uint32_t ps = k_lock();

    k_percpu_t *c   = k_cpu();
    k_tcb_t    *cur = c->current;

    // Killed while it ran (stage 5.2): this switch-out is its last. Whether the
    // tick brought it here or it came itself - to sleep, block or wait on the
    // mailbox - it does not get to carry on.
    if (cur && cur->kill) {
        k_unlock(ps);
        k_end(K_ZOMBIE);
    }

    k_tcb_t    *nxt = pick_next(c);

    // The caller may have just put itself to sleep. If nothing else can run,
    // there is genuinely nothing to do until the tick fires, so idle in waiti
    // rather than spinning. Interrupts have to be unmasked for waiti to end,
    // which is why the lock is dropped around it. A kill ends the wait too:
    // otherwise the task would idle here until whatever it was waiting for
    // arrived, and then simply carry on.
    while (!nxt && cur &&
           (cur->state == K_SLEEPING || cur->state == K_BLOCKED) &&
           any_waiting() && !cur->kill) {
        idle_wait(c, cur, &ps);
        nxt = pick_next(c);
    }
    if (cur && cur->kill) {
        k_unlock(ps);
        k_end(K_ZOMBIE);
    }

    // cur is null only if k_yield is reached before k_start has installed a
    // current task. Switching from nothing would write through a null TCB.
    if (!nxt || nxt == cur || !cur) {
        // Picking ourselves means we woke from a sleep or a block, in place,
        // and were marked READY while never having stopped running. Claim it
        // back: a running task left READY is one the other core's scan would
        // consider free, and on_other_core() should be the second line of
        // defence against that, not the only one.
        if (cur && nxt == cur) cur->state = K_RUNNING;
        k_resched_clear();
        k_unlock(ps);
        return;
    }

    k_resched_clear();

    // Switching out is the moment the outgoing stack stops being written to,
    // so it is the moment to ask whether it stayed inside its bounds.
    // The word immediately below the usable stack is the first thing an
    // overrun touches, so that is the one worth checking.
    if (cur && cur->stack &&
        *(volatile uint32_t *)(cur->stack + K_STACK_GUARD - 4) != K_STACK_FILL) {
        // Record it and carry on. Killing the task here was the first
        // instinct and it wedged the system: k_exit's bookkeeping never runs
        // for a task retired mid-yield, and core 0 sat spinning on a scheduler
        // lock that nobody was going to release. Reporting is the useful part;
        // the run-off area has already absorbed the damage.
        if (s_overflow_id == 0xFF) s_overflow_id = cur->id;
    }
    if (cur && cur->state == K_RUNNING) cur->state = K_READY;
    nxt->state = K_RUNNING;
    nxt->cpu   = (uint8_t)kport_core_id();
    nxt->switches++;
    charge(c, cur, ccount());   // the outgoing task's stretch (stage 5.3)
    c->current = nxt;
    c->switches++;

    // Interrupts stay masked across the switch. The context we resume restores
    // its own PS when its k_yield() returns, so masking does not leak between
    // tasks.
    k_ctx_switch(&cur->sp, nxt->sp);

    k_unlock(ps);
}

K_IRAM void k_exit(void)
{
    k_end(K_DEAD);
}

static K_IRAM void k_end(k_state_t final)
{
    // No k_preempt_hold here. k_exit never returns on the path that matters, so
    // the matching release would never run and preemption would be off for the
    // rest of the system's life. Interrupts are masked by k_lock across the
    // whole body anyway, and the one place they reopen - the waiti loop below -
    // holds and releases in balance.
    uint32_t ps = k_lock();

    k_percpu_t *c   = k_cpu();
    k_tcb_t    *cur = c->current;
    // Marked dead, but the stack stays allocated: we are still standing on it.
    // k_task_create reclaims it when the slot is reused - for a ZOMBIE, only
    // once k_task_reap has made it DEAD.
    if (cur) cur->state = final;

    // "Nothing is READY" is not the same as "nothing is left". A task that is
    // only sleeping will become runnable when the tick says so, and the exiting
    // task must wait for it instead of declaring the system finished. The first
    // run of this kernel got that wrong and halted the core while two tasks
    // were still mid-sleep.
    k_tcb_t *nxt = pick_next(c);
    while (!nxt && any_waiting()) {
        idle_wait(c, cur, &ps);
        nxt = pick_next(c);
    }

    if (!nxt) {
        // Now it really is over. With nothing left to schedule, the tick has no
        // business calling the scheduler any more - it would be running k_yield
        // against a current task that is already dead.
        k_preempt_disable();
        k_unlock(ps);           // klog takes the console lock; never nest the two
        klog("[kernel] last task exited, halting core %d\n",
             (int)kport_core_id());
        for (;;) { __asm__ __volatile__("waiti 0"); }
    }

    // Handing the core to someone else for good: the hold is never released,
    // because this stack is not coming back.

    nxt->state = K_RUNNING;
    nxt->switches++;
    charge(c, cur, ccount());   // the dying task's last stretch
    c->current = nxt;
    c->switches++;

    // Our stack is about to stop mattering, but k_ctx_switch still needs
    // somewhere to write the outgoing pointer.
    static K_DRAM uint32_t *discard;
    k_ctx_switch(&discard, nxt->sp);

    // Unreachable: a dead task is never picked again.
    for (;;) { }
}

// --- stage 5.2: killing, from outside ------------------------------------------
// The rules, and why ZOMBIE exists, are in ktask.h.

// The task (id, gen), if that is still what the slot holds. Lock held.
static inline K_IRAM k_tcb_t *same_task(uint8_t id, uint32_t gen)
{
    if (id >= K_MAX_TASKS) return nullptr;
    k_tcb_t *t = &s_tcb[id];
    return (t->gen == gen && t->state != K_UNUSED) ? t : nullptr;
}

K_IRAM k_kill_t k_task_kill(uint8_t id, uint32_t gen)
{
    uint32_t ps = k_lock();
    k_tcb_t *t = same_task(id, gen);
    k_kill_t r;
    if (!t || t->state == K_DEAD || t->state == K_ZOMBIE) {
        r = K_KILL_GONE;
    } else if (t == k_cpu()->current) {
        r = K_KILL_REFUSED;
    } else {
        // Out of whatever it was waiting in, first, whichever way this goes:
        // the bit would otherwise be taken for a real waiter by wake_one.
        if (t->state == K_BLOCKED && t->blocked_on) {
            *t->blocked_on &= ~(uint32_t)(1u << t->id);
        }
        t->blocked_on = nullptr;
        if (is_anyones_current(t)) {
            t->kill = 1;               // k_yield ends it at its next switch-out
            r = K_KILL_PENDING;
        } else {
            t->state = K_ZOMBIE;
            r = K_KILL_DONE;
        }
        s_kills++;
    }
    k_unlock(ps);
    return r;
}

K_IRAM k_task_status_t k_task_status(uint8_t id, uint32_t gen)
{
    uint32_t ps = k_lock();
    k_tcb_t *t = same_task(id, gen);
    k_task_status_t s;
    if (!t || t->state == K_DEAD)  s = K_TASK_ENDED;
    else if (t->state == K_ZOMBIE) s = is_anyones_current(t) ? K_TASK_DYING : K_TASK_ZOMBIE;
    else                           s = K_TASK_ALIVE;
    k_unlock(ps);
    return s;
}

K_IRAM void k_task_reap(uint8_t id, uint32_t gen)
{
    uint32_t ps = k_lock();
    k_tcb_t *t = same_task(id, gen);
    if (t && t->state == K_ZOMBIE && !is_anyones_current(t)) t->state = K_DEAD;
    k_unlock(ps);
}

K_IRAM uint32_t k_task_kills(void) { return s_kills; }

// Stage 5.3c. A task cannot free the stack it is standing on, so until now a
// DEAD task's stack went back to the heap only when k_task_create happened to
// reuse its slot - and the thirteen tasks the boot self-tests finish never had
// their slots reused: 29,952 bytes, 72 % of what the kernel heap had in use
// with nothing open. Core 0's main loop now hands them back as soon as no core
// is on them. ZOMBIE tasks are not touched here: their stacks wait for
// kmbox_forget and k_task_reap (stage 5.2).
K_IRAM size_t k_free_dead_stacks(void)
{
    uint8_t *gone[K_MAX_TASKS];
    int n = 0;
    size_t bytes = 0;
    uint32_t ps = k_lock();
    for (int i = 0; i < K_MAX_TASKS; ++i) {
        k_tcb_t *t = &s_tcb[i];
        if (t->state == K_DEAD && t->stack && !is_anyones_current(t)) {
            gone[n++] = t->stack;
            bytes += K_STACK_GUARD + t->stack_size;   // stacks differ in size
            t->stack = nullptr;        // the slot stays DEAD, reusable, stackless
        }
    }
    k_unlock(ps);
    for (int i = 0; i < n; ++i) kfree(gone[i]);   // kfree takes the lock itself
    return bytes;
}

// Called from k_task_trampoline. Never returns.
extern "C" K_IRAM void k_task_launch(void)
{
    // The context that switched to us was holding the scheduler lock and had
    // interrupts masked, and it is not coming back to undo either. A task
    // starting for the first time is the one place the release has to be done
    // by hand rather than falling out of a matching k_unlock.
    k_spin_unlock(&k_sched_lock);

    uint32_t old;
    __asm__ __volatile__("rsil %0, 0" : "=a"(old) :: "memory");

    k_tcb_t *t = k_current();
    if (t && t->entry) t->entry(t->arg);

    k_exit();
    for (;;) { }
}

K_IRAM void k_start(void)
{
    k_percpu_t *c = k_cpu();

    uint32_t ps = k_lock();
    k_tcb_t *first = pick_next(c);
    if (!first) {
        k_unlock(ps);
        klog("[kernel] k_start with no runnable task\n");
        return;
    }

    first->state = K_RUNNING;
    first->switches++;
    c->in_cc   = ccount();      // its time starts now (stage 5.3)
    c->current = first;
    c->switches++;
    c->running = true;

    // The context that called k_start is abandoned - core 1 has nothing to go
    // back to - but k_ctx_switch still needs a slot to write into.
    static K_DRAM uint32_t *boot_sp;
    k_ctx_switch(&boot_sp, first->sp);

    // Unreachable.
    k_unlock(ps);
}

// Stage 2.6b - the other core joining in.
//
// This is k_start for a core that arrives late. It switches into a kernel task
// and never comes back, so the caller has to be something that core can spare:
// on core 0 that is a FreeRTOS task created for the purpose. The core keeps
// FreeRTOS alive because one of the kernel tasks pinned to it hands the slice
// straight back with vTaskDelay.
K_IRAM void k_smp_join(void)
{
    k_percpu_t *c = k_cpu();

    uint32_t ps = k_lock();
    k_tcb_t *first = pick_next(c);
    if (!first) {
        k_unlock(ps);
        return;                 // nothing this core is allowed to run, yet
    }

    first->state = K_RUNNING;
    first->cpu   = (uint8_t)kport_core_id();
    first->switches++;
    c->in_cc   = ccount();      // its time starts now (stage 5.3)
    c->current = first;
    c->switches++;
    c->running = true;

    static K_DRAM uint32_t *host_sp[K_MAX_CPUS];
    k_ctx_switch(&host_sp[kport_core_id() % K_MAX_CPUS], first->sp);

    k_unlock(ps);               // only reached if the switch somehow returns
}

K_IRAM k_tcb_t *k_current(void)
{
    return k_cpu()->current;
}

K_IRAM uint32_t k_switch_count(void)
{
    return k_cpu()->switches;
}

K_IRAM uint8_t k_stack_overflow_id(void)
{
    return s_overflow_id;
}

K_IRAM size_t k_stack_high_water(const k_tcb_t *t)
{
    if (!t || !t->stack) return 0;
    const uint32_t *w = (const uint32_t *)(t->stack + K_STACK_GUARD);
    size_t words = t->stack_size / 4;
    size_t untouched = 0;
    while (untouched < words && w[untouched] == K_STACK_FILL) untouched++;
    return t->stack_size - untouched * 4;
}

static K_IRAM const char *state_name(k_state_t s)
{
    switch (s) {
        case K_READY:    return "READY";
        case K_RUNNING:  return "RUN";
        case K_SLEEPING: return "SLEEP";
        case K_BLOCKED:  return "BLOCK";
        case K_DEAD:    return "DEAD";
        case K_ZOMBIE:  return "ZOMBIE";
        default:        return "-";
    }
}

K_IRAM void ktop(void)
{
    klog("\n  id name        state  cpu aff pri  switches  stack used/total\n");
    klog("  -- ----------- ------ --- --- ---  --------- ----------------\n");
    for (int i = 0; i < K_MAX_TASKS; ++i) {
        k_tcb_t *t = &s_tcb[i];
        if (t->state == K_UNUSED) continue;
        char aff = (t->affinity == K_CPU_ANY) ? '*' : (char)('0' + t->affinity);
        klog("  %2d %-11s %-6s  %d   %c  %2d   %9u  %5u/%5u\n",
             t->id, t->name ? t->name : "?", state_name(t->state),
             (int)t->cpu, aff, (int)t->prio, (unsigned)t->switches,
             (unsigned)k_stack_high_water(t), (unsigned)t->stack_size);
    }
    for (int cpu = 0; cpu < K_MAX_CPUS; ++cpu) {
        klog("  core %d: %u switches, %u tick-driven%s\n",
             cpu, (unsigned)s_cpu[cpu].switches, (unsigned)s_cpu[cpu].preempts,
             s_cpu[cpu].running ? "" : "  (never joined the scheduler)");
    }
    klog("  preempt %s\n", k_preempt_enabled() ? "on" : "off");
    if (s_overflow_id != 0xFF) {
        klog("  !! task %d overran its stack (it kept running - the run-off\n",
             (int)s_overflow_id);
        klog("     area below it absorbed the damage, but its stack is too small)\n");
    }
    klog("\n");
}
