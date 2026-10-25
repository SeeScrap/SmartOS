#include "kernel/ksync.h"

#include <string.h>

#include "port/kport.h"
#include "kernel/klog.h"

// Everything here runs on core 1 as often as core 0, and all of it is reachable
// from inside a critical section, so it lives in IRAM like the rest of the
// kernel. See the note at the top of ksched.cpp.
//
// One rule holds this file together: never call klog() while the scheduler lock
// is held. klog takes the console lock, and a core holding the console lock and
// wanting the scheduler lock would close the cycle. Where something needs
// reporting, the message is remembered and printed after the unlock.

// --- wait sets -------------------------------------------------------------
// Both helpers must be called with the scheduler lock already held.

static inline K_IRAM void wait_add(k_waitset_t *set, const k_tcb_t *t)
{
    *set |= (k_waitset_t)(1u << t->id);
}

// Wakes the lowest-numbered waiter and takes it out of the set. Returns false
// if there was nobody waiting.
static K_IRAM bool wake_one(k_waitset_t *set)
{
    if (*set == 0) return false;

    for (int id = 0; id < K_MAX_TASKS; ++id) {
        k_waitset_t bit = (k_waitset_t)(1u << id);
        if (!(*set & bit)) continue;

        *set &= (k_waitset_t)~bit;
        k_tcb_t *t = k_task_by_id((uint8_t)id);
        if (t && t->state == K_BLOCKED) t->state = K_READY;
        return true;
    }
    return false;
}

// Park the calling task on a wait set and give up the core. Called with the
// lock held; returns with it held again, having released and retaken it around
// the yield.
//
// The caller must re-test its condition afterwards rather than assume it now
// holds the resource: between being woken and running again, another task on
// the other core may have taken it. Every wait in this file is therefore a
// loop, not an if.
static K_IRAM void block_on(k_waitset_t *set, uint32_t *ps)
{
    k_tcb_t *me = k_current();
    if (!me) {
        // No task context - the only honest thing is to spin and re-check.
        k_unlock(*ps);
        *ps = k_lock();
        return;
    }

    wait_add(set, me);
    me->state      = K_BLOCKED;
    me->blocked_on = set;          // so k_task_kill can take it out again

    k_unlock(*ps);
    k_yield();
    *ps = k_lock();
    me->blocked_on = nullptr;
}

// --- mutex -----------------------------------------------------------------

K_IRAM void k_mutex_init(k_mutex_t *m, const char *name)
{
    m->owner   = nullptr;
    m->waiters = 0;
    m->name    = name;
}

K_IRAM bool k_mutex_try_lock(k_mutex_t *m)
{
    uint32_t ps = k_lock();
    bool got = (m->owner == nullptr);
    if (got) m->owner = k_current();
    k_unlock(ps);
    return got;
}

K_IRAM void k_mutex_lock(k_mutex_t *m)
{
    bool recursive = false;
    uint32_t ps = k_lock();

    for (;;) {
        if (m->owner == nullptr) {
            m->owner = k_current();
            break;
        }
        if (m->owner == k_current()) {
            // Not supported, and saying so beats deadlocking against ourselves.
            recursive = true;
            break;
        }
        block_on(&m->waiters, &ps);
    }

    k_unlock(ps);

    if (recursive) {
        klog("[ksync] mutex '%s' locked twice by the same task\n",
             m->name ? m->name : "?");
    }
}

K_IRAM void k_mutex_unlock(k_mutex_t *m)
{
    bool not_owner = false;

    uint32_t ps = k_lock();
    if (m->owner != k_current()) {
        not_owner = true;
    } else {
        m->owner = nullptr;
        wake_one(&m->waiters);
    }
    k_unlock(ps);

    if (not_owner) {
        klog("[ksync] mutex '%s' unlocked by a task that does not hold it\n",
             m->name ? m->name : "?");
    }
}

// --- counting semaphore ----------------------------------------------------

K_IRAM void k_sem_init(k_sem_t *s, uint32_t initial, uint32_t max,
                          const char *name)
{
    s->count   = initial;
    s->max     = max ? max : 1;
    s->waiters = 0;
    s->name    = name;
}

K_IRAM bool k_sem_try_take(k_sem_t *s)
{
    uint32_t ps = k_lock();
    bool got = (s->count > 0);
    if (got) s->count--;
    k_unlock(ps);
    return got;
}

K_IRAM void k_sem_take(k_sem_t *s)
{
    uint32_t ps = k_lock();
    while (s->count == 0) {
        block_on(&s->waiters, &ps);
    }
    s->count--;
    k_unlock(ps);
}

K_IRAM void k_sem_give(k_sem_t *s)
{
    bool overflow = false;

    uint32_t ps = k_lock();
    if (s->count < s->max) {
        s->count++;
        wake_one(&s->waiters);
    } else {
        overflow = true;
    }
    k_unlock(ps);

    if (overflow) {
        klog("[ksync] semaphore '%s' given past its maximum of %u\n",
             s->name ? s->name : "?", (unsigned)s->max);
    }
}

// --- message queue ---------------------------------------------------------

K_IRAM void k_queue_init(k_queue_t *q, void *storage, size_t item_size,
                            size_t capacity, const char *name)
{
    q->buf          = (uint8_t *)storage;
    q->item_size    = item_size;
    q->capacity     = capacity;
    q->head         = 0;
    q->count        = 0;
    q->send_waiters = 0;
    q->recv_waiters = 0;
    q->name         = name;
}

// Both of these assume the lock is held.
static inline K_IRAM void q_push(k_queue_t *q, const void *item)
{
    size_t tail = (q->head + q->count) % q->capacity;
    memcpy(q->buf + tail * q->item_size, item, q->item_size);
    q->count++;
}

static inline K_IRAM void q_pop(k_queue_t *q, void *out)
{
    memcpy(out, q->buf + q->head * q->item_size, q->item_size);
    q->head = (q->head + 1) % q->capacity;
    q->count--;
}

K_IRAM bool k_queue_try_send(k_queue_t *q, const void *item)
{
    uint32_t ps = k_lock();
    bool room = (q->count < q->capacity);
    if (room) {
        q_push(q, item);
        wake_one(&q->recv_waiters);
    }
    k_unlock(ps);
    return room;
}

K_IRAM void k_queue_send(k_queue_t *q, const void *item)
{
    uint32_t ps = k_lock();
    while (q->count == q->capacity) {
        block_on(&q->send_waiters, &ps);
    }
    q_push(q, item);
    wake_one(&q->recv_waiters);
    k_unlock(ps);
}

K_IRAM bool k_queue_try_recv(k_queue_t *q, void *out)
{
    uint32_t ps = k_lock();
    bool had = (q->count > 0);
    if (had) {
        q_pop(q, out);
        wake_one(&q->send_waiters);
    }
    k_unlock(ps);
    return had;
}

K_IRAM void k_queue_recv(k_queue_t *q, void *out)
{
    uint32_t ps = k_lock();
    while (q->count == 0) {
        block_on(&q->recv_waiters, &ps);
    }
    q_pop(q, out);
    wake_one(&q->send_waiters);
    k_unlock(ps);
}

K_IRAM size_t k_queue_count(const k_queue_t *q)
{
    return q->count;
}
