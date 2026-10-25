#include "tests/tests.h"

#include <stdio.h>
#include <string.h>

#include "esp_timer.h"

#include "gfx/fb.h"
#include "gfx/text.h"
#include "s3hal/hal_time.h"
#include "s3hal/lcd_bus.h"
#include "s3hal/lcd_panel.h"
#include "s3hal/log.h"
#include "system.h"
#include "tests/harness.h"

// ---------------------------------------------------------------------------
// Stage 5.4b - the graphics tests, moved here out of main.cpp.
//
// Damage tracking (3.3), one transaction per rectangle (3.4) and the font
// (3.5). All three run on every boot and all three measure: the numbers they
// print are what this firmware claims about the panel.
//
// They draw on the whole panel, status bar included: they run before any app
// exists. The framebuffer belongs to main.cpp.
// ---------------------------------------------------------------------------

static fb_t &s_fb = *system_fb();

// ---------------------------------------------------------------------------
// Stage 3.3 - proving damage tracking.
//
// Three separate questions, because they can fail independently:
//
//   1. Does the merge logic produce the right regions? Pure arithmetic, no
//      panel involved. The invariant that matters is containment - every
//      rectangle ever marked must end up inside some region - because a region
//      list that fails it would leave stale pixels on the glass, and that is
//      the kind of bug that shows up as "sometimes the screen is wrong".
//   2. What does a flush of a given size actually cost?
//   3. What does that buy on a frame shaped like a real interface?
// ---------------------------------------------------------------------------
static bool rect_contains(const fb_rect_t *outer, const fb_rect_t *inner)
{
    return outer->x <= inner->x && outer->y <= inner->y &&
           outer->x + outer->w >= inner->x + inner->w &&
           outer->y + outer->h >= inner->y + inner->h;
}


static bool damage_covers(const fb_t *fb, const fb_rect_t *r)
{
    for (int i = 0; i < fb_damage_count(fb); ++i)
        if (rect_contains(fb_damage_at(fb, i), r)) return true;
    return false;
}


// Feeds a list of rectangles through the tracker and reports what came out,
// plus whether every one of them is still covered.
static void damage_case(const char *what, const fb_rect_t *in, int n, int expect,
                        bool at_most = false)
{
    fb_t probe = s_fb;              // shares the pixels; nothing here draws
    fb_damage_reset(&probe);

    for (int i = 0; i < n; ++i)
        fb_damage(&probe, in[i].x, in[i].y, in[i].w, in[i].h);

    bool covered = true;
    for (int i = 0; i < n; ++i)
        if (!damage_covers(&probe, &in[i])) covered = false;

    int    got = fb_damage_count(&probe);
    size_t px  = fb_damage_pixels(&probe);
    size_t raw = 0;
    for (int i = 0; i < n; ++i) raw += (size_t)in[i].w * in[i].h;

    logf("  %-22s %2d in -> %d regions (want %s%d)  %6u px touched, %6u sent  %s\n",
         what, n, got, at_most ? "<=" : "", expect, (unsigned)raw, (unsigned)px,
         ((at_most ? got <= expect : got == expect) && covered)
             ? "OK" : (covered ? "COUNT" : "LOST PIXELS"));
}


static void damage_logic_test(void)
{
    logln();
    logln("--- stage 3.3a: does the merge logic hold up? ---");

    const fb_rect_t overlap[] = { {10,10,60,40}, {40,20,60,40} };
    const fb_rect_t tiled[]   = { {10,10,60,40}, {10,50,60,40} };
    const fb_rect_t apart[]   = { {0,0,40,20},   {270,210,40,20} };
    const fb_rect_t outline[] = { {20,20,200,1}, {20,139,200,1},
                                  {20,20,1,120}, {219,20,1,120} };
    const fb_rect_t scatter[] = { {0,0,20,20},    {60,0,20,20},   {120,0,20,20},
                                  {180,0,20,20},  {240,0,20,20},  {300,0,20,20},
                                  {0,100,20,20},  {60,100,20,20}, {120,100,20,20},
                                  {180,100,20,20},{240,100,20,20},{300,100,20,20} };

    // Overlapping regions MUST become one, or the overlap goes out twice.
    damage_case("overlapping",         overlap, 2, 1);
    // Stacked exactly: the union wastes nothing, so merging is free.
    damage_case("edge to edge",        tiled,   2, 1);
    // Opposite corners: merging would send the whole screen to change 1600 px.
    damage_case("opposite corners",    apart,   2, 2);
    // A hollow box. The four thin sides must stay apart - their union is 24000
    // pixels to describe a change of 638.
    damage_case("box outline, 4 sides", outline, 4, 4);
    // More regions than slots. At most FB_DAMAGE_MAX may come out - fewer is
    // fine and expected, because coalesce() runs after a forced merge and the
    // grown region can then absorb another. The one thing that must not happen
    // is a rectangle falling out of coverage.
    damage_case("12 into 8 slots",     scatter, 12, FB_DAMAGE_MAX, true);
}


