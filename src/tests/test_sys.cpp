// The OS services, checked from where they are used: settings and the system
// log (4.5), killing apps (5.2), CPU accounting and the task manager (5.3),
// minimising (5.5), and the launcher's polish (5.4).
//
// Only built with -D S3_TEST_APPS: everything here opens apps.

#include "tests/tests.h"

#ifdef S3_TEST_APPS

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_timer.h"

#include "apps/apps.h"
#include "gfx/text.h"
#include "kernel/kheap.h"
#include "kernel/kmbox.h"
#include "kernel/kpark.h"
#include "kernel/ktask.h"
#include "kernel/ktime.h"
#include "os/app.h"
#include "os/evbus.h"
#include "os/fs.h"
#include "os/screen.h"
#include "os/settings.h"
#include "os/syslog.h"
#include "s3hal/hal_time.h"
#include "s3hal/input.h"
#include "s3hal/lcd_panel.h"
#include "s3hal/log.h"
#include "system.h"
#include "tests/harness.h"

// Core 1's accounting and each task's cycles over a window (stage 5.3).
typedef struct {
    uint64_t busy, idle, park, wall_cycles;
    uint64_t task[K_MAX_TASKS];
    uint32_t gen[K_MAX_TASKS];
} window_t;

static k_task_info_t s_w_info[K_MAX_TASKS];



