#include "tests/tests.h"

#ifdef SMARTOS_TEST_APPS

#include <stdio.h>
#include <string.h>

#include "esp_timer.h"

#include "apps/apps.h"
#include "kernel/kheap.h"
#include "kernel/ktask.h"
#include "os/app.h"
#include "os/evbus.h"
#include "os/fs.h"
#include "os/screen.h"
#include "os/settings.h"
#include "os/syslog.h"
#include "s3hal/hal_time.h"
#include "s3hal/input.h"
#include "s3hal/log.h"
#include "system.h"
#include "tests/harness.h"

// ---------------------------------------------------------------------------
// Stage 5.4d - the limits, on purpose.
//
// Everything here is a thing the board can run out of. Each one had a number
// in a header and nothing that ever reached it, so what actually happened at
// the limit was a guess - and a guess about the worst moment to be wrong.
//
//   the stack      every app used to get 2 KB whether it needed 2 KB or not
//   the timers     EVBUS_MAX_TIMERS for the whole system
//   the bus        EVBUS_MAX_SUBS subscriber slots, apps included
//   the pool       an app whose state the kernel pool cannot hold
//   the disk       12 MB of LittleFS, filled (-D SMARTOS_TEST_FSFULL: minutes)
//
// What is claimed is not that these never happen. It is that when they do, the
// system says which one it was, keeps running, and gives the memory back.
// ---------------------------------------------------------------------------

// The kernel task behind an app, for its stack figures.
static const k_task_info_t *task_of(k_task_info_t *info, int n, const app_ctx_t *a)
{
    for (int i = 0; i < n; ++i) {
        if (a && info[i].id == a->task && info[i].gen == a->task_gen) return &info[i];
    }
    return nullptr;
}

void sys54d_limits_test(void)
{
    logln();
    logln("--- stage 5.4d: the limits ---");
    char d[128];

    // 1. A stack the app chose. tiny asks for 1 KB, where every app used to
    //    get 2 KB, and uses a few hundred bytes of it.
    {
        const int tid = app_start(&app_kt_tiny);
        pump(300);
        k_task_info_t info[K_MAX_TASKS];
        const int n = k_task_snapshot(info, K_MAX_TASKS);
        const app_ctx_t *a = app_get(tid);
        const k_task_info_t *t = task_of(info, n, a);
        const uint32_t size = t ? t->stack_size : 0;
        const uint32_t used = t ? t->stack_used : 0;
        snprintf(d, sizeof(d), "asked 1024, got %lu, %lu used, %d frames deep",
                 (unsigned long)size, (unsigned long)used, app_kt_tiny_depth());
        sdk_case("an app picks its stack", tid >= 0 && size == 1024 && used > 0 &&
                 used < 1024 && app_kt_tiny_depth() == 3, d);

        // And the default is still the default.
        const int cid = app_start(&app_counter);
        pump(200);
        const int n2 = k_task_snapshot(info, K_MAX_TASKS);
        const k_task_info_t *tc = task_of(info, n2, app_get(cid));
        snprintf(d, sizeof(d), "counter: %lu bytes; the kernel's own floor %d, ceiling %d",
                 (unsigned long)(tc ? tc->stack_size : 0), (int)k_stack_bytes_for(1),
                 (int)k_stack_bytes_for(1024 * 1024));
        sdk_case("2 KB unless it says otherwise", tc && tc->stack_size == K_STACK_SIZE &&
                 k_stack_bytes_for(1) == K_STACK_MIN &&
                 k_stack_bytes_for(1024 * 1024) == K_STACK_MAX, d);
        app_stop(tid); wait_gone(tid, 1000);
        app_stop(cid); wait_gone(cid, 1000);
    }

    // 2. More timers than the system has. The app keeps running and what it
    //    did get still ticks - and the refusal is in the log, with its name.
    {
        const uint32_t denied0 = app_timers_denied();
        const int id = app_start(&app_kt_timers);
        pump(400);
        const uint32_t ticks0 = app_kt_timer_ticks();
        pump(400);
        const bool alive = app_get(id) != nullptr;
        const uint32_t ticked = app_kt_timer_ticks() - ticks0;
        snprintf(d, sizeof(d), "asked %d, got %d, refused %d; %lu ticks after, alive %d",
                 EVBUS_MAX_TIMERS + 4, app_kt_timers_got(), app_kt_timers_refused(),
                 (unsigned long)ticked, (int)alive);
        sdk_case("more timers than there are", app_kt_timers_got() > 0 &&
                 app_kt_timers_refused() >= 4 && alive && ticked > 0 &&
                 app_timers_denied() > denied0 &&
                 log_has("timers asked for a timer"), d);
        app_stop(id);
        wait_gone(id, 1000);
    }

    // 3. A state block bigger than one app is allowed (APP_KERNEL_MAX, stage
    //    5.4e) and bigger than the pool besides: refused before anything is
    //    created, and nothing left behind.
    {
        pump(300);                      // the apps above are reaped first, so
        const int before = app_user_count();     // the pool figure holds still
        kheap_stats_t h0;
        kheap_stats_pool(KHEAP_INTERNAL, &h0);
        const app_can_t why = app_can_start(&app_kt_hungry);
        const int id = app_start(&app_kt_hungry);
        pump(100);
        kheap_stats_t h1;
        kheap_stats_pool(KHEAP_INTERNAL, &h1);
        snprintf(d, sizeof(d), "256 KB of state: \"%s\", start %d, apps %d -> %d, pool %u -> %u",
                 app_can_why(why), id, before, app_user_count(),
                 (unsigned)h0.used, (unsigned)h1.used);
        sdk_case("state past what one app may take", why == APP_TOO_BIG && id < 0 &&
                 app_user_count() == before && h1.used <= h0.used, d);
    }

    // 4. The event bus's subscriber slots, taken until an app cannot have one.
    //    This is the limit Home could not explain before: app_start simply
    //    returned -1 and it said "could not start".
    {
        static ev_t q[EVBUS_MAX_SUBS][2];
        int subs[EVBUS_MAX_SUBS], taken = 0;
        const int free0 = evbus_subs_free();
        while (evbus_subs_free() > 0 && taken < EVBUS_MAX_SUBS) {
            subs[taken] = evbus_subscribe("hog", 0, q[taken], 2);
            if (subs[taken] < 0) break;
            taken++;
        }
        const app_can_t why = app_can_start(&app_counter);
        const int id = app_start(&app_counter);
        for (int i = 0; i < taken; ++i) evbus_unsubscribe(subs[i]);
        const app_can_t after = app_can_start(&app_counter);
        snprintf(d, sizeof(d), "%d slots taken: \"%s\", start %d; free again %d, then \"%s\"",
                 taken, app_can_why(why), id, evbus_subs_free(),
                 after == APP_OK ? "OK" : app_can_why(after));
        sdk_case("the bus runs out of slots", free0 > 0 && taken == free0 &&
                 why == APP_NO_BUS && id < 0 && evbus_subs_free() == free0 &&
                 after == APP_OK, d);
    }
}