// What one flush costs, by size. The full-screen row is measured here rather
// than quoted from stage 3.2, so every row shares one bus mode and one run.
static void damage_cost_test(double *full_ms_out)
{
    logln();
    logln("--- stage 3.3b/3.4: what a flush costs, by area (was = stage 3.3, one transaction per row) ---");

    static const struct { const char *name; int x, y, w, h; double was_ms; } sizes[] = {
        { "cursor       40x20",  100, 100,  40,  20,  0.97 },
        { "button      100x40",  100, 100, 100,  40,  2.27 },
        { "status bar  320x24",    0,   0, 320,  24,  2.65 },
        { "half screen 320x120",   0,   0, 320, 120, 12.48 },
        { "whole frame 320x240",   0,   0, 320, 240, 25.25 },
    };
    const int N = (int)(sizeof(sizes) / sizeof(sizes[0]));

    for (int i = 0; i < N; ++i) {
        fb_damage_reset(&s_fb);
        fb_damage(&s_fb, sizes[i].x, sizes[i].y, sizes[i].w, sizes[i].h);

        int64_t t0 = hal_micros();
        fb_flush(&s_fb);
        double ms = (hal_micros() - t0) / 1000.0;

        size_t bytes = (size_t)sizes[i].w * sizes[i].h * 2;
        if (i == N - 1) *full_ms_out = ms;

        logf("  %-20s %8u bytes  %6.2f ms (was %5.2f, %4.1fx)  %5.1f MB/s  %7.1f FPS-eq\n",
             sizes[i].name, (unsigned)bytes, ms,
             sizes[i].was_ms, sizes[i].was_ms / ms,
             (bytes / 1e6) / (ms / 1000.0), 1000.0 / ms);
    }
}


// A frame that behaves like an interface does: a static background with one
// small thing moving on it.
static void damage_frame_test(double full_ms)
{
    const uint16_t bg  = fb_rgb(16, 20, 32);
    const uint16_t bar = fb_rgb(40, 48, 72);
    const uint16_t box = fb_rgb(255, 160, 0);

    // The static part, sent once.
    fb_clear(&s_fb, bg);
    fb_fill_rect(&s_fb, 0, 0, s_fb.w, 24, bar);
    fb_hline(&s_fb, 0, 24, s_fb.w, FB_CYAN);
    fb_hline(&s_fb, 0, s_fb.h - 40, s_fb.w, fb_rgb(60, 60, 80));
    fb_present(&s_fb);

    const int BW = 40, BH = 20, BY = 120;
    const int FRAMES = 150;

    int     x = 0, dx = 3;
    int64_t busy_us = 0;
    size_t  total_px = 0;
    int     total_rects = 0, max_rects = 0;

    for (int f = 0; f < FRAMES; ++f) {
        int nx = x + dx;
        if (nx < 0 || nx + BW > s_fb.w) { dx = -dx; nx = x + dx; }

        int64_t t0 = hal_micros();

        fb_fill_rect(&s_fb, x,  BY, BW, BH, bg);    // erase where it was
        fb_fill_rect(&s_fb, nx, BY, BW, BH, box);   // draw where it is now

        int n = fb_damage_count(&s_fb);
        total_rects += n;
        if (n > max_rects) max_rects = n;
        total_px += fb_damage_pixels(&s_fb);

        fb_flush(&s_fb);
        busy_us += hal_micros() - t0;

        x = nx;
        hal_delay_ms(8);            // outside the measurement: this is for eyes
    }

    double per_frame_ms = (busy_us / 1000.0) / FRAMES;
    size_t full_px      = (size_t)s_fb.w * s_fb.h * FRAMES;

    logln();
    logln("--- stage 3.3c: a moving box on a static background ---");
    logf("  frames                 : %d\n", FRAMES);
    logf("  regions per frame      : %.2f average, %d worst\n",
         (double)total_rects / FRAMES, max_rects);
    logf("  pixels sent            : %u  (full frames would be %u)\n",
         (unsigned)total_px, (unsigned)full_px);
    logf("  work per frame         : %6.2f ms -> %7.1f FPS\n",
         per_frame_ms, 1000.0 / per_frame_ms);
    logf("  same frames, full push : %6.2f ms -> %7.1f FPS\n",
         full_ms, 1000.0 / full_ms);
    logf("  speed-up               : %.1fx, sending %.2f%% of the pixels\n",
         full_ms / per_frame_ms, 100.0 * (double)total_px / (double)full_px);
    logln("  the erase and the draw overlap at a 3 px step, so they merge into");
    logln("  one region - which is why the average comes out at a single");
    logln("  transfer per frame rather than two.");
}