void sys45_live_test(void)
{
    logln();
    logln("--- stage 4.5: settings and the system log, live ---");
    char d[128];
    struct stat st;

    // 1. The settings app changes the brightness: the PWM follows at once,
    //    and three quick presses reach flash as ONE write, a second after the
    //    last of them. It starts from 100 %, set here and saved first: the
    //    test used to assume the flash already said 100, and one run found 75
    //    there (stage 5.2).
    settings_set("backlight", 100);
    pump(SETTINGS_SAVE_MS + 300);
    const int sa = app_start(&app_settings);
    app_focus(sa);
    pump(300);
    const uint32_t saves0 = settings_saves();
    press(2, INPUT_PRESS);                                // LEFT: -10
    press(2, INPUT_PRESS);
    press(2, INPUT_PRESS);
    pump(300);
    const int32_t  dimmed = settings_get("backlight");
    const uint32_t duty   = lcd_backlight_duty();
    const uint32_t saves_soon = settings_saves() - saves0;
    pump(1300);
    const uint32_t saves_later = settings_saves() - saves0;
    const bool on_flash = file_has("/fs/sys/settings", "backlight=70");
    snprintf(d, sizeof(d), "%ld %%, duty %lu; saves %lu at 0.3 s, %lu at 1.6 s; file: %s",
             (long)dimmed, (unsigned long)duty, (unsigned long)saves_soon,
             (unsigned long)saves_later, on_flash ? "backlight=70" : "NOT updated");
    sdk_case("brightness via the app", dimmed == 70 && duty == 70u * 1023u / 100u &&
             saves_soon == 0 && saves_later == 1 && on_flash, d);
    press(3, INPUT_PRESS);                                // RIGHT x3: back to 100
    press(3, INPUT_PRESS);
    press(3, INPUT_PRESS);
    pump(1500);
    app_stop(sa);
    pump(200);

    // 2. Home remembers the app last opened - the launcher test opened
    //    counter, second in the list - in its own folder.
    const bool home_saved = file_has("/fs/apps/home/settings", "sel=1");
    snprintf(d, sizeof(d), "/fs/apps/home/settings has sel=1: %s", home_saved ? "yes" : "no");
    sdk_case("Home remembers", home_saved, d);

    // 3. Apps opening and closing are in the log.
    const bool opened = log_has("counter opened");
    const bool closed = log_has("counter closed");
    snprintf(d, sizeof(d), "\"counter opened\": %s, \"counter closed\": %s",
             opened ? "yes" : "no", closed ? "yes" : "no");
    sdk_case("apps in the log", opened && closed, d);

    // 3b. What one log line costs - in time, and in how long core 1 spends
    //     parked for it. Measured on 50 lines, and flushed so that the flash
    //     writes are counted, not just the bookkeeping. Written one at a time
    //     this was 52 ms a line, 46 ms of it parked. Batched, the 50 lines
    //     must leave in ONE write; its time varies with whether LittleFS has
    //     to erase a block for it (0.4 to 1.5 ms a line parked, measured), so
    //     the time bound is loose - an order of magnitude under the old cost.
    {
        slog_flush();                               // start from an empty buffer
        const uint32_t      w0 = slog_writes();
        const kpark_stats_t k0 = kpark_stats();
        const int64_t c0 = esp_timer_get_time();
        for (int i = 0; i < 50; ++i) slog("test", "cost line %02d", i);
        slog_flush();
        const uint32_t us = (uint32_t)(esp_timer_get_time() - c0);
        const kpark_stats_t k1 = kpark_stats();
        const uint32_t parked_us = (uint32_t)(k1.held_total_us - k0.held_total_us);
        const uint32_t writes    = slog_writes() - w0;
        snprintf(d, sizeof(d), "%lu us a line; core 1 parked %lu us a line; %lu write(s), "
                 "%lu flash ops for 50",
                 (unsigned long)(us / 50), (unsigned long)(parked_us / 50),
                 (unsigned long)writes, (unsigned long)(k1.flash_ops - k0.flash_ops));
        sdk_case("log line cost", writes == 1 && us / 50 < 5000 && parked_us / 50 < 5000, d);
    }

    // 3c. Nobody flushes: a routine line reaches flash on its own, after
    //     SYSLOG_FLUSH_MS and not long before.
    {
        slog_flush();
        slog("test", "timed flush marker");
        pump(SYSLOG_FLUSH_MS / 2);
        const bool early = file_has_raw("/fs/sys/log.txt", "timed flush marker");
        pump(SYSLOG_FLUSH_MS / 2 + 300);
        const bool later = file_has_raw("/fs/sys/log.txt", "timed flush marker");
        snprintf(d, sizeof(d), "on flash after %d ms: %s, after %d ms: %s",
                 SYSLOG_FLUSH_MS / 2, early ? "yes" : "no",
                 SYSLOG_FLUSH_MS + 300, later ? "yes" : "no");
        sdk_case("timed flush", !early && later, d);
    }

    // 4. Rotation: 350 lines of about a hundred bytes is past SYSLOG_MAX, so
    //    log.txt must have become log.1.txt and started again. The file on
    //    flash is checked after every line: neither may ever pass the cap, and
    //    log.1.txt must be nearly full - rotation happens only when the next
    //    batch (at most SYSLOG_BUF) would not fit.
    long cur_max = 0;
    for (int i = 0; i < 350; ++i) {
        slog("test", "rotation filler %03d ..........................................................", i);
        if (stat("/fs/sys/log.txt", &st) == 0 && (long)st.st_size > cur_max) cur_max = (long)st.st_size;
    }
    slog("test", "last line before the viewer");
    slog_flush();
    const bool rotated = stat("/fs/sys/log.1.txt", &st) == 0;
    const long old_size = rotated ? (long)st.st_size : 0;
    snprintf(d, sizeof(d), "log.1.txt %ld bytes, log.txt at most %ld bytes (cap %d)", old_size,
             cur_max, SYSLOG_MAX);
    sdk_case("rotation", rotated && cur_max <= SYSLOG_MAX && old_size <= SYSLOG_MAX &&
             old_size > SYSLOG_MAX - SYSLOG_BUF, d);

    // 5. The log app reads the tail through the mailbox and shows the newest
    //    line last.
    const int lv = app_start(&app_logview);
    app_focus(lv);
    pump(400);
    const app_ctx_t *L = app_get(lv);
    // The newest line is the app's own "log opened" - the runtime logs an
    // app opening before its init() runs, and init() is what reads the tail.
    // The first version of this check expected the test's line to be newest,
    // and failed on exactly that. The line before it must be the test's.
    const char *newest = app_logview_line(L, 0);
    const char *before = app_logview_line(L, 1);
    snprintf(d, sizeof(d), "%d lines; newest \"...%.18s\", before it \"...%.28s\"",
             app_logview_lines(L), strstr(newest, "log opened") ? "log opened" : newest,
             strstr(before, "last line") ? strstr(before, "last line") : before);
    sdk_case("log app shows the tail", app_logview_lines(L) >= 19 &&
             strstr(newest, "log opened") != nullptr &&
             strstr(before, "last line before the viewer") != nullptr, d);
    app_stop(lv);
    pump(200);

    // 6. An app logging in a loop: 200 lines at once, and the budget lets
    //    SYSLOG_BURST through - the rest are refused and counted.
    const uint32_t drop0 = slog_dropped();
    const size_t   size0 = slog_size();
    g_logspam_done = false;
    const int sp = app_start(&app_logspam);
    const int64_t t0 = esp_timer_get_time();
    while (!g_logspam_done && esp_timer_get_time() - t0 < 10000000) pump(20);
    const uint32_t dropped  = slog_dropped() - drop0;
    const uint32_t ms       = (uint32_t)((esp_timer_get_time() - t0) / 1000);
    const uint32_t accepted = 200 - dropped;
    // The budget refills while the app is still logging, so what may pass is
    // the burst plus the rate times however long the spam actually took - a
    // fixed "about 40" would be wrong for the wrong reason.
    const uint32_t allowed = SYSLOG_BURST + SYSLOG_RATE * ms / 1000 + 1;
    snprintf(d, sizeof(d), "%lu of 200 kept in %lu ms (allowed %lu), %lu refused",
             (unsigned long)accepted, (unsigned long)ms, (unsigned long)allowed,
             (unsigned long)dropped);
    sdk_case("rate limit", g_logspam_done && accepted <= allowed && accepted >= SYSLOG_BURST / 2 &&
             dropped >= 100, d);
    (void)size0;
    app_stop(sp);
    pump(1500);            // the budget refills; the next line reports the drops
    slog("test", "after the spam");
    const bool noted = log_has("lines dropped: over the rate limit");
    sdk_case("drops reported", noted, noted ? "a line says how many" : "no note in the log");
}


// ---------------------------------------------------------------------------
// Stage 5.2 - killing apps, wherever they are stuck.
// ---------------------------------------------------------------------------
static int tasks_alive(void)
{
    int n = 0;
    for (int i = 0; i < K_MAX_TASKS; ++i) {
        const k_tcb_t *t = k_task_by_id((uint8_t)i);
        if (t && t->state != K_UNUSED && t->state != K_DEAD) n++;
    }
    return n;
}



static int mbox_empty(void)
{
    uint32_t e = 0;
    kmbox_census(&e, nullptr, nullptr, nullptr);
    return (int)e;
}



static const char *task_state_name(int task)
{
    const k_tcb_t *t = (task >= 0) ? k_task_by_id((uint8_t)task) : nullptr;
    if (!t) return "?";
    switch (t->state) {
        case K_READY:    return "READY";
        case K_RUNNING:  return "RUNNING";
        case K_SLEEPING: return "SLEEPING";
        case K_BLOCKED:  return "BLOCKED";
        case K_DEAD:     return "DEAD";
        case K_ZOMBIE:   return "ZOMBIE";
        default:         return "UNUSED";
    }
}



