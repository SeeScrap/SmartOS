// Test only (stage 5.2): apps that have to be killed, each stuck somewhere
// different. None is registered - they must never appear in Home or start on
// their own. main.cpp's kill test starts them, kills them, and checks that
// nothing they held is left behind.
//
//   spin      init() never returns: always running or ready on core 1, never
//             reads an event, so it never sees a stop request either.
//   drawspin  draw() never returns: it has claimed the screen and never hands
//             it back.
//   sleeper   sleeps in a loop: almost always SLEEPING when the kill lands.
//   mboxer    reads a file in a loop, into a buffer on its own stack: almost
//             always has a mailbox call in flight - the case where core 0 could
//             write an answer into a stack that is no longer there.

#include "apps/apps.h"

#include <string.h>

#include "kernel/ktime.h"
#include "os/fs.h"
#include "os/kfmt.h"
#include "os/syslog.h"

static volatile uint32_t s_spins, s_draw_spins, s_sleeps, s_reads;

// --- spin -------------------------------------------------------------------------

static void spin_init(app_ctx_t *c)
{
    (void)c;
    for (;;) s_spins = s_spins + 1;
}

static void nothing_to_draw(app_ctx_t *c, fb_t *fb, bool full)
{
    (void)c; (void)fb; (void)full;
}

const app_t app_kt_spin = {
    .name = "spin",
    .init = spin_init,
    .draw = nothing_to_draw,
};

// --- drawspin -------------------------------------------------------------------------

static void drawspin_draw(app_ctx_t *c, fb_t *fb, bool full)
{
    (void)c; (void)full;
    fb_fill_rect(fb, 0, 0, 40, 40, FB_RED);      // half a frame, never finished
    for (;;) s_draw_spins = s_draw_spins + 1;
}

const app_t app_kt_drawspin = {
    .name = "drawspin",
    .draw = drawspin_draw,
};

// --- sleeper ------------------------------------------------------------------------

static void sleeper_init(app_ctx_t *c)
{
    (void)c;
    for (;;) {
        k_sleep_ms(5);
        s_sleeps = s_sleeps + 1;
    }
}

const app_t app_kt_sleeper = {
    .name = "sleeper",
    .init = sleeper_init,
    .draw = nothing_to_draw,
};

// --- mboxer -------------------------------------------------------------------------

static void mboxer_init(app_ctx_t *c)
{
    char buf[512];                                // on this task's stack, on purpose
    memset(buf, 'x', sizeof(buf));
    fs_write(c, "blob", buf, sizeof(buf));
    for (;;) {
        size_t got = 0;
        fs_read(c, "blob", buf, sizeof(buf), &got);   // core 0 writes into buf
        s_reads = s_reads + 1;
    }
}

const app_t app_kt_mboxer = {
    .name = "mboxer",
    .init = mboxer_init,
    .draw = nothing_to_draw,
};

uint32_t app_kt_spins(void)      { return s_spins; }
uint32_t app_kt_draw_spins(void) { return s_draw_spins; }
uint32_t app_kt_sleeps(void)     { return s_sleeps; }
uint32_t app_kt_reads(void)      { return s_reads; }

// --- bigbuf (stage 5.3c) -------------------------------------------------------------
// Takes 1 MB of PSRAM through app_alloc, writes a pattern through it and reads it
// back, then asks for 3.5 MB more - which cannot fit in what is left of the 4 MB
// pool and must be refused, not crash. Then it waits, holding the megabyte, to
// be closed or killed: either way the megabyte must come back.

static volatile int s_big_ok = -1, s_big_refused = -1;

static void bigbuf_init(app_ctx_t *c)
{
    uint8_t *p = (uint8_t *)app_alloc(c, 1024 * 1024);
    int ok = (p != nullptr);
    if (p) {
        for (uint32_t i = 0; i < 1024 * 1024; ++i) p[i] = (uint8_t)(i * 7 + (i >> 11));
        for (uint32_t i = 0; i < 1024 * 1024 && ok; ++i) ok = (p[i] == (uint8_t)(i * 7 + (i >> 11)));
    }
    s_big_ok = ok;
    s_big_refused = (app_alloc(c, 3584 * 1024) == nullptr);
}

const app_t app_kt_bigbuf = {
    .name      = "bigbuf",
    .init      = bigbuf_init,
    .draw      = nothing_to_draw,
    .psram_max = 2 * 1024 * 1024,      // it takes 1 MB; the rest is its headroom
};

// Declares 3 MB. With bigbuf open, 2 MB are promised to that one and only
// about 3 MB are free, so this one must be refused - not started and then
// starved (stage 5.5).
const app_t app_kt_bigger = {
    .name      = "bigger",
    .draw      = nothing_to_draw,
    .psram_max = 3 * 1024 * 1024,
};

int app_kt_big_ok(void)      { return s_big_ok; }
int app_kt_big_refused(void) { return s_big_refused; }

// --- tiny, greedy and hungry (stage 5.4d) --------------------------------------------
// The limits an app can run into that nothing used to check.
//
//   tiny     asks for a 1 KB stack - less than the 2 KB every app used to get -
//            and then uses a little of it, so the task manager's high-water
//            mark is a real figure against a real size.
//   timers   asks for more timers than the whole system has (EVBUS_MAX_TIMERS),
//            to prove the refusal is clean: it keeps running, and what it did
//            get still ticks.
//   hungry   declares a state block far bigger than the kernel pool, so it is
//            refused before anything is created.

static volatile int      s_tiny_depth;
static volatile int      s_timers_got, s_timers_refused;
static volatile uint32_t s_timer_ticks;