// --- stage 5.4e: what the board does NOT do -----------------------------------
//
// Two savings, one of time and power and one of room, and both only mean
// anything if they can be measured.
void sys54e_quiet_test(void)
{
    logln();
    logln("--- stage 5.4e: dark frames, and a ceiling per app ---");
    char d[128];

    // 1. An app drawing 50 frames a second with the backlight down: the
    //    frames are taken, and none of them reaches the panel.
    {
        const int bn = app_start(&app_bounce);
        app_focus(bn);
        pump(400);
        settings_set("screenoff", 2);
        settings_set("backlight", 100);
        press(2, INPUT_RELEASE);                // restarts the idle clock
        const int64_t t0 = esp_timer_get_time();
        while (!screen_is_off() && esp_timer_get_time() - t0 < 4000000) pump(20);

        const uint32_t sent0 = app_frames_flushed(), dark0 = app_frames_dark();
        const uint32_t px0 = app_pixels_sent();
        pump(1000);
        const uint32_t sent = app_frames_flushed() - sent0;
        const uint32_t dark = app_frames_dark() - dark0;
        const uint32_t dark_px = app_pixels_sent() - px0;

        tap(0);                                 // UP wakes it (and is swallowed)
        pump(400);
        const uint32_t after = app_frames_flushed() - sent0 - sent;
        const uint32_t px1 = app_pixels_sent();
        pump(1000);                             // the same second, awake
        const uint32_t awake_px = app_pixels_sent() - px1;

        snprintf(d, sizeof(d), "dark: %lu frames taken, %lu sent, %lu px; awake: %lu px (%lu B) a second",
                 (unsigned long)dark, (unsigned long)sent, (unsigned long)dark_px,
                 (unsigned long)awake_px, (unsigned long)(awake_px * 2));
        sdk_case("no frames while the screen is off", screen_is_off() == false &&
                 sent == 0 && dark_px == 0 && dark >= 30 && after > 5 && awake_px > 0, d);

        settings_set("screenoff", 0);
        app_stop(bn);
        wait_gone(bn, 1000);
    }

    // 2. One app may not take more than APP_KERNEL_MAX of the kernel pool,
    //    however empty the pool is - so what it is told does not depend on
    //    what else happens to be open.
    {
        kheap_stats_t in;
        kheap_stats_pool(KHEAP_INTERNAL, &in);
        const app_can_t why = app_can_start(&app_kt_hungry);
        int over = 0;
        const char *worst = "";
        size_t most = 0;
        for (int i = 0; i < app_registry_count(); ++i) {
            const app_t *a = app_registry_at(i);
            const size_t takes = K_STACK_GUARD + k_stack_bytes_for(a->stack) + a->user_size;
            if (takes > most) { most = takes; worst = a->name; }
            if (takes > APP_KERNEL_MAX) over++;
        }
        snprintf(d, sizeof(d), "hungry: \"%s\" with %u B free; biggest real app %s at %u B of %d",
                 app_can_why(why), (unsigned)in.free_bytes, worst, (unsigned)most, APP_KERNEL_MAX);
        sdk_case("a ceiling for one app", why == APP_TOO_BIG && over == 0 &&
                 in.free_bytes > APP_KERNEL_MAX, d);
    }
}