static const char *kill_name(int r)
{
    switch (r) {
        case K_KILL_DONE:    return "at once";
        case K_KILL_PENDING: return "pending (was running)";
        case K_KILL_GONE:    return "already gone";
        case K_KILL_REFUSED: return "refused";
        default:             return "error";
    }
}


// Starts an app, lets it get going, kills it, waits for its slot. Fills d.
static bool kill_one(const app_t *a, bool focus, uint32_t (*progress)(void),
                     char *d, size_t dn)
{
    const int id = app_start(a);
    if (id < 0) { snprintf(d, dn, "could not start %s", a->name); return false; }
    if (focus) app_focus(id);
    pump(200);
    const uint32_t p0 = progress ? progress() : 0;
    pump(100);
    const uint32_t p1 = progress ? progress() : 0;
    const app_ctx_t *c = app_get(id);
    const int task = c ? c->task : -1;
    const char *was = task_state_name(task);
    const int r = app_kill(id);
    const int ms = wait_gone(id, 500);
    const uint32_t q0 = progress ? progress() : 0;
    pump(100);
    const uint32_t q1 = progress ? progress() : 0;
    const bool stopped = (q0 == q1);
    snprintf(d, dn, "was %s, killed %s, slot free in %d ms, %s", was, kill_name(r), ms,
             progress ? (p1 > p0 ? (stopped ? "stopped: yes" : "STILL RUNNING")
                                 : "never got going")
                      : "");
    return (r == K_KILL_DONE || r == K_KILL_PENDING) && ms >= 0 && (!progress || (p1 > p0 && stopped));
}


void sys52_kill_test(void)
{
    logln();
    logln("--- stage 5.2: killing apps, wherever they are stuck ---");
    char d[140];
    pump(300);
    kheap_stats_t h0;
    kheap_stats(&h0);
    const int alive0 = tasks_alive();
    const int apps0  = app_count();
    const uint32_t kills0 = k_task_kills();

    // 1. Spinning on core 1, never reading an event.
    bool ok = kill_one(&app_kt_spin, false, app_kt_spins, d, sizeof(d));
    sdk_case("kill: spinning", ok, d);

    // 2. Asleep.
    ok = kill_one(&app_kt_sleeper, false, app_kt_sleeps, d, sizeof(d));
    sdk_case("kill: sleeping", ok, d);

    // 3. Blocked in its event queue - an ordinary idle app. Then the same
    //    app again in the same slot, to show nothing it was waiting in is
    //    left pointing at it: three taps must arrive as six input events.
    {
        ok = kill_one(&app_counter, false, nullptr, d, sizeof(d));
        const int id = app_start(&app_counter);
        app_focus(id);
        pump(200);
        tap(0); tap(0); tap(0);                          // UP x3
        pump(200);
        const app_ctx_t *c = app_get(id);
        const int32_t v = c ? app_counter_value(c) : -1;
        const uint32_t in = c ? c->inputs : 0;
        char d2[220];
        snprintf(d2, sizeof(d2), "%.140s; reopened: value %ld, %lu inputs", d, (long)v,
                 (unsigned long)in);
        sdk_case("kill: blocked, then reopen", ok && v == 3 && in == 6, d2);
        app_stop(id);
        wait_gone(id, 500);
    }

    // 4. In the middle of draw(), holding the screen. Without handing the
    //    screen back, no app could ever draw again.
    {
        // Counted from before the kill: drawspin never finishes a frame, so
        // everything flushed after it started is someone else's - Home's
        // redraw, which can land inside kill_one already.
        const uint32_t r0 = app_frames_rescued();
        const uint32_t f0 = app_frames_flushed();
        ok = kill_one(&app_kt_drawspin, true, nullptr, d, sizeof(d));
        pump(300);                                       // Home takes focus and draws
        const uint32_t flushed = app_frames_flushed() - f0;
        char d2[220];
        snprintf(d2, sizeof(d2), "%.140s; screen handed back %lu, frames after %lu, focus %s",
                 d, (unsigned long)(app_frames_rescued() - r0), (unsigned long)flushed,
                 app_focused() == app_home_id() ? "Home" : "other");
        sdk_case("kill: mid-draw", ok && app_frames_rescued() - r0 == 1 && flushed >= 1 &&
                 app_focused() == app_home_id(), d2);
    }

    // 5. With a mailbox call in flight, its buffer on its own stack. Core 0
    //    is kept away from the mailbox for 20 ms first, so the call is sitting
    //    there, POSTED, when the kill lands - and app_poll is run on its own,
    //    before kmbox_service could take the job.
    {
        const int id = app_start(&app_kt_mboxer);
        pump(300);
        const uint32_t r0 = app_kt_reads();
        hal_delay_ms(20);                                // no service: the call waits
        const app_ctx_t *c = app_get(id);
        const char *was = task_state_name(c ? c->task : -1);
        const uint32_t served0 = kmbox_served_count();
        const int r = app_kill(id);
        hal_delay_ms(3);                                 // a tick or two: switched out
        app_poll();
        const int ms = wait_gone(id, 500);
        uint32_t fc = 0, fp = 0, fd = 0;
        app_last_forgot(&fc, &fp, &fd);
        pump(100);
        const int empty = mbox_empty();
        snprintf(d, sizeof(d), "was %s, killed %s; its slots: %lu claimed, %lu posted, "
                 "%lu answered; mailbox %d/%d empty; %lu reads before",
                 was, kill_name(r), (unsigned long)fc, (unsigned long)fp, (unsigned long)fd,
                 empty, KMBOX_SLOTS, (unsigned long)r0);
        (void)served0;
        sdk_case("kill: mailbox call in flight", (r == K_KILL_DONE || r == K_KILL_PENDING) &&
                 ms >= 0 && r0 > 0 && fc + fp + fd >= 1 && empty == KMBOX_SLOTS, d);
    }

    // 6. Hold BACK on an app that never reads events: it cannot hear the
    //    request, so after APP_STOP_GRACE_MS it is killed.
    {
        const int id = app_start(&app_kt_spin);
        app_focus(id);
        pump(300);
        press(5, INPUT_PRESS);
        press(5, INPUT_REPEAT);
        press(5, INPUT_RELEASE);
        const int ms = wait_gone(id, APP_STOP_GRACE_MS + 1000);
        const bool logged = log_has("did not close within");
        snprintf(d, sizeof(d), "gone %d ms after hold BACK (grace %d); the log says why: %s",
                 ms, APP_STOP_GRACE_MS, logged ? "yes" : "no");
        sdk_case("hold BACK on a stuck app", ms >= APP_STOP_GRACE_MS &&
                 ms <= APP_STOP_GRACE_MS + 200 && logged, d);
    }

    // 7. Home is not an app the user can kill.
    {
        const int r = app_kill(app_home_id());
        snprintf(d, sizeof(d), "app_kill(Home) = %d", r);
        sdk_case("Home refused", r == -2 && app_get(app_home_id()) != nullptr, d);
    }

    // 8. Twenty more, back to back: nothing may be left behind - not heap,
    //    not task slots, not mailbox slots, not app slots.
    {
        int fails = 0;
        for (int i = 0; i < 20; ++i) {
            const int id = app_start((i & 1) ? &app_kt_sleeper : &app_kt_spin);
            if (id < 0) { fails++; continue; }
            pump(30);
            app_kill(id);
            if (wait_gone(id, 500) < 0) fails++;
        }
        pump(200);
        kheap_stats_t h1;
        kheap_stats(&h1);
        snprintf(d, sizeof(d), "20 kills, %d failed; heap %u -> %u B used; tasks %d -> %d; "
                 "apps %d -> %d; mailbox %d/%d empty",
                 fails, (unsigned)h0.used, (unsigned)h1.used, alive0, tasks_alive(), apps0,
                 app_count(), mbox_empty(), KMBOX_SLOTS);
        sdk_case("nothing left behind", fails == 0 && h1.used == h0.used &&
                 tasks_alive() == alive0 && app_count() == apps0 &&
                 mbox_empty() == KMBOX_SLOTS, d);
    }
    logf("  kernel kills this run: %lu\n", (unsigned long)(k_task_kills() - kills0));
}


