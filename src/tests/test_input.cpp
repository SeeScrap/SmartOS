#include "tests/tests.h"

#include <stdio.h>
#include <string.h>

#include "gfx/fb.h"
#include "os/evbus.h"
#include "board/input.h"
#include "board/log.h"
#include "system.h"
#include "tests/harness.h"

#include "esp_heap_caps.h"

// ---------------------------------------------------------------------------
// Stage 3.6 - buttons as an event stream (moved out of main.cpp in 5.4b).
//
// The debouncer, fed a script. input_step() is one sample of the real sampler
// with the time passed in, so bounces, glitches, holds and a full queue can
// all be checked in a few microseconds, without anyone pressing anything -
// and checked the same way every boot.
// ---------------------------------------------------------------------------

#define EV_MAX     64
#define TEST_Q_LEN 32

// The debouncer publishes on the bus now, so the script reads it the way any
// app would: as a subscriber. A fresh one per case, so every case starts with
// an empty queue, seq 0 and no drops.
// Both of these used to be statics, and both are scratch this test needs for
// the second it runs. Together they were 1,920 bytes of internal RAM held for
// the whole life of the board - the scarcest memory on this chip, spent on a
// test that finishes before the launcher is even on screen. They are borrowed
// and given back now.
//
// NOT kmalloc, although every other test in this directory uses it. This file
// is the debouncer fed a script: pure logic, no hardware, nothing of the
// kernel in it - so main.cpp runs it BEFORE wake_core1(), which means before
// kheap_reserve() has taken the kernel's pools from ESP-IDF. kmalloc() here
// returns null, every case is skipped, and the event bus tests that follow
// then fail in a way that points at the bus rather than at this. Measured,
// on the board, by making exactly that mistake.
//
// So: core 0's own allocator, which is up from the first line of app_main.
// tests/test_park.cpp does use kmalloc, and is right to - it runs after the
// kernel, and its buffer has to survive the flash cache being switched off.
static ev_t *s_test_q  = nullptr;
static int   s_test_sub = -1;

#define TEST_CAPS (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)


static void reset_case(void)
{
    input_reset();
    if (s_test_sub >= 0) evbus_unsubscribe(s_test_sub);
    s_test_sub = evbus_subscribe("test", EV_MASK(EV_INPUT), s_test_q, TEST_Q_LEN);
}


// Holds `mask` for `ms` milliseconds of samples, advancing the clock.
static void feed(uint32_t mask, uint32_t ms, uint32_t *t)
{
    for (uint32_t e = 0; e < ms; e += INPUT_SAMPLE_MS) {
        input_step(mask, *t);
        *t += INPUT_SAMPLE_MS;
    }
}


static int drain(ev_t *out, int max)
{
    int n = 0;
    while (n < max && evbus_try_recv(s_test_sub, &out[n])) ++n;
    return n;
}


static int count_kind(const ev_t *ev, int n, uint8_t kind)
{
    int c = 0;
    for (int i = 0; i < n; ++i) if (ev[i].a == kind) ++c;
    return c;
}


static int count_button(const ev_t *ev, int n, uint8_t button)
{
    int c = 0;
    for (int i = 0; i < n; ++i) if (ev[i].b == button) ++c;
    return c;
}


static void input_case(const char *what, bool ok, const char *detail)
{
    logf("  %-30s %-38s %s\n", what, detail, ok ? "OK" : "FAIL");
}


