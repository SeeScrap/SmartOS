#include "os/evbus.h"

#include <string.h>

#include "esp_attr.h"
#include "kernel/kspin.h"
#include "kernel/ksync.h"
#include "kernel/ktask.h"
#include "kernel/ktime.h"

typedef struct {
    bool          active;
    const char   *name;
    uint32_t      mask;
    k_queue_t     q;
    uint16_t      next_seq;
    evbus_stats_t st;
} sub_t;

typedef struct {
    bool     active;
    int8_t   sub;
    uint32_t period;
    uint32_t next;       // k_millis() when it is next due
    uint32_t tag;
} tmr_t;

static DRAM_ATTR sub_t    s_subs[EVBUS_MAX_SUBS];
static DRAM_ATTR tmr_t    s_tmrs[EVBUS_MAX_TIMERS];
static DRAM_ATTR uint16_t s_next_id;
static DRAM_ATTR k_spin_t s_bus_lock;

// Same shape as k_lock(): interrupts masked first, so that the tick cannot
// preempt a holder and leave another task on this core spinning on a lock
// its own core is sitting on.
static inline IRAM_ATTR uint32_t bus_lock(void)
{
    uint32_t ps;
    __asm__ __volatile__("rsil %0, 3" : "=a"(ps) :: "memory");
    k_spin_lock_hard(&s_bus_lock);
    return ps;
}

static inline IRAM_ATTR void bus_unlock(uint32_t ps)
{
    k_spin_unlock(&s_bus_lock);
    __asm__ __volatile__("wsr %0, ps; rsync" :: "a"(ps) : "memory");
}

static inline IRAM_ATTR bool valid(int sub)
{
    return sub >= 0 && sub < EVBUS_MAX_SUBS;
}

// Bus lock held. Stamps this subscriber's own sequence number whether or not
// the event fits, so a dropped event still leaves its gap.
static IRAM_ATTR bool deliver(sub_t *s, ev_t *ev)
{
    ev->seq = s->next_seq++;
    if (k_queue_try_send(&s->q, ev)) {
        s->st.delivered++;
        return true;
    }
    s->st.dropped++;
    return false;
}

void evbus_init(void)
{
    memset((void *)s_subs, 0, sizeof(s_subs));
    memset((void *)s_tmrs, 0, sizeof(s_tmrs));
    s_next_id  = 0;
    s_bus_lock = 0;
}

IRAM_ATTR int evbus_subscribe(const char *name, uint32_t mask, ev_t *storage,
                              uint32_t capacity)
{
    if (!storage || capacity == 0) return -1;

    uint32_t ps = bus_lock();
    int id = -1;
    for (int i = 0; i < EVBUS_MAX_SUBS; ++i) {
        if (s_subs[i].active) continue;
        sub_t *s = &s_subs[i];
        k_queue_init(&s->q, storage, sizeof(ev_t), capacity, name);
        s->name     = name;
        s->mask     = mask;
        s->next_seq = 0;
        s->st.delivered = 0;
        s->st.dropped   = 0;
        s->active   = true;     // last: nothing is delivered until it is whole
        id = i;
        break;
    }
    bus_unlock(ps);
    return id;
}

IRAM_ATTR int evbus_subs_free(void)
{
    uint32_t ps = bus_lock();
    int n = 0;
    for (int i = 0; i < EVBUS_MAX_SUBS; ++i) if (!s_subs[i].active) n++;
    bus_unlock(ps);
    return n;
}

IRAM_ATTR void evbus_unsubscribe(int sub)
{
    if (!valid(sub)) return;
    uint32_t ps = bus_lock();
    s_subs[sub].active = false;
    // Its timers go with it, or they would fire into a slot the next
    // subscriber is about to be given.
    for (int t = 0; t < EVBUS_MAX_TIMERS; ++t) {
        if (s_tmrs[t].active && s_tmrs[t].sub == sub) s_tmrs[t].active = false;
    }
    bus_unlock(ps);
}

IRAM_ATTR int evbus_publish_at(uint8_t type, uint8_t src, uint32_t a, uint32_t b,
                               uint32_t t_ms)
{
    ev_t ev;
    ev.t_ms = t_ms;
    ev.a    = a;
    ev.b    = b;
    ev.type = type;
    ev.src  = src;
    ev._pad = 0;

    const uint32_t bit = EV_MASK(type);
    int taken = 0;

    uint32_t ps = bus_lock();
    ev.id = s_next_id++;
    for (int i = 0; i < EVBUS_MAX_SUBS; ++i) {
        sub_t *s = &s_subs[i];
        if (!s->active || !(s->mask & bit)) continue;
        if (deliver(s, &ev)) taken++;
    }
    bus_unlock(ps);
    return taken;
}