static void measure(window_t *w, uint32_t ms, bool flash)
{
    memset(w, 0, sizeof(*w));
    k_cpu_time_t a, b;
    int n = k_task_snapshot(s_w_info, K_MAX_TASKS);
    uint64_t start[K_MAX_TASKS] = {};
    for (int i = 0; i < n; ++i) { start[s_w_info[i].id] = s_w_info[i].cycles; w->gen[s_w_info[i].id] = s_w_info[i].gen; }
    k_cpu_time(1, &a);
    const int64_t t0 = esp_timer_get_time();

    static char blob[16 * 1024];
    int64_t next_write = t0;
    while (esp_timer_get_time() - t0 < (int64_t)ms * 1000) {
        if (flash && esp_timer_get_time() >= next_write) {
            FILE *f = fopen("/fs/sys/.acct_test", "w");
            if (f) { fwrite(blob, 1, sizeof(blob), f); fclose(f); }
            next_write += 100000;
        }
        system_pass();
        hal_delay_ms(1);
    }

    k_cpu_time(1, &b);
    const int64_t t1 = esp_timer_get_time();
    n = k_task_snapshot(s_w_info, K_MAX_TASKS);
    for (int i = 0; i < n; ++i) {
        const k_task_info_t *t = &s_w_info[i];
        w->task[t->id] = (w->gen[t->id] == t->gen) ? t->cycles - start[t->id] : t->cycles;
    }
    w->busy = b.busy - a.busy;
    w->idle = b.idle - a.idle;
    w->park = b.park - a.park;
    w->wall_cycles = (uint64_t)(t1 - t0) * 240;
    if (flash) unlink("/fs/sys/.acct_test");
}


static uint32_t pm_of(const window_t *w, uint64_t part)
{
    const uint64_t all = w->busy + w->idle + w->park;
    return all ? (uint32_t)(part * 1000 / all) : 0;
}


static uint32_t app_pm(const window_t *w, int app_id)
{
    const app_ctx_t *c = app_get(app_id);
    return (c && c->task >= 0) ? pm_of(w, w->task[c->task]) : 0;
}


