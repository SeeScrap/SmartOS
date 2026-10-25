#include "tests/tests.h"

#include <stdio.h>
#include <string.h>

#include "esp_timer.h"

#include "apps/apps.h"
#include "board/devkitc_n16r8.h"
#include "gfx/fb.h"
#include "os/app.h"
#include "os/dev.h"
#include "os/evbus.h"
#include "kernel/kfmt.h"
#include "os/krand.h"
#include "board/hal_time.h"
#include "board/input.h"
#include "board/log.h"
#include "system.h"
#include "tests/harness.h"

// ---------------------------------------------------------------------------
// Stage 4.3c - the app SDK (moved out of main.cpp in 5.4b).
//
// The rules an app can be held to without running one - those run every boot -
// and then the same rules with apps actually on core 1, which needs
// -D SMARTOS_TEST_APPS.
// ---------------------------------------------------------------------------

// Stage 6.1: its own framebuffer, not the panel's. These checks count pixels
// in memory and never flush, so they do not need the panel - and since 6.1
// the panel is showing the boot screen by the time they run, which this used
// to clear to black. (That is what the boot test's "px of the mark at the
// end" caught: 0.)
static fb_t s_fb;


// Pure functions and pin rules, checked before core 1 starts.
void sdk_logic_test(void)
{
    logln();
    logln("--- stage 4.3c: the app SDK, fed a script ---");
    char d[160], b[96];
    int  r;

    if (!fb_init(&s_fb, LCD_WIDTH, LCD_HEIGHT)) {
        sdk_case("a framebuffer to check in", false, "no room for a 320x240 buffer");
        return;
    }

    // k_snprintf: what apps format text with on core 1. Written in 4.3b and
    // not checked until now.
    struct { const char *want; int got_len; } fmt[7];
    char o[7][24];
    fmt[0].got_len = k_snprintf(o[0], sizeof(o[0]), "%5d|%-5d|", -42, 7);      fmt[0].want = "  -42|7    |";
    fmt[1].got_len = k_snprintf(o[1], sizeof(o[1]), "%05d", -42);              fmt[1].want = "-0042";
    fmt[2].got_len = k_snprintf(o[2], sizeof(o[2]), "%x %X %u", 0xbeefu, 0xbeefu, 4000000000u); fmt[2].want = "beef BEEF 4000000000";
    fmt[3].got_len = k_snprintf(o[3], sizeof(o[3]), "%s:%c%%", "ok", 'y');     fmt[3].want = "ok:y%";
    fmt[4].got_len = k_snprintf(o[4], sizeof(o[4]), "%ld", (long)-2147483647 - 1); fmt[4].want = "-2147483648";
    fmt[5].got_len = k_snprintf(o[5], 5, "%s", "truncated");                  fmt[5].want = "trun";
    fmt[6].got_len = k_snprintf(o[6], sizeof(o[6]), "[%-6.3s]", "abcdef");    fmt[6].want = "[abc   ]";
    int fmt_ok = 0;
    for (int i = 0; i < 7; ++i) if (strcmp(o[i], fmt[i].want) == 0) fmt_ok++;
    snprintf(d, sizeof(d), "%d of 7 match; truncation reports %d, wants 9", fmt_ok, fmt[5].got_len);
    sdk_case("k_snprintf", fmt_ok == 7 && fmt[5].got_len == 9, d);
    if (fmt_ok != 7) for (int i = 0; i < 7; ++i) logf("    got \"%s\" want \"%s\"\n", o[i], fmt[i].want);

    // Lines and circles, counted on the real framebuffer before anything else
    // has been drawn on it.
    fb_clear(&s_fb, FB_BLACK);
    fb_damage_reset(&s_fb);
    fb_line(&s_fb, 10, 10, 19, 19, FB_WHITE);                 // a true diagonal
    const fb_rect_t *lr = fb_damage_at(&s_fb, 0);
    const int diag = count_colour(&s_fb, FB_WHITE);
    snprintf(d, sizeof(d), "%d px, %d region %dx%d", diag, fb_damage_count(&s_fb),
             lr ? lr->w : 0, lr ? lr->h : 0);
    sdk_case("fb_line diagonal", diag == 10 && fb_damage_count(&s_fb) == 1 &&
             lr && lr->w == 10 && lr->h == 10, d);

    fb_clear(&s_fb, FB_BLACK);
    fb_line(&s_fb, 5, 50, 300, 61, FB_WHITE);                  // shallow: one pixel per column
    const int shallow = count_colour(&s_fb, FB_WHITE);
    fb_clear(&s_fb, FB_BLACK);
    fb_line(&s_fb, -50, -50, 30, 30, FB_WHITE);                // mostly off the buffer
    const int clipped = count_colour(&s_fb, FB_WHITE);
    snprintf(d, sizeof(d), "shallow %d px (want 296), clipped %d (want 31)", shallow, clipped);
    sdk_case("fb_line slope and clip", shallow == 296 && clipped == 31, d);

    fb_clear(&s_fb, FB_BLACK);
    fb_fill_circle(&s_fb, 100, 100, 20, FB_WHITE);
    const int area = count_colour(&s_fb, FB_WHITE);            // pi r^2 = 1257
    // Symmetry: the same number of pixels either side of both axes.
    int left = 0, right = 0, up = 0, down = 0;
    for (int y = 80; y <= 120; ++y) for (int x = 80; x <= 120; ++x) {
        if (s_fb.px[(size_t)y * s_fb.w + x] != FB_WHITE) continue;
        if (x < 100) left++;
        if (x > 100) right++;
        if (y < 100) up++;
        if (y > 100) down++;
    }
    fb_clear(&s_fb, FB_BLACK);
    fb_circle(&s_fb, 100, 100, 20, FB_WHITE);
    const int ring = count_colour(&s_fb, FB_WHITE);           // about 2 pi r = 126
    snprintf(d, sizeof(d), "filled %d px (pi r^2 = 1257), outline %d, L/R %d/%d U/D %d/%d",
             area, ring, left, right, up, down);
    sdk_case("fb_circle / fb_fill_circle", area > 1180 && area < 1330 && ring > 100 &&
             ring < 140 && left == right && up == down, d);
    fb_clear(&s_fb, FB_BLACK);
    fb_damage_reset(&s_fb);

    // k_rand: the same seed gives the same sequence, and k_rand_below is
    // uniform - ten buckets over 20,000 draws, each within 10% of 2,000.
    k_srand(12345);
    const uint32_t a1 = k_rand(), a2 = k_rand();
    k_srand(12345);
    const bool repeat = (k_rand() == a1 && k_rand() == a2);
    int bucket[10] = {0};
    for (int i = 0; i < 20000; ++i) bucket[k_rand_below(10)]++;
    int lo = bucket[0], hi = bucket[0];
    for (int i = 1; i < 10; ++i) { if (bucket[i] < lo) lo = bucket[i]; if (bucket[i] > hi) hi = bucket[i]; }
    k_srand(0);                                                // back to a hardware seed
    snprintf(d, sizeof(d), "seed repeats: %s; buckets %d..%d of 2000", repeat ? "yes" : "no", lo, hi);
    sdk_case("k_rand", repeat && lo >= 1800 && hi <= 2200, d);

    // The pins apps may and may not have.
    const int sys_pins[] = { PIN_LCD_SCK, PIN_LCD_CS, PIN_BTN_UP, PIN_BTN_BACK, PIN_SD_CS,
                             26, 33, 19, 20, 0, 46, 49, -1 };
    const int app_pins[] = { 1, 2, 17, 18, 38, 42, 48 };
    int refused = 0, allowed = 0;
    for (int p : sys_pins) if (!dev_pin_free(p)) refused++;
    for (int p : app_pins) if (dev_pin_free(p)) allowed++;
    snprintf(d, sizeof(d), "%d of 13 system pins refused, %d of 7 free pins allowed",
             refused, allowed);
    sdk_case("pin rules", refused == 13 && allowed == 7, d);

    int adc = 0;
    const int adc_pins[] = { 1, 2, 17, 18 };
    for (int p : adc_pins) if (dev_adc_capable(p)) adc++;
    const bool no_adc = !dev_adc_capable(38) && !dev_adc_capable(PIN_BTN_UP);
    snprintf(d, sizeof(d), "%d of 4 ADC pins, GPIO38 and a button pin refused: %s",
             adc, no_adc ? "yes" : "no");
    sdk_case("ADC pins", adc == 4 && no_adc, d);

    // The registry is filled by static constructors before app_main runs.
    b[0] = 0;
    for (int i = 0; i < app_registry_count(); ++i) {
        strncat(b, app_registry_at(i)->name, sizeof(b) - strlen(b) - 2);
        if (i + 1 < app_registry_count()) strncat(b, " ", sizeof(b) - strlen(b) - 1);
    }
    r = app_registry_count();
    snprintf(d, sizeof(d), "%d: %s", r, b);
    sdk_case("registry", r == 11 &&
             strcmp(b, "home input counter bounce scope snake hello settings tasks log notes") == 0 &&
             app_find("snake") == &app_snake && !app_find("hog"), d);

    fb_free(&s_fb);
}


