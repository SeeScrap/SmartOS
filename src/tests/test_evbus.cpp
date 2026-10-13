#include "tests/tests.h"

#include <stdio.h>
#include <string.h>

#include "os/evbus.h"
#include "s3hal/log.h"
#include "tests/harness.h"

// ---------------------------------------------------------------------------
// Stage 4.2a - the event bus's rules, checked by script before core 1 starts
// (moved out of main.cpp in 5.4b).
// ---------------------------------------------------------------------------

static ev_t s_bq[3][8];


static int bus_drain(int sub, ev_t *out, int max)
{
    int n = 0;
    while (n < max && evbus_try_recv(sub, &out[n])) ++n;
    return n;
}


static void bus_case(const char *what, bool ok, const char *detail)
{
    logf("  %-28s %-44s %s\n", what, detail, ok ? "OK" : "FAIL");
}


void evbus_logic_test(void)
{
    logln();
    logln("--- stage 4.2a: the event bus, fed a script ---");

    ev_t a[16], b[16], c[16];
    char d[80];
    int  na, nb, nc;

    // 1. Fan-out by mask. A wants input, B input and app messages, C all.
    int A = evbus_subscribe("A", EV_MASK(EV_INPUT), s_bq[0], 8);
    int B = evbus_subscribe("B", EV_MASK(EV_INPUT) | EV_MASK(EV_APP), s_bq[1], 8);
    int C = evbus_subscribe("C", EV_MASK_ALL, s_bq[2], 8);
    int t_in  = evbus_publish(EV_INPUT, EV_SRC_TEST, 1, 0);
    int t_app = evbus_publish(EV_APP,   EV_SRC_TEST, 2, 0);
    int t_sys = evbus_publish(EV_SYS,   EV_SRC_TEST, 3, 0);
    na = bus_drain(A, a, 16); nb = bus_drain(B, b, 16); nc = bus_drain(C, c, 16);
    snprintf(d, sizeof(d), "A %d, B %d, C %d; taken by %d/%d/%d; one id: %s",
             na, nb, nc, t_in, t_app, t_sys,
             (na && nb && nc && a[0].id == b[0].id && b[0].id == c[0].id) ? "yes" : "no");
    bus_case("fan-out by type mask", na == 1 && nb == 2 && nc == 3 &&
             t_in == 3 && t_app == 2 && t_sys == 1 &&
             a[0].id == b[0].id && b[0].id == c[0].id, d);

    // 2. A targeted send reaches its subscriber whatever the mask says, and
    //    nobody else.
    bool sent = evbus_send(A, EV_APP, EV_SRC_TEST, 42, 0);
    na = bus_drain(A, a, 16); nb = bus_drain(B, b, 16); nc = bus_drain(C, c, 16);
    snprintf(d, sizeof(d), "A %d (a=%lu), B %d, C %d", na,
             na ? (unsigned long)a[0].a : 0ul, nb, nc);
    bus_case("targeted send", sent && na == 1 && a[0].a == 42 && nb == 0 && nc == 0, d);

    // 3. Isolation. A gets a 4-slot queue and never reads; ten events later it
    //    has dropped six, and B and C have not noticed a thing.
    evbus_unsubscribe(A);
    A = evbus_subscribe("A", EV_MASK(EV_INPUT), s_bq[0], 4);
    for (uint32_t i = 0; i < 10; ++i) evbus_publish(EV_INPUT, EV_SRC_TEST, i, 0);
    evbus_stats_t sa = evbus_stats(A), sb = evbus_stats(B), sc = evbus_stats(C);
    na = bus_drain(A, a, 16); nb = bus_drain(B, b, 16); nc = bus_drain(C, c, 16);
    evbus_publish(EV_INPUT, EV_SRC_TEST, 10, 0);
    ev_t a_next = {};
    bool a_got = evbus_try_recv(A, &a_next);
    bus_drain(B, b + nb, 1); bus_drain(C, c + nc, 1);
    snprintf(d, sizeof(d), "A kept %d dropped %lu, then seq jumps %u; B %d, C %d",
             na, (unsigned long)sa.dropped,
             a_got && na ? (unsigned)(a_next.seq - a[na - 1].seq) : 0u, nb, nc);
    bus_case("one full subscriber", na == 4 && sa.dropped == 6 && a_got &&
             a_next.seq - a[3].seq == 7 && nb == 8 && nc == 8 &&
             sb.dropped == 2 && sc.dropped == 2, d);
    // B and C have 8 slots, so they drop 2 of the 10 as well - by their own
    // capacity, not because of A. What matters is that A's full queue did not
    // take anything from them.

    // 4. Unsubscribed means gone: nothing more reaches B.
    evbus_unsubscribe(B);
    int taken = evbus_publish(EV_INPUT, EV_SRC_TEST, 99, 0);
    snprintf(d, sizeof(d), "publish taken by %d (A and C), B pending %d",
             taken, evbus_pending(B));
    bus_case("unsubscribe", taken == 2 && evbus_pending(B) == 0, d);
    bus_drain(A, a, 16); bus_drain(C, c, 16);

    // 5. Timers belong to their subscriber. Fill the table from C, check it
    //    is full, then unsubscribe C and check every slot came back.
    int got_t = 0;
    for (int i = 0; i < EVBUS_MAX_TIMERS; ++i) if (evbus_timer_start(C, 1000, i) >= 0) got_t++;
    int extra = evbus_timer_start(C, 1000, 99);
    evbus_unsubscribe(C);
    int freed = 0, T[EVBUS_MAX_TIMERS];
    for (int i = 0; i < EVBUS_MAX_TIMERS; ++i) if ((T[i] = evbus_timer_start(A, 1000, i)) >= 0) freed++;
    for (int i = 0; i < EVBUS_MAX_TIMERS; ++i) evbus_timer_stop(T[i]);
    snprintf(d, sizeof(d), "%d started, 9th %s, %d free after unsubscribe",
             got_t, extra < 0 ? "refused" : "ACCEPTED", freed);
    bus_case("timers go with subscriber", got_t == EVBUS_MAX_TIMERS && extra < 0 &&
             freed == EVBUS_MAX_TIMERS, d);

    // 6. The table itself is bounded, and says so rather than overwriting.
    //    Checked without assuming it starts empty: since stage 4.3b the app
    //    manager subscribes before this runs, and the first version of this
    //    case - which expected all EVBUS_MAX_SUBS slots free - failed on that.
    //    What matters is that filling it ends in a refusal, and that every
    //    slot it took comes back.
    int subs[EVBUS_MAX_SUBS + 1], ok_n = 0, ok_again = 0;
    evbus_unsubscribe(A);
    for (int i = 0; i <= EVBUS_MAX_SUBS; ++i) {
        subs[i] = evbus_subscribe("fill", EV_MASK_ALL, s_bq[0], 8);
        if (subs[i] >= 0) ok_n++;
    }
    const bool refused = subs[EVBUS_MAX_SUBS] < 0;
    for (int i = 0; i <= EVBUS_MAX_SUBS; ++i) if (subs[i] >= 0) evbus_unsubscribe(subs[i]);
    for (int i = 0; i <= EVBUS_MAX_SUBS; ++i) {
        subs[i] = evbus_subscribe("fill", EV_MASK_ALL, s_bq[0], 8);
        if (subs[i] >= 0) ok_again++;
    }
    for (int i = 0; i <= EVBUS_MAX_SUBS; ++i) if (subs[i] >= 0) evbus_unsubscribe(subs[i]);
    snprintf(d, sizeof(d), "%d free of %d, then refused; %d again after", ok_n,
             EVBUS_MAX_SUBS, ok_again);
    bus_case("subscriber table full", refused && ok_n >= 1 && ok_again == ok_n, d);
}
