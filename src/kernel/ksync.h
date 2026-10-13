#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kernel/ktask.h"

// ---------------------------------------------------------------------------
// Stage 2.7 - the kernel's synchronisation primitives.
//
// Until now the kernel could run tasks but they had no way to talk to each
// other except through shared variables and hope. The spinlock in kspin.h is
// not a substitute: it spins, which is right for the two or three instructions
// of a scheduler critical section and completely wrong for a task waiting on
// another task. Waiting has to give the core away.
//
// So these block. A task that cannot proceed goes to K_BLOCKED, the scheduler
// stops picking it, and whoever releases the resource puts it back on the
// ready queue.
//
// WAIT LISTS ARE BITMASKS
//
// K_MAX_TASKS is small and fixed, so a waiter set is one word and waking
// somebody is a bit test. No allocation, no list surgery, and it works from
// inside a critical section - which matters, because these are exactly the
// places a linked list would be inconvenient to walk.
//
// FAIRNESS: waking the lowest task id first is deliberate and simple, not
// fair. A task that spends its life waiting on a busy mutex behind a lower id
// can starve. Priorities (also in this stage) are the answer to that, not a
// cleverer wait list.
// ---------------------------------------------------------------------------

typedef uint32_t k_waitset_t;   // one bit per task

// The wait sets and the task table have to agree, and nothing but this would
// notice if they stopped: a task id past the width of the set would shift its
// bit off the end, and that task could never be woken.
static_assert(K_MAX_TASKS <= sizeof(k_waitset_t) * 8,
              "k_waitset_t has one bit per task - widen it with K_MAX_TASKS");

// --- mutex -----------------------------------------------------------------
// Owned. Only the task that locked it may unlock it, and locking it twice from
// the same task is reported rather than deadlocking silently.
typedef struct {
    k_tcb_t     *owner;
    k_waitset_t  waiters;
    const char  *name;
} k_mutex_t;

void k_mutex_init(k_mutex_t *m, const char *name);
void k_mutex_lock(k_mutex_t *m);
bool k_mutex_try_lock(k_mutex_t *m);
void k_mutex_unlock(k_mutex_t *m);

// --- counting semaphore ----------------------------------------------------
typedef struct {
    uint32_t     count;
    uint32_t     max;
    k_waitset_t  waiters;
    const char  *name;
} k_sem_t;

void k_sem_init(k_sem_t *s, uint32_t initial, uint32_t max, const char *name);
void k_sem_take(k_sem_t *s);        // blocks while the count is zero
bool k_sem_try_take(k_sem_t *s);
void k_sem_give(k_sem_t *s);

// --- message queue ---------------------------------------------------------
// Fixed capacity, fixed item size, copied in and out. The storage belongs to
// the caller so a queue can live wherever it needs to - including DRAM that
// stays reachable with the flash cache off.
typedef struct {
    uint8_t     *buf;
    size_t       item_size;
    size_t       capacity;
    size_t       head;          // next slot to read
    size_t       count;
    k_waitset_t  send_waiters;  // blocked because it was full
    k_waitset_t  recv_waiters;  // blocked because it was empty
    const char  *name;
} k_queue_t;

void   k_queue_init(k_queue_t *q, void *storage, size_t item_size,
                    size_t capacity, const char *name);
void   k_queue_send(k_queue_t *q, const void *item);   // blocks while full
bool   k_queue_try_send(k_queue_t *q, const void *item);
void   k_queue_recv(k_queue_t *q, void *out);          // blocks while empty
bool   k_queue_try_recv(k_queue_t *q, void *out);
size_t k_queue_count(const k_queue_t *q);