void gfx_damage_demo(void)
{
    if (!s_fb.px) return;

    double full_ms = 25.6;          // replaced by a measured value in 3.3b
    damage_logic_test();
    damage_cost_test(&full_ms);
    damage_frame_test(full_ms);
}


// ---------------------------------------------------------------------------
// Stage 3.4 - what changed when a rectangle became one transaction.
//
// Two questions the size table alone cannot answer:
//
//   a. Small rectangles now cost more in setting the address window than in
//      sending pixels. That is the new floor, and it is worth knowing where it
//      is before trying to beat it.
//   b. The second staging buffer is only worth its 8 KB if the copy really does
//      disappear under the transfer. Polling cannot overlap by construction, so
//      running the same frame both ways prices the overlap exactly.
// ---------------------------------------------------------------------------
static void chunk_fixed_cost(void)
{
    logln();
    logln("--- stage 3.4a: the fixed cost of a rectangle ---");

    const int N = 200;

    int64_t t0 = hal_micros();
    for (int i = 0; i < N; ++i) lcd_set_window(0, 0, 40, 20);
    double win_us = (double)(hal_micros() - t0) / N;

    logf("  address window (CASET+RASET+RAMWR) : %6.1f us, whatever the size\n",
         win_us);
    const unsigned row_bytes = 320u * lcd_bytes_per_pixel();
    const unsigned rows_per_chunk = LCD_MAX_TRANSFER / row_bytes;
    logf("  transactions per full frame        : 240 -> %u  (%u rows each)\n",
         (240u + rows_per_chunk - 1) / rows_per_chunk, rows_per_chunk);
    logf("  a %dx%d cursor is %u bytes = %4.0f us on the wire at 80 MHz\n",
         40, 20, 1600u, 1600.0 * 8.0 / 80.0);
    logln("  so a small flush is now mostly window setup: five polling");
    logln("  transactions to move eleven bytes of address. That is the floor");
    logln("  stage 3.4 leaves behind, and it is a command-path problem, not a");
    logln("  pixel-path one.");
}


