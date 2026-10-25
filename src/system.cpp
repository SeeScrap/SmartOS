// Core 0's main loop: the system around the apps.
//
// main.cpp is the boot sequence; this is what runs forever afterwards. Core 0
// owns the panel, so it does the two jobs that need SPI - drawing the status
// bar and flushing the frames apps hand over - and it keeps serving core 1's
// mailbox and watching for a fault.
//
// Moved out of main.cpp in R2d of the SmartOS restructuring.

#include "boot_gfx.h"
#include "system.h"

#include <stdio.h>
#include <string.h>

#include "esp_timer.h"

#include "gfx/fb.h"
#include "gfx/text.h"
#include "kernel/kfault.h"
#include "kernel/kheap.h"
#include "kernel/kmbox.h"
#include "kernel/ktask.h"
#include "kernel/ktime.h"
#include "apps/apps.h"
#include "os/app.h"
#include "os/boot.h"
#include "os/evbus.h"
#include "os/fs.h"
#include "os/screen.h"
#include "os/settings.h"
#include "os/shell.h"
#include "os/syslog.h"
#include "os/sysinfo.h"
#include "s3hal/hal_time.h"
#include "s3hal/input.h"
#include "s3hal/lcd_panel.h"
#include "s3hal/log.h"
#include "tests/harness.h"
#include "tests/tests.h"

// ---------------------------------------------------------------------------
// Stage 4.3b - core 0's main loop: the system around the apps.
//
// Core 0 owns the panel, so it does the two jobs that need SPI: drawing the
// status bar, and flushing the frames apps hand over. It also keeps serving
// core 1's mailbox and watching for a fault, as before.
// ---------------------------------------------------------------------------
static int s_status_sub = -1;

static void status_draw(void)
{
    // Stage 6.1: while the shell's "bootscreen" preview is up, the bar stays
    // off it - otherwise the clock repaints over the top of the picture a
    // second later, and a screenshot of the boot screen is not one.
    if (boot_preview_active()) return;

    const uint16_t bar = fb_rgb(40, 48, 72);
    fb_fill_rect(system_fb(), 0, 0, system_fb()->w, APP_AREA_Y - 1, bar);
    fb_hline(system_fb(), 0, APP_AREA_Y - 1, system_fb()->w, FB_CYAN);

    const int f = app_focused();
    const app_ctx_t *c = (f != APP_NONE) ? app_get(f) : nullptr;
    fb_text(system_fb(), 6, 2, c ? c->app->name : "-", FB_WHITE, 2);

    // Stage 5.4: how long it has been up, and how much room is left on the
    // filesystem - the two things worth a glance from any app.
    const uint32_t s = k_millis() / 1000;
    sysinfo_t si;
    sysinfo_get(&si);
    char right[64];
    snprintf(right, sizeof(right), "apps %d   %lu.%lu MB free   %lu:%02lu:%02lu", app_count(),
             (unsigned long)((si.fs_total - si.fs_used) / (1024 * 1024)),
             (unsigned long)((si.fs_total - si.fs_used) % (1024 * 1024) / (104858)),
             (unsigned long)(s / 3600), (unsigned long)(s / 60 % 60), (unsigned long)(s % 60));
    fb_text(system_fb(), system_fb()->w - fb_text_width(right, 1) - 6, 8, right, FB_GREY, 1);
}