void sys53_tasks_test(void)
{
    logln();
    logln("--- stage 5.3: where core 1's time goes, and the task manager ---");
    char d[150];
    window_t w;

    // 1. The books balance: busy + idle + parked is all of core 1's time,
    //    measured against esp_timer - with flash being written meanwhile, so
    //    the parked part is really there.
    measure(&w, 1000, true);
    {
        const uint64_t sum = w.busy + w.idle + w.park;
        const int64_t diff_pm = ((int64_t)sum - (int64_t)w.wall_cycles) * 1000 / (int64_t)w.wall_cycles;
        snprintf(d, sizeof(d), "busy %lu.%lu%% idle %lu.%lu%% parked %lu.%lu%%; sum vs wall clock %+ld per mille",
                 (unsigned long)(pm_of(&w, w.busy) / 10), (unsigned long)(pm_of(&w, w.busy) % 10),
                 (unsigned long)(pm_of(&w, w.idle) / 10), (unsigned long)(pm_of(&w, w.idle) % 10),
                 (unsigned long)(pm_of(&w, w.park) / 10), (unsigned long)(pm_of(&w, w.park) % 10),
                 (long)diff_pm);
        sdk_case("core 1 books balance", diff_pm >= -5 && diff_pm <= 5 && w.park > 0, d);
    }

    // 2. And task by task: every busy cycle belongs to some task.
    {
        // Tasks on core 0 are counted too, by core 0's own clock - take core
        // 1's only: every app and system task here is pinned there.
        uint64_t core1 = 0;
        for (int i = 0; i < K_MAX_TASKS; ++i) {
            const k_tcb_t *t = k_task_by_id((uint8_t)i);
            if (t && t->affinity == 1) core1 += w.task[i];
        }
        const int64_t diff = ((int64_t)core1 - (int64_t)w.busy) * 1000 / (int64_t)(w.busy ? w.busy : 1);
        snprintf(d, sizeof(d), "core-1 tasks %llu vs busy %llu cycles: %+ld per mille",
                 (unsigned long long)core1, (unsigned long long)w.busy, (long)diff);
        sdk_case("busy = sum of its tasks", diff >= -5 && diff <= 5, d);
    }

    // 3. Nothing open but Home: what the system itself costs core 1.
    measure(&w, 1000, false);
    snprintf(d, sizeof(d), "busy %lu.%lu%% with only Home open", (unsigned long)(pm_of(&w, w.busy) / 10),
             (unsigned long)(pm_of(&w, w.busy) % 10));
    sdk_case("system at rest", pm_of(&w, w.busy) < 200, d);
    const uint32_t rest_pm = pm_of(&w, w.busy);

    // 4. One app that never stops: it gets everything the system does not use.
    {
        const int sp = app_start(&app_kt_spin);
        pump(200);
        measure(&w, 1000, false);
        const uint32_t pm = app_pm(&w, sp);
        snprintf(d, sizeof(d), "spin %lu.%lu%% of core 1; busy %lu.%lu%%", (unsigned long)(pm / 10),
                 (unsigned long)(pm % 10), (unsigned long)(pm_of(&w, w.busy) / 10),
                 (unsigned long)(pm_of(&w, w.busy) % 10));
        sdk_case("one spinning app", pm >= 1000 - rest_pm - 50 && pm_of(&w, w.busy) >= 980, d);

        // 5. Two: round robin shares it evenly.
        const int hg = app_start(&app_hog);
        pump(200);
        measure(&w, 1000, false);
        const uint32_t a = app_pm(&w, sp), b = app_pm(&w, hg);
        snprintf(d, sizeof(d), "spin %lu.%lu%%, hog %lu.%lu%%", (unsigned long)(a / 10), (unsigned long)(a % 10),
                 (unsigned long)(b / 10), (unsigned long)(b % 10));
        sdk_case("two spinning apps", a > 350 && b > 350 && (a > b ? a - b : b - a) < 100, d);

        // 5b. Three. With one round-robin cursor per core, the input sampler
        //     - HIGH, awake every 2 ms - dragged the cursor back to its own low
        //     slot each time, and the scan started over from there: the first
        //     two NORMAL tasks alternated and a third never ran at all. Found
        //     in stage 5.3, by the task manager (the third task) showing 0 ms.
        {
            const int sp2 = app_start(&app_kt_spin);
            pump(200);
            measure(&w, 1000, false);
            const uint32_t x = app_pm(&w, sp), y = app_pm(&w, hg), z = app_pm(&w, sp2);
            const uint32_t lo = x < y ? (x < z ? x : z) : (y < z ? y : z);
            const uint32_t hi = x > y ? (x > z ? x : z) : (y > z ? y : z);
            snprintf(d, sizeof(d), "spin %lu.%lu%%, hog %lu.%lu%%, spin again %lu.%lu%%",
                     (unsigned long)(x / 10), (unsigned long)(x % 10), (unsigned long)(y / 10),
                     (unsigned long)(y % 10), (unsigned long)(z / 10), (unsigned long)(z % 10));
            sdk_case("three spinning apps", lo > 250 && hi - lo < 100, d);
            app_kill(sp2);
            wait_gone(sp2, 500);
        }

        // 6. The task manager sees the same thing, and kills from its own
        //    screen: select spin, RIGHT twice.
        const int tm = app_start(&app_tasks);
        app_focus(tm);
        pump(2600);                                  // two samples
        const app_ctx_t *T = app_get(tm);
        const int spin_pm = app_tasks_pm(T, "spin"), hog_pm = app_tasks_pm(T, "hog");
        int busy = 0;
        app_tasks_core_pm(T, &busy, nullptr, nullptr);

        int presses = 0;
        while (app_tasks_selected(T) != sp && presses++ < APP_MAX) { tap(1); pump(20); }
        tap(3);                                      // RIGHT: armed
        pump(100);
        char armed[60];
        snprintf(armed, sizeof(armed), "%s", app_tasks_message(T));
        tap(3);                                      // RIGHT: kill
        const int ms = wait_gone(sp, 500);
        snprintf(d, sizeof(d), "shows spin %d.%d%%, hog %d.%d%%, busy %d.%d%%; \"%s\", then gone in %d ms",
                 spin_pm / 10, spin_pm % 10, hog_pm / 10, hog_pm % 10, busy / 10, busy % 10, armed, ms);
        sdk_case("task manager", spin_pm > 350 && hog_pm > 350 && busy >= 980 &&
                 strstr(armed, "again to kill spin") != nullptr && ms >= 0, d);

        // 7. OK on an app asks it to close: the hog cannot hear that, so the
        //    2 s rule ends it - from the task manager's screen too.
        presses = 0;
        while (app_tasks_selected(T) != hg && presses++ < APP_MAX) { tap(1); pump(20); }
        tap(4);                                      // OK: close
        pump(100);
        char asked[60];
        snprintf(asked, sizeof(asked), "%s", app_tasks_message(T));
        const int hms = wait_gone(hg, APP_STOP_GRACE_MS + 1000);
        snprintf(d, sizeof(d), "\"%s\"; gone %d ms later", asked, hms);
        sdk_case("close from the task manager", strstr(asked, "asked hog to close") != nullptr &&
                 hms >= APP_STOP_GRACE_MS - 200 && hms <= APP_STOP_GRACE_MS + 300, d);
        app_stop(tm);
        wait_gone(tm, 500);
    }

    // 7b. The performance page (5.3b): LEFT to it, and its graphs follow
    //     what is really happening - a spinning app fills the CPU graph, the
    //     memory graph is the kernel heap, flash writes show in the flash one.
    {
        const int tm = app_start(&app_tasks);
        app_focus(tm);
        pump(300);
        const app_ctx_t *T = app_get(tm);
        tap(2);                                      // LEFT: performance
        const int sp = app_start(&app_kt_spin);
        pump(3500);
        int samples = 0;
        // The best second the spin app got. Not every second is a full one:
        // a flash write parks core 1 for as long as it takes, and that time
        // is not the spin app's - one run read 86.9% because a log flush
        // landed in the sampled second. What is claimed here is that a second
        // with the spin app and no flash in it is a second entirely its own.
        int cpu = app_tasks_history(T, 0, &samples);
        for (int s = 0; s < 5 && cpu < 950; ++s) {
            pump(1100);
            const int v = app_tasks_history(T, 0, &samples);
            if (v > cpu) cpu = v;
        }
        const int mem = app_tasks_history(T, 1, nullptr);
        kheap_stats_t h;
        kheap_stats(&h);
        const int heap_pm = (int)((uint64_t)h.used * 1000 / h.total);

        // What the panel really holds: the CPU graph's line colour, drawn.
        const int blue = count_colour(system_fb(), fb_rgb(17, 125, 187));
        tap(1);                                      // DOWN: memory
        pump(1200);
        const int purple = count_colour(system_fb(), fb_rgb(160, 60, 200));
        tap(1);                                      // DOWN: RAM
        pump(1200);
        const int pink = count_colour(system_fb(), fb_rgb(210, 90, 170));
        tap(1);                                      // DOWN: PSRAM
        pump(1200);
        const int violet = count_colour(system_fb(), fb_rgb(120, 110, 230));
        const int ram = app_tasks_history(T, 2, nullptr), psram = app_tasks_history(T, 3, nullptr);

        // Flash busy: write for a second and a half, then read the newest sample.
        static char blob[8 * 1024];
        const int64_t t0 = esp_timer_get_time();
        while (esp_timer_get_time() - t0 < 1500000) {
            FILE *f = fopen("/fs/sys/.perf_test", "w");
            if (f) { fwrite(blob, 1, sizeof(blob), f); fclose(f); }
            pump(20);
        }
        const int flash = app_tasks_history(T, 4, nullptr);   // CPU, kernel, RAM, PSRAM, flash
        unlink("/fs/sys/.perf_test");

        char d2[200];
        snprintf(d2, sizeof(d2), "page %d, %d samples; cpu %d.%d%%, kernel %d.%d%% (heap %d.%d%%), "
                 "ram %d.%d%%, psram %d.%d%%, flash %d.%d%%; px %d/%d/%d/%d",
                 app_tasks_page(T), samples, cpu / 10, cpu % 10, mem / 10, mem % 10,
                 heap_pm / 10, heap_pm % 10, ram / 10, ram % 10, psram / 10, psram % 10,
                 flash / 10, flash % 10, blue, purple, pink, violet);
        sdk_case("performance page", app_tasks_page(T) == 1 && samples >= 3 && cpu >= 950 &&
                 mem >= heap_pm - 20 && mem <= heap_pm + 20 && flash > 0 && ram > 0 &&
                 psram > 0 && blue > 200 && purple > 200 && pink > 200 && violet > 200, d2);
        app_kill(sp);
        wait_gone(sp, 500);
        tap(2);                                      // LEFT: back to processes
        pump(200);
        const bool back = app_tasks_page(T) == 0;
        app_stop(tm);
        wait_gone(tm, 500);
        sdk_case("back to processes", back, back ? "LEFT again: page 0" : "still on performance");
    }

    // 7c. PSRAM for apps (5.3c): 1 MB taken, written and read back, 3.5 MB
    //     more refused; and the megabyte comes back when the app closes - and
    //     when it is killed instead.
    {
        kheap_stats_t p0;
        kheap_stats_pool(KHEAP_PSRAM, &p0);
        int id = app_start(&app_kt_bigbuf);
        const int64_t t0 = esp_timer_get_time();
        while (app_kt_big_ok() < 0 && esp_timer_get_time() - t0 < 3000000) pump(20);
        pump(100);
        const app_ctx_t *B = app_get(id);
        const size_t held = B ? app_psram_bytes(B) : 0;
        app_stop(id);
        const int ms_close = wait_gone(id, 1000);
        kheap_stats_t p1;
        kheap_stats_pool(KHEAP_PSRAM, &p1);
        const bool logged = log_has("bytes of PSRAM returned");

        id = app_start(&app_kt_bigbuf);
        pump(600);
        kheap_stats_t p2;
        kheap_stats_pool(KHEAP_PSRAM, &p2);
        app_kill(id);
        wait_gone(id, 500);
        kheap_stats_t p3;
        kheap_stats_pool(KHEAP_PSRAM, &p3);

        snprintf(d, sizeof(d), "1 MB ok: %d, 3.5 MB refused: %d; held %u B; after close %u -> %u B, "
                 "held again %u, after kill %u B",
                 app_kt_big_ok(), app_kt_big_refused(), (unsigned)held, (unsigned)p0.used,
                 (unsigned)p1.used, (unsigned)p2.used, (unsigned)p3.used);
        sdk_case("app PSRAM", app_kt_big_ok() == 1 && app_kt_big_refused() == 1 &&
                 held >= 1024 * 1024 && ms_close >= 0 && p1.used == p0.used && logged &&
                 p2.used >= p0.used + 1024 * 1024 && p3.used == p0.used, d);
    }

    // 8. A sleeper costs next to nothing.
    {
        const int sl = app_start(&app_kt_sleeper);
        pump(200);
        measure(&w, 1000, false);
        const uint32_t pm = app_pm(&w, sl);
        snprintf(d, sizeof(d), "sleeper (awake every 5 ms) %lu.%lu%%", (unsigned long)(pm / 10),
                 (unsigned long)(pm % 10));
        sdk_case("a sleeping app", pm < 20, d);
        app_kill(sl);
        wait_gone(sl, 500);
    }
}