static void chunk_overlap_cost(void)
{
    logln();
    logln("--- stage 3.4b: is the second staging buffer earning its keep? ---");

    lcd_bus_set_mode(LCD_XFER_POLLING);
    int64_t t0 = hal_micros();
    fb_present(&s_fb);
    double poll_ms = (hal_micros() - t0) / 1000.0;

    lcd_bus_set_mode(LCD_XFER_QUEUED);
    t0 = hal_micros();
    fb_present(&s_fb);
    double queue_ms = (hal_micros() - t0) / 1000.0;

    t0 = hal_micros();
    lcd_blit_wire_direct(0, 0, s_fb.w, s_fb.h, (const uint8_t *)s_fb.px,
                         (size_t)s_fb.w * 2);
    double direct_ms = (hal_micros() - t0) / 1000.0;

    const double wire_ms = 153600.0 * 8.0 / 80e6 * 1000.0;

    logf("  full frame, polling (no overlap possible) : %6.2f ms  %5.1f FPS\n",
         poll_ms, 1000.0 / poll_ms);
    logf("  full frame, queued  (copy under transfer) : %6.2f ms  %5.1f FPS\n",
         queue_ms, 1000.0 / queue_ms);
    logf("  full frame, DMA straight from PSRAM       : %6.2f ms  %5.1f FPS\n",
         direct_ms, 1000.0 / direct_ms);
    logf("  153600 bytes at 80 MHz                    : %6.2f ms  <- the floor\n",
         wire_ms);
    logf("  overlap is worth %5.2f ms; queued sits %5.2f ms above the wire\n",
         poll_ms - queue_ms, queue_ms - wire_ms);

    lcd_bus_set_mode(LCD_XFER_QUEUED);
}


void gfx_chunk_demo(void)
{
    if (!s_fb.px) return;
    chunk_fixed_cost();
    chunk_overlap_cost();
}


// ---------------------------------------------------------------------------
// Stage 3.5 - text.
//
//   a. The property the renderer was designed around: one call, one damage
//      region, whatever the length - and clipped at the screen edge.
//   b. What drawing text costs the CPU, before any of it reaches the panel.
//   c. A glyph sheet, left on screen, for judging the font by eye.
//   d. A counter rewritten in place, which is what text on this system will
//      mostly be doing.
// ---------------------------------------------------------------------------
static void text_damage_test(void)
{
    logln();
    logln("--- stage 3.5a: one call, one damage region ---");

    const char *line = "The quick brown fox jumps over the lazy dog";   // 43 chars

    fb_damage_reset(&s_fb);
    fb_text_bg(&s_fb, 10, 50, line, FB_WHITE, FB_BLACK, 1);
    const fb_rect_t *r = fb_damage_at(&s_fb, 0);
    bool one = fb_damage_count(&s_fb) == 1 && r &&
               r->x == 10 && r->y == 50 &&
               r->w == fb_text_width(line, 1) && r->h == fb_text_height(line, 1);
    logf("  43 chars, scale 1       : %d region(s), %dx%d at (%d,%d)   %s\n",
         fb_damage_count(&s_fb), r ? r->w : 0, r ? r->h : 0,
         r ? r->x : 0, r ? r->y : 0, one ? "OK" : "FAIL");

    // Runs 60 px off the right edge: the region must stop at the screen.
    fb_damage_reset(&s_fb);
    fb_text_bg(&s_fb, 260, 80, "clipped->", FB_WHITE, FB_BLACK, 2);
    r = fb_damage_at(&s_fb, 0);
    bool clipped = fb_damage_count(&s_fb) == 1 && r &&
                   r->x == 260 && r->x + r->w == s_fb.w;
    logf("  off the right edge      : region ends at x=%d (screen is %d)    %s\n",
         r ? r->x + r->w : -1, s_fb.w, clipped ? "OK" : "FAIL");

    fb_damage_reset(&s_fb);
}


static void text_cost_test(void)
{
    logln();
    logln("--- stage 3.5b: what drawing text costs, before the panel sees it ---");

    const char *line = "The quick brown fox jumps over the lazy dog";   // 43 chars
    const int   N = 200;

    struct { const char *name; bool opaque; int scale; } runs[] = {
        { "opaque,      scale 1", true,  1 },
        { "transparent, scale 1", false, 1 },
        { "opaque,      scale 2", true,  2 },
    };

    for (unsigned i = 0; i < sizeof(runs) / sizeof(runs[0]); ++i) {
        int64_t t0 = hal_micros();
        for (int n = 0; n < N; ++n) {
            if (runs[i].opaque) fb_text_bg(&s_fb, 0, 100, line, FB_WHITE, FB_BLACK, runs[i].scale);
            else                fb_text   (&s_fb, 0, 100, line, FB_WHITE,           runs[i].scale);
        }
        double us = (double)(hal_micros() - t0) / N;

        // Only the part on screen is drawn; at scale 2 that is 26 of the 43.
        int drawn = s_fb.w / (TEXT_CELL_W * runs[i].scale);
        if (drawn > 43) drawn = 43;

        logf("  %s : %7.1f us per line, %5.2f us per character\n",
             runs[i].name, us, us / drawn);
    }
    fb_damage_reset(&s_fb);
}