// The live half opens apps, so it belongs to the app build.
#ifdef SMARTOS_TEST_APPS

// The same SDK with the kernel running: a real ADC pin read by the scope app,
// the game playing itself into a wall, and a stream that ends with its app.
void sdk_live_test(void)
{
    logln();
    logln("--- stage 4.3c: SDK apps, live ---");
    char d[96];

    const int sc = app_start(&app_scope);
    const int sn = app_start(&app_snake);
    const app_ctx_t *S = app_get(sc), *N = app_get(sn);
    if (!S || !N) { logln("  !! scope or snake is not running"); return; }

    // 1. The stream: 100 readings a second, from the device service on core 0,
    //    reaching an app on core 1 as events.
    app_focus(sc);
    pump(300);
    const uint32_t s0 = app_scope_samples(S);
    pump(1000);
    const uint32_t per_s = app_scope_samples(S) - s0;
    snprintf(d, sizeof(d), "GPIO%d: %lu readings in 1 s, last %lu mV", app_scope_pin(S),
             (unsigned long)per_s, (unsigned long)app_scope_mv(S));
    sdk_case("ADC stream to an app", per_s >= 95 && per_s <= 105 && app_scope_mv(S) <= 3300, d);

    // 2. The same pin with its pull-up and then its pull-down on: with nothing
    //    wired to it, that is the only thing on the pin, so the reading must
    //    follow - the whole path from the pad to this screen, with no hardware.
    press(4, INPUT_PRESS);                                     // OK: pull up
    pump(400);
    const uint32_t up = app_scope_mv(S);
    press(4, INPUT_PRESS);                                     // OK: pull down
    pump(400);
    const uint32_t down = app_scope_mv(S);
    press(4, INPUT_PRESS);                                     // OK: back to none
    pump(100);
    snprintf(d, sizeof(d), "pull-up %lu mV, pull-down %lu mV", (unsigned long)up,
             (unsigned long)down);
    sdk_case("ADC follows the pad", up > 2500 && down < 300, d);

    // 3. The game: running right from the middle of the board, then turned up.
    //    It must crash into the top wall, and the head must stop on the top row.
    app_focus(sn);
    pump(400);
    const uint32_t m_turn = app_snake_moves(N);
    press(0, INPUT_PRESS);                                     // UP
    const int64_t t0 = esp_timer_get_time();
    while (!app_snake_over(N) && esp_timer_get_time() - t0 < 4000000) pump(50);
    snprintf(d, sizeof(d), "over: %s after %lu moves, head on row %d (top row 3)",
             app_snake_over(N) ? "yes" : "no", (unsigned long)app_snake_moves(N),
             app_snake_head_row(N));
    sdk_case("snake hits the wall", app_snake_over(N) && app_snake_head_row(N) == 3 &&
             app_snake_moves(N) >= m_turn + 12, d);
    press(4, INPUT_PRESS);                                     // OK: play again
    pump(200);
    snprintf(d, sizeof(d), "over: %s, moves %lu", app_snake_over(N) ? "yes" : "no",
             (unsigned long)app_snake_moves(N));
    sdk_case("snake restarts on OK", !app_snake_over(N), d);

    // 4. Stopping the scope app must stop its stream: the device service is
    //    told by the app runtime on the way out.
    app_stop(sc);
    pump(200);
    const uint32_t d0 = dev_samples_sent();
    pump(500);
    const uint32_t after = dev_samples_sent() - d0;
    const int sc2 = app_start(&app_scope);
    pump(300);
    const uint32_t d1 = dev_samples_sent();
    pump(500);
    const uint32_t restarted = dev_samples_sent() - d1;
    snprintf(d, sizeof(d), "%lu readings while stopped, %lu in 0.5 s after restart (id %d)",
             (unsigned long)after, (unsigned long)restarted, sc2);
    sdk_case("stream ends with its app", after == 0 && restarted >= 45, d);

}

#endif // SMARTOS_TEST_APPS