// ---------------------------------------------------------------------------
// Stage 5.5 - minimising, switching back, and room to open at all.
// ---------------------------------------------------------------------------

// DOWN held, BACK pressed, both let go: the minimise chord, as a finger does it.
static void chord_minimise(void)
{
    press(1, INPUT_PRESS);            // DOWN
    press(5, INPUT_PRESS);            // BACK
    press(5, INPUT_RELEASE);
    press(1, INPUT_RELEASE);
}


void sys55_minimise_test(void)
{
    logln();
    logln("--- stage 5.5: minimise, switch back, and room to open ---");
    char d[150];
    const int home = app_home_id();

    // 1. A counter, given a value, then minimised: it keeps running - its
    //    cursor timer goes on ticking - and Home comes to the front.
    const int ct = app_start(&app_counter);
    app_focus(ct);
    pump(300);
    tap(0); tap(0);                                  // UP x2: value 2
    pump(200);
    const app_ctx_t *C = app_get(ct);
    const int32_t before = C ? app_counter_value(C) : -1;

    chord_minimise();
    pump(300);
    const uint32_t ev0 = C ? C->events : 0;
    const bool still_open = app_get(ct) != nullptr;
    const int focus_now = app_focused();
    pump(1000);                                      // a second in the background
    const uint32_t ev1 = C ? C->events : 0;
    snprintf(d, sizeof(d), "focus %s, counter still open: %s, %lu events while minimised",
             focus_now == home ? "Home" : "NOT Home", still_open ? "yes" : "no",
             (unsigned long)(ev1 - ev0));
    sdk_case("DOWN+BACK minimises", focus_now == home && still_open && ev1 > ev0, d);

    // 2. Home lists it under "Running", and OK brings it back with its value.
    {
        const app_ctx_t *H = app_get(home);
        int guard = 0;
        while (strcmp(app_home_selected_name(H), "counter") != 0 && guard++ < 24) {
            tap(1);                                  // DOWN through the rows
            pump(60);
        }
        const char *on = app_home_selected_name(H);
        tap(4);                                      // OK
        pump(400);
        const app_ctx_t *C2 = app_get(ct);
        const int32_t after = C2 ? app_counter_value(C2) : -1;
        snprintf(d, sizeof(d), "Home selects \"%s\"; back in front: %s, value %ld (was %ld)",
                 on, app_focused() == ct ? "yes" : "no", (long)after, (long)before);
        sdk_case("Home switches back to it", strcmp(on, "counter") == 0 &&
                 app_focused() == ct && after == before && before == 2, d);
    }

    // 3. A DOWN tap is not delayed - its release sends it on at once - while a
    //    DOWN that is held waits APP_CHORD_MS for a BACK that may follow.
    {
        const app_ctx_t *C3 = app_get(ct);
        const int32_t v0 = app_counter_value(C3);
        tap(1);                                      // DOWN tap: counter -1
        pump(60);
        const int32_t v_tap = app_counter_value(C3);

        press(1, INPUT_PRESS);                       // held: nothing yet
        pump(80);
        const int32_t v_held_early = app_counter_value(C3);
        pump(150);
        const int32_t v_held_late = app_counter_value(C3);
        press(1, INPUT_RELEASE);
        pump(100);
        snprintf(d, sizeof(d), "tap: %ld -> %ld in 60 ms; held: %ld at 80 ms, %ld at 230 ms",
                 (long)v0, (long)v_tap, (long)v_held_early, (long)v_held_late);
        sdk_case("a tap is not delayed, a hold waits", v_tap == v0 - 1 &&
                 v_held_early == v_tap && v_held_late == v_tap - 1, d);
    }

    // 4. Room to open: bigbuf declares 2 MB of PSRAM and takes 1 MB; bigger
    //    declares 3 MB, which no longer fits beside it - so it is refused
    //    before anything starts, and Home says why.
    {
        const int bb = app_start(&app_kt_bigbuf);
        const int64_t t0 = esp_timer_get_time();
        while (app_kt_big_ok() < 0 && esp_timer_get_time() - t0 < 3000000) pump(20);
        pump(200);
        const app_can_t can = app_can_start(&app_kt_bigger);
        const int refused_id = app_start(&app_kt_bigger);
        const int open_now = app_user_count();

        // Through Home, so the user sees the reason on the screen.
        app_focus(home);
        pump(300);
        const app_ctx_t *H = app_get(home);
        int guard = 0;
        while (strcmp(app_home_selected_name(H), "counter") != 0 && guard++ < 24) {
            tap(1);
            pump(60);
        }
        // counter is open, so OK on it only switches; ask for bigger instead by
        // starting it the way Home would and reading the reason it gives.
        char why[60];
        snprintf(why, sizeof(why), "%s", app_can_why(can));
        snprintf(d, sizeof(d), "bigger: %s (start returned %d), %d apps open, kernel says \"%s\"",
                 can == APP_NO_PSRAM ? "refused" : "ALLOWED", refused_id, open_now, why);
        sdk_case("no room: refused, not started", can == APP_NO_PSRAM && refused_id < 0 &&
                 app_running(&app_kt_bigger) < 0, d);
        app_kill(bb);
        wait_gone(bb, 500);

        // With it gone, the same app fits.
        const app_can_t can_after = app_can_start(&app_kt_bigger);
        snprintf(d, sizeof(d), "after bigbuf closed: %s", app_can_why(can_after));
        sdk_case("room again once it closes", can_after == APP_OK, d);
    }

    app_stop(ct);
    wait_gone(ct, 500);
}


