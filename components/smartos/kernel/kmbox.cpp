#include "kernel/kmbox.h"

#include <string.h>

#include "kernel/klog.h"
#include "kernel/kspin.h"
#include "kernel/internal/ktask_internal.h"
#include "kernel/ktask.h"
#include "kernel/ktime.h"
#include "port/kport.h"

// The slots must be somewhere both cores can reach with the flash cache off,
// and in internal SRAM specifically: S32C1I, which every hand-over below uses,
// does not work on PSRAM.
static K_DRAM k_job_t  s_slots[KMBOX_SLOTS];
static K_DRAM uint32_t s_served;
static volatile K_DRAM uint32_t s_withdrawn;
static volatile K_DRAM uint32_t s_abandoned;
static kmbox_handler_t              s_handlers[KMBOX_OPS];   // core 0 only

static inline K_IRAM void barrier(void)
{
    __asm__ __volatile__("memw" ::: "memory");
}

K_IRAM void kmbox_init(void)
{
    memset((void *)s_slots, 0, sizeof(s_slots));
    s_served    = 0;
    s_withdrawn = 0;
    s_abandoned = 0;
    barrier();
}

// --- poster side (any kernel task, either core) -----------------------------

K_IRAM int kmbox_post(uint32_t op, uint32_t a0, uint32_t a1, uint32_t a2)
{
    for (int i = 0; i < KMBOX_SLOTS; ++i) {
        // A cheap look first, so a full scan does not hammer the bus with
        // atomics on slots that are obviously taken.
        if (s_slots[i].state != KMB_EMPTY) continue;
#ifdef SMARTOS_TEST_MBOX_STRESS
        kport_delay_us(20);      // test only: hold the race window open
#endif
        // The look is only a hint. Another poster - on the other core, or on
        // this one after a preemption right here - may have seen the same
        // EMPTY. Only one compare-and-swap can win, and the loser moves on.
        //
        // Preemption is held from the claim until the slot says whose it is:
        // a poster killed in between would leave a CLAIMED slot naming its
        // previous owner, which kmbox_forget could never find.
        k_preempt_hold();
        if (!k_cas(&s_slots[i].state, KMB_EMPTY, KMB_CLAIMED)) {
            k_preempt_release();
            continue;
        }
        const k_tcb_t *me = k_current();
        s_slots[i].owner     = me ? me->id : 0xFF;
        s_slots[i].owner_gen = me ? me->gen : 0;
        k_preempt_release();

        s_slots[i].op     = op;
        s_slots[i].a0     = a0;
        s_slots[i].a1     = a1;
        s_slots[i].a2     = a2;
        s_slots[i].result = 0;
        s_slots[i].status = 0;

        // CLAIMED belongs to us alone, so publishing needs no atomic - only
        // the barrier that lands the payload before the state says it is there.
        barrier();
        s_slots[i].state = KMB_POSTED;
        barrier();
        return i;
    }
    return -1;                   // mailbox full
}

K_IRAM bool kmbox_done(int slot)
{
    if (slot < 0 || slot >= KMBOX_SLOTS) return false;
    return s_slots[slot].state == KMB_DONE;
}

K_IRAM uint32_t kmbox_result(int slot)
{
    if (slot < 0 || slot >= KMBOX_SLOTS) return 0;
    return s_slots[slot].result;
}

K_IRAM void kmbox_release(int slot)
{
    if (slot < 0 || slot >= KMBOX_SLOTS) return;
    // DONE belongs to the poster alone, so this is a plain store.
    barrier();
    s_slots[slot].state = KMB_EMPTY;
    barrier();
}

K_IRAM bool kmbox_call_timeout(uint32_t op, uint32_t a0, uint32_t a1,
                                  uint32_t a2, uint32_t *result,
                                  uint32_t timeout_ms)
{
    int slot = kmbox_post(op, a0, a1, a2);
    if (slot < 0) return false;

    volatile uint32_t *state = &s_slots[slot].state;

    // Waiting is a yield, not a spin: the whole point of handing work to the
    // other core is that this one gets on with something else.
    //
    // Bounded in TIME. It used to be two million passes round this loop, and
    // how long that took depended on how many other tasks each yield handed
    // the core to - and, after the -O2 switch, on the compiler.
    const uint32_t start = k_millis();
    while (*state != KMB_DONE) {
        if (k_millis() - start >= timeout_ms) {
            // Giving up has to be negotiated, because the slot may be in core
            // 0's hands right now. The old code simply wrote EMPTY here, and a
            // new poster could then fill the slot while core 0 was still
            // running the old job - and core 0 would write the old job's
            // answer into the new one.
            if (k_cas(state, KMB_POSTED, KMB_EMPTY)) {
                // Never started: withdrawn cleanly, the slot is free again.
                s_withdrawn = s_withdrawn + 1;
                return false;
            }
            if (k_cas(state, KMB_RUNNING, KMB_ABANDONED)) {
                // Mid-job: core 0 still owns it, and will free it when done.
                s_abandoned = s_abandoned + 1;
                return false;
            }
            // Neither: it finished between the check and the swaps. Take it.
            break;
        }
        k_yield();
    }

    barrier();                   // read the results only after seeing DONE
    if (result) *result = s_slots[slot].result;
    bool ok = (s_slots[slot].status == 0);
    kmbox_release(slot);
    return ok;
}