// Everything the font can draw, at every scale worth using.
static void text_sheet(void)
{
    const uint16_t bg  = fb_rgb(16, 20, 32);
    const uint16_t bar = fb_rgb(40, 48, 72);

    fb_clear(&s_fb, bg);

    fb_fill_rect(&s_fb, 0, 0, s_fb.w, 24, bar);
    fb_hline(&s_fb, 0, 24, s_fb.w, FB_CYAN);
    fb_text(&s_fb, 6, 2, "SmartOS  text 5x8", FB_WHITE, 2);

    // All 95 glyphs, in code order, split over two lines.
    char set[96];
    for (int c = 32; c <= 126; ++c) set[c - 32] = (char)c;
    set[95] = 0;
    char first[49];
    memcpy(first, set, 48);
    first[48] = 0;
    fb_text(&s_fb, 4, 30, first,    FB_YELLOW, 1);
    fb_text(&s_fb, 4, 40, set + 48, FB_YELLOW, 1);

    fb_text(&s_fb, 4, 56, "The quick brown fox jumps over the lazy dog.", FB_WHITE, 1);
    fb_text(&s_fb, 4, 66, "Sphinx of black quartz, judge my vow!",       FB_WHITE, 1);

    fb_text(&s_fb, 4, 80,  "0123456789 +-*/=",  FB_CYAN,  2);
    fb_text(&s_fb, 4, 102, "gjpqy descenders",  FB_GREEN, 2);

    fb_text(&s_fb, 4, 128, "SmartOS", FB_WHITE, 3);

    // Transparent text over stripes: the stripes must show between the letters.
    for (int i = 0; i < 16; ++i) {
        fb_fill_rect(&s_fb, 150 + i * 10, 128, 5, 30, fb_rgb(80, 40, 120));
    }
    fb_text(&s_fb, 158, 134, "see-through", FB_YELLOW, 2);

    // Both edges, to show clipping by eye as well as by the test above.
    fb_text(&s_fb, -9,  168, "<-clipped", FB_MAGENTA, 2);
    fb_text(&s_fb, 260, 168, "clipped->", FB_MAGENTA, 2);

    fb_fill_rect(&s_fb, 0, 214, s_fb.w, 26, bar);
    fb_hline(&s_fb, 0, 214, s_fb.w, FB_CYAN);

    fb_present(&s_fb);
}


static void text_counter_test(void)
{
    const uint16_t bar = fb_rgb(40, 48, 72);
    const int      N   = 200;

    char    buf[48];
    int64_t start = hal_micros();
    int64_t busy_us = 0;
    size_t  px = 0;

    for (int i = 1; i <= N; ++i) {
        double up = (double)(hal_micros() - start) / 1e6;

        int64_t t0 = hal_micros();
        snprintf(buf, sizeof(buf), "uptime %7.3f s   update %3d/%d", up, i, N);
        fb_text_bg(&s_fb, 6, 222, buf, FB_WHITE, bar, 1);
        px += fb_damage_pixels(&s_fb);
        fb_flush(&s_fb);
        busy_us += hal_micros() - t0;

        hal_delay_ms(20);           // outside the measurement: this is for eyes
    }

    double per = (double)busy_us / N;

    logln();
    logln("--- stage 3.5d: a counter rewritten in place ---");
    logf("  updates             : %d\n", N);
    logf("  region per update   : %u px (%u bytes)\n",
         (unsigned)(px / N), (unsigned)(px / N * 2));
    logf("  format+draw+flush   : %6.1f us per update\n", per);
    logln("  the line is opaque, so each update overwrites the last one");
    logln("  completely - no erase pass, and no ghost of the old digits.");
}


void gfx_text_demo(void)
{
    if (!s_fb.px) return;

    text_damage_test();
    text_cost_test();
    text_sheet();
    text_counter_test();
    logln("  glyph sheet left on the panel for inspection");
}