// --- the filesystem, filled (-D SMARTOS_TEST_FSFULL) ------------------------------
#ifdef SMARTOS_TEST_FSFULL
void sys54d_fsfull_test(void)
{
    logln();
    logln("--- stage 5.4d: the filesystem, filled ---");
    logln("  (this writes about 12 MB and takes minutes)");
    char d[160];

    const uint32_t lost0 = slog_lost_batches();
    const uint32_t writes0 = slog_writes();
    size_t total = 0, used0 = 0;
    fs_info(&total, &used0);

    const int64_t t0 = esp_timer_get_time();
    const int id = app_start(&app_kt_filler);
    // Wait for the app's whole init - fill, one more write, a log line, then
    // deleting it all - not just for the write that found the end. No timeout:
    // half a filesystem of leftovers is worse than a long wait. (The first run
    // waited on the fill alone and read the three figures below while the app
    // was still working, which is why they said 99 and 2904 KB.)
    while (!app_kt_fill_done() && app_get(id)) pump(100);
    const int secs = (int)((esp_timer_get_time() - t0) / 1000000);

    // A byte is not 8 KB: LittleFS can still place a small write when a large
    // one has nowhere to go, so what is claimed is the error, not that the
    // filesystem is unusable from then on.
    snprintf(d, sizeof(d), "%lu KB in %d files, then %d (%s); one byte more: %d (%s); %d s all in",
             (unsigned long)app_kt_fill_kb(), app_kt_fill_files(), app_kt_fill_rc(),
             app_kt_fill_rc() == FS_ERR_FULL ? "full" : "not FS_ERR_FULL",
             app_kt_fill_small_rc(), app_kt_fill_small_rc() == FS_OK ? "still fits" : "refused too",
             secs);
    sdk_case("writing until it is full", app_kt_fill_rc() == FS_ERR_FULL &&
             app_kt_fill_kb() > 1024 &&
             (app_kt_fill_small_rc() == FS_OK || app_kt_fill_small_rc() == FS_ERR_FULL), d);

    // The log is on the same filesystem, so while it was full its lines had
    // nowhere to go. Either they got through - the log rotates, which frees a
    // whole log.1.txt - or they were dropped, and then they are counted and
    // the console was told. Both are fine; silence would not be.
    pump(3000);
    slog_flush();
    const uint32_t lost = slog_lost_batches() - lost0;
    snprintf(d, sizeof(d), "%lu batches lost (%lu bytes), %lu writes since; log %u bytes",
             (unsigned long)lost, (unsigned long)slog_lost_bytes(),
             (unsigned long)(slog_writes() - writes0), (unsigned)slog_size());
    sdk_case("the log survives a full disk", slog_writes() > writes0 &&
             (lost == 0 || slog_lost_bytes() > 0), d);

    // And the space came back.
    app_stop(id);
    wait_gone(id, 2000);
    size_t total2 = 0, used2 = 0;
    fs_info(&total2, &used2);
    snprintf(d, sizeof(d), "%u KB used of %u KB, was %u KB before the fill (%lu ms in the app)",
             (unsigned)(used2 / 1024), (unsigned)(total2 / 1024), (unsigned)(used0 / 1024),
             (unsigned long)app_kt_fill_ms());
    sdk_case("deleting gives it back", used2 < used0 + 64 * 1024, d);
}
#endif // SMARTOS_TEST_FSFULL

#endif // SMARTOS_TEST_APPS