K_IRAM bool kmbox_call(uint32_t op, uint32_t a0, uint32_t a1, uint32_t a2,
                          uint32_t *result)
{
    return kmbox_call_timeout(op, a0, a1, a2, result, KMBOX_TIMEOUT_MS);
}

// --- server side (core 0, FreeRTOS context) ---------------------------------

K_IRAM void kmbox_service(void)
{
    for (int i = 0; i < KMBOX_SLOTS; ++i) {
        if (s_slots[i].state != KMB_POSTED) continue;

        // Taking the job is a swap too: the poster may be withdrawing it at
        // this very moment, and exactly one of us may win.
        if (!k_cas(&s_slots[i].state, KMB_POSTED, KMB_RUNNING)) continue;

        k_job_t *j = &s_slots[i];

        switch (j->op) {
            case K_JOB_LOG:
                // This is the job core 1 cannot do for itself: a real printf,
                // with the locks that come with it, on the core that owns them.
                kport_printf("[core1 via core0] %s\n", (const char *)(uintptr_t)j->a0);
                j->result = 0;
                j->status = 0;
                break;

            case K_JOB_SUM: {
                const uint32_t *v = (const uint32_t *)(uintptr_t)j->a0;
                uint32_t sum = 0;
                for (uint32_t n = 0; n < j->a1; ++n) sum += v[n];
                j->result = sum;
                j->status = 0;
                break;
            }

            case K_JOB_SLEEP:
                kport_yield_ms(j->a0);
                j->result = j->a0;
                j->status = 0;
                break;

            default:
                if (j->op < KMBOX_OPS && s_handlers[j->op]) {
                    uint32_t status = 0;
                    j->result = s_handlers[j->op](j, &status);
                    j->status = status;
                } else {
                    j->result = 0;
                    j->status = 1;   // unknown op
                }
                break;
        }

        s_served++;

        // Handing back is a swap as well, because the poster may have given up
        // while we worked. If so, nobody is waiting for this answer and the
        // slot is ours to free.
        if (!k_cas(&j->state, KMB_RUNNING, KMB_DONE)) {
            barrier();
            j->state = KMB_EMPTY;
            barrier();
        }
    }
}

kmbox_forgot_t kmbox_forget(uint8_t owner, uint32_t owner_gen)
{
    kmbox_forgot_t f = {};
    for (int i = 0; i < KMBOX_SLOTS; ++i) {
        k_job_t *j = &s_slots[i];
        if (j->owner != owner || j->owner_gen != owner_gen) continue;
        switch (j->state) {
            case KMB_CLAIMED:
                // Its poster is dead and on no core, so nobody else can be
                // writing this slot.
                barrier();
                j->state = KMB_EMPTY;
                barrier();
                f.claimed++;
                break;
            case KMB_POSTED:
                // Contested with kmbox_service, like a timeout's withdrawal.
                // We ARE the service here, so this wins - but say so anyway.
                if (k_cas(&j->state, KMB_POSTED, KMB_EMPTY)) f.posted++;
                else f.running++;
                break;
            case KMB_DONE:
                barrier();
                j->state = KMB_EMPTY;
                barrier();
                f.done++;
                break;
            case KMB_RUNNING:
                f.running++;
                break;
            default:
                break;              // EMPTY, or ABANDONED: core 0 frees it
        }
    }
    return f;
}

bool kmbox_register(uint32_t op, kmbox_handler_t fn)
{
    if (op < KMBOX_OP_EXT || op >= KMBOX_OPS || s_handlers[op]) return false;
    s_handlers[op] = fn;
    return true;
}

K_IRAM uint32_t kmbox_served_count(void)    { return s_served; }
K_IRAM uint32_t kmbox_withdrawn_count(void) { return s_withdrawn; }
K_IRAM uint32_t kmbox_abandoned_count(void) { return s_abandoned; }

K_IRAM void kmbox_census(uint32_t *empty, uint32_t *posted, uint32_t *done,
                            uint32_t *busy)
{
    uint32_t e = 0, p = 0, d = 0, b = 0;
    for (int i = 0; i < KMBOX_SLOTS; ++i) {
        switch (s_slots[i].state) {
            case KMB_EMPTY:  e++; break;
            case KMB_POSTED: p++; break;
            case KMB_DONE:   d++; break;
            default:         b++; break;   // CLAIMED, RUNNING, ABANDONED
        }
    }
    if (empty)  *empty  = e;
    if (posted) *posted = p;
    if (done)   *done   = d;
    if (busy)   *busy   = b;
}