// A little real stack use: three frames of 64 bytes each.
static int tiny_work(int depth)
{
    volatile uint8_t pad[64];
    for (int i = 0; i < 64; ++i) pad[i] = (uint8_t)(depth + i);
    int sum = pad[depth & 63];
    if (depth > s_tiny_depth) s_tiny_depth = depth;      // the deepest it went
    if (depth < 3) sum += tiny_work(depth + 1);
    return sum;
}

static void tiny_init(app_ctx_t *c)
{
    (void)c;
    tiny_work(0);
}

const app_t app_kt_tiny = {
    .name  = "tiny",
    .init  = tiny_init,
    .draw  = nothing_to_draw,
    .stack = 1024,
};

static void timers_init(app_ctx_t *c)
{
    int got = 0, refused = 0;
    for (int i = 0; i < EVBUS_MAX_TIMERS + 4; ++i) {
        if (app_timer(c, 50, (uint32_t)i) >= 0) got++;
        else                                    refused++;
    }
    s_timers_got     = got;
    s_timers_refused = refused;
}

static void timers_event(app_ctx_t *c, const ev_t *ev)
{
    (void)c;
    if (ev->type == EV_TIMER) s_timer_ticks = s_timer_ticks + ev->b;
}

const app_t app_kt_timers = {
    .name     = "timers",
    .init     = timers_init,
    .on_event = timers_event,
    .draw     = nothing_to_draw,
};

// 256 KB of state, against a 128 KB pool.
const app_t app_kt_hungry = {
    .name      = "hungry",
    .user_size = 256 * 1024,
    .draw      = nothing_to_draw,
};

int      app_kt_tiny_depth(void)     { return s_tiny_depth; }
int      app_kt_timers_got(void)     { return s_timers_got; }
int      app_kt_timers_refused(void) { return s_timers_refused; }
uint32_t app_kt_timer_ticks(void)    { return s_timer_ticks; }

// --- filler (stage 5.4d, -D SMARTOS_TEST_FSFULL) ------------------------------------------
// Writes until LittleFS says no, to see what a full filesystem does to a board
// that has to keep running: whether the error comes back or something crashes,
// what happens to the system log while there is no room for it, and whether
// deleting really gives the space back. It writes about 12 MB and takes
// minutes, so it is a build of its own - not part of the app tests.
#ifdef SMARTOS_TEST_FSFULL
#define FILL_MAX 400                       // 32 KB each: more than the 12 MB there is
static volatile int      s_fill_rc = 99, s_fill_small_rc = 99, s_fill_files;
static volatile uint32_t s_fill_kb, s_fill_used_after, s_fill_total, s_fill_ms;
static volatile bool     s_fill_done;      // set last: everything below is done

static void filler_init(app_ctx_t *c)
{
    uint8_t *block = (uint8_t *)app_alloc(c, 8192);        // PSRAM: not the 2 KB stack
    if (!block) { s_fill_rc = -99; return; }
    memset(block, 0xA5, 8192);

    const uint32_t t0 = k_millis();
    char name[FS_NAME_MAX];
    int  rc = FS_OK, files = 0;
    uint32_t kb = 0;

    // Anything left by an earlier run goes first - these are appends, and a
    // run that ended halfway leaves files this one would only make longer.
    // (The first run on hardware left 2.8 MB behind exactly that way.)
    for (int i = 0; i < FILL_MAX; ++i) {
        k_snprintf(name, sizeof(name), "f%d", i);
        fs_remove(c, name);
    }
    fs_remove(c, "small");

    while (rc == FS_OK && files < FILL_MAX) {
        k_snprintf(name, sizeof(name), "f%d", files);
        for (int i = 0; i < 4 && rc == FS_OK; ++i) {       // 32 KB a file
            rc = fs_append(c, name, block, 8192);
            if (rc == FS_OK) kb += 8;
        }
        if (rc == FS_OK) files++;
    }
    s_fill_rc    = rc;
    s_fill_kb    = kb;
    s_fill_files = files;

    // With no room left: one more write of a single byte, and a line to the
    // system log - which cannot reach flash either, and must say so rather
    // than take the board down.
    s_fill_small_rc = fs_write(c, "small", "x", 1);
    app_log(c, "filesystem full after %lu KB in %d files", (unsigned long)kb, files);

    for (int i = 0; i < FILL_MAX; ++i) {                   // and give it all back
        k_snprintf(name, sizeof(name), "f%d", i);
        fs_remove(c, name);
    }
    fs_remove(c, "small");
    uint32_t total = 0, used = 0;
    fs_usage(c, &total, &used);
    s_fill_total      = total;
    s_fill_used_after = used;
    s_fill_ms         = k_millis() - t0;
    s_fill_done       = true;      // the script waits for this, not for s_fill_rc
}

const app_t app_kt_filler = {
    .name      = "filler",
    .init      = filler_init,
    .draw      = nothing_to_draw,
    .psram_max = 64 * 1024,
};

int      app_kt_fill_rc(void)       { return s_fill_rc; }
int      app_kt_fill_small_rc(void) { return s_fill_small_rc; }
int      app_kt_fill_files(void)    { return s_fill_files; }
uint32_t app_kt_fill_kb(void)       { return s_fill_kb; }
uint32_t app_kt_fill_used(void)     { return s_fill_used_after; }
uint32_t app_kt_fill_total(void)    { return s_fill_total; }
uint32_t app_kt_fill_ms(void)       { return s_fill_ms; }
bool     app_kt_fill_done(void)     { return s_fill_done; }
#endif // SMARTOS_TEST_FSFULL
