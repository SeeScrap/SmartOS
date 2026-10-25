#include "tests/tests.h"

#ifdef SMARTOS_TEST_APPS

#include <stdio.h>
#include <string.h>

#include "esp_timer.h"

#include "apps/apps.h"
#include "gfx/image.h"
#include "gfx/wallpaper.h"
#include "os/app.h"
#include "os/fs.h"
#include "os/settings.h"
#include "ui/keyboard.h"
#include <unistd.h>
#include "port/kport.h"
#include "board/input.h"
#include "board/log.h"
#include "system.h"
#include "tests/harness.h"

// ---------------------------------------------------------------------------
// Stage 6.2 - Home's background, checked against the panel.
//
// The wallpaper is 320x216 in flash and the app area is 320x216, so every
// pixel of the picture has one place it can be on the screen. That makes the
// check exact rather than approximate: count the pixels that are the picture,
// where the picture should be.
//
// Home draws rows, text and a help line over it, so a perfect match is not
// expected - what is expected is most of it, and then almost none of it when
// the setting is turned off.
// ---------------------------------------------------------------------------

static int wallpaper_px(void)
{
    const fb_t *fb = system_fb();
    if (!fb->px || !image_ok(&g_wallpaper)) return -1;
    int n = 0;
    for (int y = 0; y < g_wallpaper.h; ++y) {
        const uint16_t *row = fb->px + (size_t)(y + APP_AREA_Y) * fb->w;
        const uint16_t *src = g_wallpaper.px + (size_t)y * g_wallpaper.w;
        for (int x = 0; x < g_wallpaper.w; ++x) if (row[x] == src[x]) n++;
    }
    return n;
}

static int flat_px(void)
{
    const fb_t *fb = system_fb();
    const uint16_t bg = app_bg();
    int n = 0;
    for (int y = APP_AREA_Y; y < fb->h; ++y) {
        const uint16_t *row = fb->px + (size_t)y * fb->w;
        for (int x = 0; x < fb->w; ++x) if (row[x] == bg) n++;
    }
    return n;
}

void sys62_wallpaper_test(void)
{
    logln();
    logln("--- stage 6.2: the wallpaper ---");
    char d[140];
    const int all = (int)g_wallpaper.w * g_wallpaper.h;

    app_focus(app_home_id());
    settings_set("wallpaper", 1);
    pump(400);
    const int on = wallpaper_px();
    snprintf(d, sizeof(d), "%dx%d in flash (%d B); %d of %d px on the panel are the picture",
             g_wallpaper.w, g_wallpaper.h, all * 2, on, all);
    sdk_case("Home shows the picture", g_wallpaper.w == LCD_WIDTH && g_wallpaper.h == APP_AREA_H &&
             on > all * 6 / 10, d);

    // Off: the same screen, drawn on the flat colour, with no pixel of the
    // picture left anywhere.
    settings_set("wallpaper", 0);
    pump(500);
    const int off = wallpaper_px();
    const int flat = flat_px();
    snprintf(d, sizeof(d), "picture pixels %d -> %d; flat background %d of %d px",
             on, off, flat, all);
    sdk_case("the setting turns it off", off < all / 20 && flat > all * 6 / 10, d);

    settings_set("wallpaper", 1);
    pump(500);
    const int back = wallpaper_px();
    snprintf(d, sizeof(d), "back on: %d px of the picture (was %d)", back, on);
    sdk_case("and on again", back > all * 6 / 10, d);

    // What it costs. The picture is read from flash through the cache, a row
    // at a time; the blend is what a selected row over it costs.
    {
        fb_t scratch;
        if (fb_init(&scratch, LCD_WIDTH, APP_AREA_H)) {
            const int64_t t0 = esp_timer_get_time();
            for (int i = 0; i < 20; ++i) image_blit(&scratch, &g_wallpaper, 0, 0);
            const int64_t t1 = esp_timer_get_time();
            for (int i = 0; i < 20; ++i) fb_blend_rect(&scratch, 4, 0, 312, 22, FB_BLUE, 70);
            const int64_t t2 = esp_timer_get_time();
            const int blit_us = (int)((t1 - t0) / 20), blend_us = (int)((t2 - t1) / 20);
            // bytes / microseconds is megabytes a second, near enough: this is
            // the flash cache's read speed, which is what sets it.
            snprintf(d, sizeof(d), "whole picture %d us (%d MB/s from flash), one row blended %d us",
                     blit_us, blit_us ? (int)((int64_t)all * 2 / blit_us) : 0, blend_us);
            // A full Home redraw draws the picture once; a flush of the same
            // area is 15.4 ms, so anything in this range is free beside it.
            sdk_case("what the background costs", blit_us > 0 && blit_us < 15000 &&
                     blend_us < 5000, d);
            fb_free(&scratch);
        } else {
            sdk_case("what the background costs", false, "no room for a scratch buffer");
        }
    }
}

// ---------------------------------------------------------------------------
// Stage 6.4a - typing on the board, with the six buttons and nothing else.
//
// The whole way through: a key chosen on the grid, a line built out of them,
// Done, and the line read back off the flash by a different piece of code. The
// note file is removed first, so what is found afterwards cannot be a leftover
// from an earlier run.
// ---------------------------------------------------------------------------
void sys64_keyboard_test(void)
{
    logln();
    logln("--- stage 6.4a: the keyboard ---");
    char d[140];

    unlink("/fs/apps/notes/note");               // core 0 owns the filesystem

    const int id = app_start(&app_notes);
    app_focus(id);
    pump(500);
    const app_ctx_t *N = app_get(id);
    char opened[64];
    snprintf(opened, sizeof(opened), "%s", app_notes_message(N));

    // From a known start - the cursor begins on 'a' - "as", then Done. Each
    // tap is given time: a DOWN is held back APP_CHORD_MS to see whether BACK
    // follows it (stage 5.5), so a burst of them would arrive as a chord.
    const int path[] = { 4, 3, 4, 1, 1, 1, 3, 3, 3, 3, 4 };   // OK RIGHT OK DOWNx3 RIGHTx4 OK
    for (unsigned i = 0; i < sizeof(path) / sizeof(path[0]); ++i) {
        tap((uint32_t)path[i]);
        pump(250);
    }
    pump(400);

    const char *text = app_notes_text(N);
    const bool on_flash = file_has_raw("/fs/apps/notes/note", "as");
    snprintf(d, sizeof(d), "opened \"%s\"; typed \"%s\", said \"%s\"; on flash: %s",
             opened, text, app_notes_message(N), on_flash ? "yes" : "NO");
    sdk_case("typed on the board, kept on flash", strcmp(text, "as") == 0 && on_flash &&
             strstr(app_notes_message(N), "saved") != nullptr &&
             strstr(opened, "no note yet") != nullptr, d);

    // Closed and opened again: what comes back is what the flash holds, read
    // by the app's own fs_read from core 1.
    app_stop(id);
    wait_gone(id, 2000);
    const int id2 = app_start(&app_notes);
    app_focus(id2);
    pump(600);
    const app_ctx_t *N2 = app_get(id2);
    snprintf(d, sizeof(d), "reopened: \"%s\" (%s)", app_notes_text(N2), app_notes_message(N2));
    sdk_case("and it is there next time", strcmp(app_notes_text(N2), "as") == 0, d);

    app_stop(id2);
    wait_gone(id2, 2000);
}

#endif // SMARTOS_TEST_APPS