IRAM_ATTR int evbus_publish(uint8_t type, uint8_t src, uint32_t a, uint32_t b)
{
    return evbus_publish_at(type, src, a, b, k_millis());
}

IRAM_ATTR bool evbus_send(int sub, uint8_t type, uint8_t src, uint32_t a, uint32_t b)
{
    return evbus_send_at(sub, type, src, a, b, k_millis());
}

IRAM_ATTR bool evbus_send_at(int sub, uint8_t type, uint8_t src, uint32_t a, uint32_t b,
                             uint32_t t_ms)
{
    if (!valid(sub)) return false;

    ev_t ev;
    ev.t_ms = t_ms;
    ev.a    = a;
    ev.b    = b;
    ev.type = type;
    ev.src  = src;
    ev._pad = 0;

    bool ok = false;
    uint32_t ps = bus_lock();
    ev.id = s_next_id++;
    if (s_subs[sub].active) ok = deliver(&s_subs[sub], &ev);
    bus_unlock(ps);
    return ok;
}

IRAM_ATTR bool evbus_forward(int sub, const ev_t *ev)
{
    if (!valid(sub) || !ev) return false;
    ev_t copy = *ev;
    bool ok = false;
    uint32_t ps = bus_lock();
    if (s_subs[sub].active) ok = deliver(&s_subs[sub], &copy);
    bus_unlock(ps);
    return ok;
}

IRAM_ATTR void evbus_recv(int sub, ev_t *out)
{
    if (!valid(sub)) return;
    k_queue_recv(&s_subs[sub].q, out);
}

IRAM_ATTR bool evbus_try_recv(int sub, ev_t *out)
{
    if (!valid(sub) || !s_subs[sub].active) return false;
    return k_queue_try_recv(&s_subs[sub].q, out);
}

IRAM_ATTR evbus_stats_t evbus_stats(int sub)
{
    evbus_stats_t st = { 0, 0 };
    if (!valid(sub)) return st;
    uint32_t ps = bus_lock();
    st = s_subs[sub].st;
    bus_unlock(ps);
    return st;
}

IRAM_ATTR int evbus_pending(int sub)
{
    if (!valid(sub)) return 0;
    return (int)k_queue_count(&s_subs[sub].q);
}

// --- timers -------------------------------------------------------------------

IRAM_ATTR int evbus_timer_start(int sub, uint32_t period_ms, uint32_t tag)
{
    if (!valid(sub) || period_ms == 0) return -1;

    uint32_t ps = bus_lock();
    int id = -1;
    if (s_subs[sub].active) {
        for (int t = 0; t < EVBUS_MAX_TIMERS; ++t) {
            if (s_tmrs[t].active) continue;
            s_tmrs[t].sub    = (int8_t)sub;
            s_tmrs[t].period = period_ms;
            s_tmrs[t].next   = k_millis() + period_ms;
            s_tmrs[t].tag    = tag;
            s_tmrs[t].active = true;
            id = t;
            break;
        }
    }
    bus_unlock(ps);
    return id;
}

IRAM_ATTR void evbus_timer_stop(int timer)
{
    if (timer < 0 || timer >= EVBUS_MAX_TIMERS) return;
    uint32_t ps = bus_lock();
    s_tmrs[timer].active = false;
    bus_unlock(ps);
}

IRAM_ATTR void evbus_timer_task(void *arg)
{
    (void)arg;

    for (;;) {
        const uint32_t now   = k_millis();
        uint32_t       sleep = EVBUS_TIMER_MAX_SLEEP_MS;

        uint32_t ps = bus_lock();
        for (int t = 0; t < EVBUS_MAX_TIMERS; ++t) {
            tmr_t *tm = &s_tmrs[t];
            if (!tm->active) continue;

            const int32_t late = (int32_t)(now - tm->next);
            if (late >= 0) {
                // One event for however many periods have gone by. A burst of
                // catch-up ticks would tell a subscriber nothing it cannot
                // read from b, and would crowd everything else out of its queue.
                const uint32_t periods = 1 + (uint32_t)late / tm->period;
                tm->next += periods * tm->period;

                ev_t ev;
                ev.t_ms = now;
                ev.a    = tm->tag;
                ev.b    = periods;
                ev.type = EV_TIMER;
                ev.src  = EV_SRC_TIMER;
                ev._pad = 0;
                ev.id   = s_next_id++;
                deliver(&s_subs[tm->sub], &ev);
            }

            const uint32_t until = tm->next - now;
            if (until < sleep) sleep = until;
        }
        bus_unlock(ps);

        k_sleep_ms(sleep ? sleep : 1);
    }
}
