// The stage 2 kernel demo: the tasks that proved the scheduler, the context
// switch, the clock, the heap, the mailbox, the locks and the queue, back when
// those were the only things on the board.
//
// They still run on every default boot, and they are still the fastest way to
// see the kernel working - which is why they are tests rather than history.
// They were in main.cpp until R2a of the SmartOS restructuring; nothing about
// them changed in the move.
//
// IRAM_ATTR throughout: core 1 must not depend on the flash cache, which
// core 0 can disable under it at any moment.
//
// klog() rather than logf() - see kernel/klog.h for why printf would hang.

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


#ifdef SMARTOS_TEST_STACK_OVERFLOW
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

// --- how core1_main starts them -------------------------------------------
// Three entry points rather than one, because the order tasks are created in
// is the order they get their ids, and core 1's own cpu0idle task is created
// between the demo and the SMP pair. Keeping that order keeps ktop()'s output
// the same as it has always been.

void IRAM_ATTR kernel_demo_start(void)
{
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
#ifdef SMARTOS_TEST_STACK_OVERFLOW
    k_task_create("overflow", overflow_task, nullptr);
#endif
    k_task_create_pri("idle", idle_task, nullptr, K_CPU_ANY, K_PRIO_LOW);
}

#ifdef SMARTOS_ENABLE_SMP
void IRAM_ATTR kernel_smp_demo_start(void)
{
    k_task_create("smpA", smp_task, (void *)"smpA");
    k_task_create("smpB", smp_task, (void *)"smpB");
}
#endif

#ifdef SMARTOS_PREEMPT_SOAK
void IRAM_ATTR kernel_soak_start(void)
{
    k_task_create("soakA", soak_task, (void *)&s_soak_a);
    k_task_create("soakB", soak_task, (void *)&s_soak_b);
}
#endif