void input_logic_test(void)
{
    logln();
    logln("--- stage 3.6a: the debouncer, fed a script ---");

    ev_t  *ev = (ev_t *)heap_caps_malloc(sizeof(ev_t) * EV_MAX, TEST_CAPS);
    s_test_q  = (ev_t *)heap_caps_malloc(sizeof(ev_t) * TEST_Q_LEN, TEST_CAPS);
    if (!ev || !s_test_q) {
        input_case("the debouncer has room to be tested", false,
                   "internal RAM could not spare 1,920 bytes of scratch");
        heap_caps_free(ev); heap_caps_free(s_test_q); s_test_q = nullptr;
        return;
    }

    char   d[64];
    uint32_t t;
    int n;

    // 1. A clean press: one PRESS, one RELEASE, accepted one debounce late.
    reset_case(); t = 1000;
    feed(0, 20, &t);
    uint32_t contact = t;
    feed(BTN_UP, 100, &t);
    feed(0, 100, &t);
    n = drain(ev, EV_MAX);
    snprintf(d, sizeof(d), "%d events, PRESS %lu ms after contact",
             n, n ? (unsigned long)(ev[0].t_ms - contact) : 0ul);
    input_case("clean press and release",
               n == 2 && ev[0].a == INPUT_PRESS && ev[1].a == INPUT_RELEASE &&
               ev[0].t_ms - contact == INPUT_DEBOUNCE_MS, d);

    // 2. Five edges in ten milliseconds, then held: still exactly one press.
    reset_case(); t = 1000;
    feed(0, 20, &t);
    feed(BTN_UP, 2, &t); feed(0, 2, &t); feed(BTN_UP, 2, &t); feed(0, 2, &t);
    feed(BTN_UP, 100, &t);
    feed(0, 100, &t);
    n = drain(ev, EV_MAX);
    snprintf(d, sizeof(d), "%d PRESS, %d RELEASE",
             count_kind(ev, n, INPUT_PRESS), count_kind(ev, n, INPUT_RELEASE));
    input_case("contact bounce, 5 edges/10 ms", n == 2 &&
               count_kind(ev, n, INPUT_PRESS) == 1, d);

    // 3. Closed for 8 ms - shorter than the debounce - is noise, not a press.
    reset_case(); t = 1000;
    feed(0, 20, &t);
    feed(BTN_UP, 8, &t);
    feed(0, 100, &t);
    n = drain(ev, EV_MAX);
    snprintf(d, sizeof(d), "%d events", n);
    input_case("8 ms glitch", n == 0, d);

    // 4. Held for a second. PRESS lands at +12; repeats at +412 and every 120
    //    after that while the contact is still closed: +412 +532 +652 +772
    //    +892. The next would be +1012, after the release began at +1000.
    reset_case(); t = 1000;
    feed(0, 20, &t);
    feed(BTN_UP, 1000, &t);
    feed(0, 100, &t);
    n = drain(ev, EV_MAX);
    int reps = count_kind(ev, n, INPUT_REPEAT);
    uint32_t gap = (reps >= 2) ? ev[2].t_ms - ev[1].t_ms : 0;
    snprintf(d, sizeof(d), "PRESS + %d REPEAT (every %lu ms) + RELEASE",
             reps, (unsigned long)gap);
    input_case("held for 1000 ms", n == 7 && reps == 5 &&
               ev[0].a == INPUT_PRESS && ev[6].a == INPUT_RELEASE &&
               gap == INPUT_REPEAT_EVERY_MS, d);

    // 5. Two buttons closing in the same sample: both reported, same time,
    //    in button order.
    reset_case(); t = 1000;
    feed(0, 20, &t);
    feed(BTN_UP | BTN_DOWN, 50, &t);
    feed(0, 50, &t);
    n = drain(ev, EV_MAX);
    snprintf(d, sizeof(d), "%s %s then %s %s, same ms: %s",
             n > 0 ? input_kind_name((uint8_t)ev[0].a) : "-", n > 0 ? input_name(ev[0].b) : "-",
             n > 1 ? input_kind_name((uint8_t)ev[1].a) : "-", n > 1 ? input_name(ev[1].b) : "-",
             (n > 1 && ev[0].t_ms == ev[1].t_ms) ? "yes" : "no");
    input_case("two buttons, one sample", n == 4 &&
               ev[0].a == INPUT_PRESS && ev[0].b == 0 &&
               ev[1].a == INPUT_PRESS && ev[1].b == 1 &&
               ev[0].t_ms == ev[1].t_ms, d);

    // 6. One button bouncing for 60 ms while another is pressed cleanly. The
    //    clean one must still land on time. The old driver ran a single settle
    //    timer for all six buttons, and would have held DOWN back until UP
    //    stopped bouncing - which, here, is never.
    reset_case(); t = 1000;
    feed(0, 20, &t);
    contact = t;
    for (int i = 0; i < 15; ++i) {
        feed(BTN_DOWN | BTN_UP, 2, &t);
        feed(BTN_DOWN,          2, &t);
    }
    n = drain(ev, EV_MAX);
    snprintf(d, sizeof(d), "DOWN at +%lu ms, %d UP events",
             n ? (unsigned long)(ev[0].t_ms - contact) : 0ul,
             count_button(ev, n, 0));
    input_case("one bouncing, one clean", n == 1 && ev[0].b == 1 &&
               ev[0].a == INPUT_PRESS && ev[0].t_ms - contact == INPUT_DEBOUNCE_MS, d);

    // 7. Forty events and nobody draining. The queue keeps the first 32, the
    //    driver counts 8 drops, and the next event after a drain carries a
    //    sequence number that shows exactly how many went missing.
    reset_case(); t = 1000;
    feed(0, 20, &t);
    for (int i = 0; i < 20; ++i) { feed(BTN_OK, 20, &t); feed(0, 20, &t); }
    n = drain(ev, EV_MAX);
    bool contiguous = true;
    for (int i = 1; i < n; ++i) if (ev[i].seq != (uint16_t)(ev[i - 1].seq + 1)) contiguous = false;
    uint16_t last = n ? ev[n - 1].seq : 0;
    feed(BTN_OK, 20, &t);
    ev_t next = {};
    bool got = evbus_try_recv(s_test_sub, &next);
    snprintf(d, sizeof(d), "%d kept, %lu dropped, next seq jumps %u",
             n, (unsigned long)evbus_stats(s_test_sub).dropped, got ? (unsigned)(next.seq - last) : 0u);
    input_case("40 events into 32 slots", n == TEST_Q_LEN && contiguous &&
               evbus_stats(s_test_sub).dropped == 8 && got && next.seq - last == 9, d);

    input_reset();
    evbus_unsubscribe(s_test_sub);
    s_test_sub = -1;

    // The subscription is gone, so nothing can write into the queue any more.
    // Freeing it before unsubscribing would leave the bus holding a pointer
    // into the heap, which is the kind of bug that waits months to be seen.
    heap_caps_free(s_test_q); s_test_q = nullptr;
    heap_caps_free(ev);
}