// One pass of the loop. The scripted test below runs these too, so that apps
// get their frames flushed while the script is waiting on them.
void system_pass(void)
{
    kmbox_service();
    app_poll();                     // stage 5.2: apps that ended or were killed
    k_free_dead_stacks();           // stage 5.3c: ended tasks' stacks, back to the heap
    if (kfault_pending()) kfault_report();

    bool redraw = false;
    ev_t ev;
    while (evbus_try_recv(s_status_sub, &ev)) redraw = true;   // focus or clock
    // Stage 6.4d: and not while the panel is dark. Stage 5.4e stopped the app
    // area going out to a screen nobody can see, but the status bar has its own
    // way to the panel and kept taking it - its clock ticks once a second
    // whatever the backlight is doing, so a board left on a desk was still
    // pushing the bar down the SPI bus all night. The bar is redrawn by the
    // next tick after the screen comes back.
    if (redraw && !screen_is_off()) { status_draw(); fb_flush(system_fb()); }

    while (app_compositor_poll()) { }
    settings_poll();                // a settled change reaches flash
    slog_poll();                    // log lines that have waited long enough
    sysinfo_poll();                 // stage 5.3b: RAM and storage figures, for core 1
    screen_poll();                  // stage 5.4: the backlight, after a quiet spell
}