// ---------------------------------------------------------------------------
// Stage 5.4 - the launcher's polish: descriptions, a confirmation, a screen
// that switches itself off.
// ---------------------------------------------------------------------------
void sys54_polish_test(void)
{
    logln();
    logln("--- stage 5.4: descriptions, confirmation, screen off ---");
    char d[150];
    const int home = app_home_id();

    // 1. Every app registered says what it is, in a line that fits the screen.
    {
        int missing = 0, too_long = 0;
        const char *worst = "";
        for (int i = 0; i < app_registry_count(); ++i) {
            const app_t *a = app_registry_at(i);
            if (!a->about || !a->about[0]) { missing++; worst = a->name; continue; }
            if (fb_text_width(a->about, 1) > system_fb()->w - 20) { too_long++; worst = a->name; }
        }
        snprintf(d, sizeof(d), "%d apps, %d without a line, %d too wide%s%s",
                 app_registry_count(), missing, too_long, worst[0] ? ", worst: " : "", worst);
        sdk_case("every app says what it is", missing == 0 && too_long == 0, d);
    }

    // 2. Clearing the log takes two presses, and anything else cancels.
    {
        const int sa = app_start(&app_settings);
        app_focus(sa);
        pump(300);
        tap(1);                                     // DOWN: the "clear" row
        pump(150);
        const app_ctx_t *S = app_get(sa);
        slog("test", "a line that must survive the first press");
        slog_flush();
        const size_t before = slog_size();

        tap(4);                                     // OK: arms only
        pump(200);
        char armed[60];
        snprintf(armed, sizeof(armed), "%s", app_settings_message(S));
        const size_t after_one = slog_size();

        tap(0);                                     // UP: cancels
        pump(150);
        char cancelled[60];
        snprintf(cancelled, sizeof(cancelled), "%s", app_settings_message(S));

        tap(1);                                     // back to the row
        pump(150);
        tap(4);                                     // OK, OK: this time it goes
        pump(200);
        tap(4);
        pump(400);
        const size_t after_two = slog_size();
        snprintf(d, sizeof(d), "\"%.40s\" then \"%.20s\"; log %u -> %u -> %u bytes",
                 armed, cancelled, (unsigned)before, (unsigned)after_one, (unsigned)after_two);
        sdk_case("clearing the log asks first", strstr(armed, "OK again") != nullptr &&
                 after_one == before && strstr(cancelled, "not cleared") != nullptr &&
                 after_two < before, d);
        app_stop(sa);
        wait_gone(sa, 500);
    }

    // 3. The screen switches itself off after the idle time, and the press
    //    that wakes it does nothing else.
    {
        const int ct = app_start(&app_counter);
        app_focus(ct);
        pump(300);
        const app_ctx_t *C = app_get(ct);
        const int32_t v0 = app_counter_value(C);
        const uint32_t offs0 = screen_offs();
        settings_set("screenoff", 2);
        settings_set("backlight", 100);

        // Nothing pressed for over two seconds. The idle clock is restarted
        // here with a lone release - apps ignore those - so the two seconds
        // are counted from now rather than from the last tap above.
        press(2, INPUT_RELEASE);
        pump(50);
        const int64_t t0 = esp_timer_get_time();
        while (!screen_is_off() && esp_timer_get_time() - t0 < 4000000) pump(20);
        const int off_ms = (int)((esp_timer_get_time() - t0) / 1000);
        const uint32_t duty_off = lcd_backlight_duty();

        tap(0);                                     // UP: wakes it, and no more
        pump(300);
        const uint32_t duty_on = lcd_backlight_duty();
        const int32_t v_wake = app_counter_value(C);

        tap(0);                                     // UP again: now it counts
        pump(200);
        const int32_t v_after = app_counter_value(C);

        snprintf(d, sizeof(d), "off after %d ms (duty %lu), woke to duty %lu; counter %ld -> %ld -> %ld",
                 off_ms, (unsigned long)duty_off, (unsigned long)duty_on, (long)v0, (long)v_wake,
                 (long)v_after);
        sdk_case("screen off, and the press that wakes it", screen_offs() == offs0 + 1 &&
                 off_ms >= 1900 && off_ms <= 2600 && duty_off == 0 && duty_on == 1023 &&
                 v_wake == v0 && v_after == v0 + 1, d);

        settings_set("screenoff", 0);               // off again for the rest
        app_stop(ct);
        wait_gone(ct, 500);
        pump(200);
    }

    // 4. And with it set to 0 it never switches off.
    {
        const uint32_t offs0 = screen_offs();
        pump(2500);
        snprintf(d, sizeof(d), "screenoff = 0: %lu switch-offs in 2.5 s idle",
                 (unsigned long)(screen_offs() - offs0));
        sdk_case("0 means never", screen_offs() == offs0 && !screen_is_off(), d);
    }
    (void)home;
}

#endif   // S3_TEST_APPS