void system_loop(void)
{
    static ev_t status_q[16];
    s_status_sub = evbus_subscribe("status", EV_MASK(EV_SYS), EV_Q(status_q));
    evbus_timer_start(s_status_sub, 1000, 1);   // the clock in the corner

#ifndef SMARTOS_KERNEL_TEST_BUILD
    // Stage 6.1: the boot screen has been up since the panel was lit. It is
    // held to its minimum HERE, before the status bar is drawn - the first
    // version cleared the screen and drew the bar first, so the mark was wiped
    // and the last second of boot was a spinner floating on an empty bar.
    boot_step("the launcher");
    boot_screen_hold();
    boot_screen_end();

    // Every drawing primitive at once, held long enough to look at. Its whole
    // purpose is to let clipping and colour order be judged by eye, and from
    // stage 6.1 until R2f it could not be: it was painted before the boot
    // screen, while the backlight was still at 0, and then painted over.
    //
    // The wait serves the mailbox but does not call system_pass(): a pass can
    // decide to redraw the status bar, which would put the bar across the top
    // of the scene. Core 1 is already running, and kmbox_call() gives up after
    // KMBOX_TIMEOUT_MS, so leaving the mailbox unserved for this long is the
    // one thing that would really break.
#if !defined(SMARTOS_GFX_SELFTEST) && GFX_DEMO_HOLD_MS > 0
    gfx_demo_scene();
    const int64_t demo_t0 = esp_timer_get_time();
    while ((esp_timer_get_time() - demo_t0) / 1000 < GFX_DEMO_HOLD_MS) {
        kmbox_service();
        hal_delay_ms(2);
    }
#endif
#endif

    fb_clear(system_fb(), fb_rgb(16, 20, 32));
    status_draw();
    fb_present(system_fb());

#ifndef SMARTOS_KERNEL_TEST_BUILD
    // At boot only the system starts: Home. Every app waits for the user to
    // open it. (Until stage 4.3d every registered app was started here.)
    for (int i = 0; i < app_registry_count(); ++i) {
        const app_t *a = app_registry_at(i);
        if (a->flags & APP_FLAG_SYSTEM) {
            logf("[apps] %s (system) -> %d\n", a->name, app_start(a));
        }
    }
    pump(300);
    // Stage 6.1: the boot screen was up, and was still whole when it ended.
    {
        char d[112];
        snprintf(d, sizeof(d), "up %lu ms, %lu steps, %lu frames, %lu px of the mark at the end, duty %lu",
                 (unsigned long)boot_screen_ms(), (unsigned long)boot_steps(),
                 (unsigned long)boot_frames(), (unsigned long)boot_mark_px(),
                 (unsigned long)lcd_backlight_duty());
        logln();
        logln("--- stage 6.1: the boot screen ---");
        app_case("boot screen", boot_screen_shown() && boot_screen_ms() >= BOOT_MIN_MS &&
                 boot_steps() >= 8 && boot_frames() > boot_steps() &&
                 boot_mark_px() > 200 && lcd_backlight_duty() > 0, d);
    }

    {
        char d[80];
        snprintf(d, sizeof(d), "%d apps running, %d registered, focus %s",
                 app_user_count(), app_registry_count() - 1,
                 app_focused() == app_home_id() ? "Home" : "NOT Home");
        logln();
        logln("--- stage 4.3d: nothing runs until the user opens it ---");
        app_case("boot starts no app", app_user_count() == 0 &&
                 app_focused() == app_home_id() && app_home_id() != APP_NONE, d);
    }

    // Stage 5.3c: the kernel's two pools, taken from ESP-IDF at boot, and no
    // ended task still sitting on its stack. Before 5.3c the heap was 64 KB and
    // thirteen finished self-test tasks held 29,952 bytes of it for good.
    {
        pump(50);
        kheap_stats_t in, ps;
        kheap_stats_pool(KHEAP_INTERNAL, &in);
        kheap_stats_pool(KHEAP_PSRAM, &ps);
        int held = 0;
        for (int i = 0; i < K_MAX_TASKS; ++i) {
            const k_tcb_t *t = k_task_by_id((uint8_t)i);
            if (t && t->state == K_DEAD && t->stack) held++;
        }
        char d[120];
        snprintf(d, sizeof(d), "internal %u of %u B used (asked %u KB), psram %u KB; %d ended tasks hold stacks",
                 (unsigned)in.used, (unsigned)in.total, (unsigned)(KHEAP_INTERNAL_SIZE / 1024),
                 (unsigned)(ps.total / 1024), held);
        // Exact: every byte of each region is a block header (16 B) or a
        // payload. The internal pool's size is what it was given, which since
        // stage 6.3 may be less than it asked for - the radio's memory comes
        // out of the same heap - so the claim is that the arithmetic adds up
        // and that what it got is enough to work in, not that it is 128 KB.
        const size_t in_region = in.total + in.blocks * 16;
        app_case("kernel memory", in_region >= KHEAP_INTERNAL_MIN &&
                 in_region <= KHEAP_INTERNAL_SIZE &&
                 ps.total + ps.blocks * 16 == KHEAP_PSRAM_SIZE && held == 0 && in.used < 20000, d);
    }

    // The tests below open apps, which only the user should do - so they are
    // a test build now, not part of every boot.
#ifdef SMARTOS_TEST_APPS
    launcher_test();
    {
        const int in = app_start(&app_input);
        const int ct = app_start(&app_counter);
        const int bn = app_start(&app_bounce);
        const int bn2 = app_model_test(in, ct, bn);
#ifdef SMARTOS_TEST_APPHOG
        app_hog_test(bn2);
#else
        (void)bn2;
#endif
    }
    sdk_live_test();
    fs_live_test();
    sys45_live_test();
    close_user_apps();
    sys52_kill_test();
    sys53_tasks_test();
    sys55_minimise_test();
    sys54_polish_test();
    sys54d_limits_test();
    sys54e_quiet_test();
    sys62_wallpaper_test();
    sys63_radio_test();
    sys64_keyboard_test();
#ifdef SMARTOS_TEST_FSFULL
    sys54d_fsfull_test();
#endif
#endif
#endif

    uint32_t next_alive = k_millis() + 15000;
    shell_set_screen(system_fb());
    shell_init();
    for (;;) {
        system_pass();
        shell_poll();               // stage 5.1: only now, after the boot tests

        // A heartbeat, because a quiet serial log cannot tell "nobody pressed
        // anything" from "something hung". It runs off the clock directly, not
        // off a bus timer: a heartbeat must not depend on the thing it watches.
        const uint32_t now = k_millis();
        if ((int32_t)(now - next_alive) >= 0) {
            logf("[alive] kernel %lu ms, sampler passes %lu, frames flushed %lu, "
                 "focus %d, %d apps open\n",
                 (unsigned long)now, (unsigned long)input_samples(),
                 (unsigned long)app_frames_flushed(), app_focused(), app_user_count());
            next_alive = now + 15000;
        }

        hal_delay_ms(2);
    }
}
