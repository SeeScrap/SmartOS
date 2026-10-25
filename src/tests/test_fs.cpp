#include "tests/tests.h"

#ifdef SMARTOS_TEST_APPS

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_timer.h"

#include "port/kpark.h"

#include "apps/apps.h"
#include "os/app.h"
#include "os/dev.h"
#include "os/fs.h"
#include "port/kport.h"
#include "board/input.h"
#include "board/log.h"
#include "system.h"
#include "tests/harness.h"

// ---------------------------------------------------------------------------
// Stage 4.4 - files, from where apps use them (moved out of main.cpp in 5.4b).
// ---------------------------------------------------------------------------


// ---------------------------------------------------------------------------
// Stage 4.4 - files, from where apps use them.
// ---------------------------------------------------------------------------
void fs_live_test(void)
{
    logln();
    logln("--- stage 4.4: files from an app on core 1 ---");
    char d[128];

    // 1. The API, run by a test app on core 1: every call crosses the
    //    mailbox to LittleFS on core 0, under the global park hook.
    const kpark_stats_t p0 = kpark_stats();
    g_fscheck_n = 0;
    g_fscheck_done = false;
    const int ft = app_start(&app_fstest);
    const int64_t t0 = esp_timer_get_time();
    while (!g_fscheck_done && esp_timer_get_time() - t0 < 15000000) pump(50);
    const kpark_stats_t p1 = kpark_stats();
    for (int i = 0; i < g_fscheck_n; ++i) {
        sdk_case(g_fscheck[i].what, g_fscheck[i].ok, g_fscheck[i].detail);
    }
    if (!g_fscheck_done) sdk_case("fstest finished", false, "timed out");
    app_stop(ft);
    pump(200);

    // 2. Sandboxing, checked from the outside: the app's files went into its
    //    own folder and nowhere else.
    struct stat st;
    const bool own   = stat("/fs/apps/fstest", &st) == 0;
    const bool leak1 = stat("/fs/x", &st) == 0;
    const bool leak2 = stat("/fs/apps/x", &st) == 0;
    snprintf(d, sizeof(d), "own folder: %s; nothing at /fs/x or /fs/apps/x: %s",
             own ? "yes" : "no", (!leak1 && !leak2) ? "yes" : "NO");
    sdk_case("sandbox", own && !leak1 && !leak2, d);

    snprintf(d, sizeof(d), "%lu flash ops, %lu unparked, core 1 parked %lu us at worst",
             (unsigned long)(p1.flash_ops - p0.flash_ops),
             (unsigned long)(p1.unparked - p0.unparked), (unsigned long)p1.held_max_us);
    sdk_case("every op parked", p1.flash_ops > p0.flash_ops && p1.unparked == p0.unparked, d);

    // 3. A real app writing a real file: two seconds of scope recording,
    //    read back from core 0 straight off the filesystem.
    const int sc = app_start(&app_scope);
    app_focus(sc);
    pump(300);
    // Flash stays busy during the recording - the app's own appends, the log -
    // and until the sampling moved to an interrupt that cost readings in step
    // with it (up to 20 of 200). So: every reading kept, 10 ms apart.
    const kpark_stats_t w0 = kpark_stats();
    const uint32_t      lost0 = dev_samples_lost();
    const evbus_stats_t e0 = evbus_stats(app_get(sc)->sub);
    press(3, INPUT_PRESS);                                    // RIGHT: record
    pump(2000);
    press(3, INPUT_PRESS);                                    // RIGHT: stop
    const kpark_stats_t w1 = kpark_stats();
    const uint32_t      lost = dev_samples_lost() - lost0;
    const evbus_stats_t e1 = evbus_stats(app_get(sc)->sub);
    pump(300);
    const uint32_t app_lines = app_scope_rec_lines(app_get(sc));

    uint32_t lines = 0, bad = 0, prev_t = 0, gap = 0, first_t = 0;
    bool header = false;
    FILE *f = fopen("/fs/apps/scope/log.csv", "r");
    if (f) {
        char ln[48];
        while (fgets(ln, sizeof(ln), f)) {
            if (!header) { header = (strcmp(ln, "t_ms,mV\n") == 0); continue; }
            unsigned long t = 0, mv = 0;
            if (sscanf(ln, "%lu,%lu", &t, &mv) != 2 || mv > 3300 || t < prev_t) bad++;
            if (!lines) first_t = (uint32_t)t;
            if (lines && t - prev_t > gap) gap = (uint32_t)(t - prev_t);
            prev_t = (uint32_t)t;
            lines++;
        }
        fclose(f);
    }
    // A reading the app never collected is not automatically a fault: while
    // core 0 writes flash, core 1 is parked and the app cannot empty its queue
    // (stage 4.3a). At one reading every 10 ms, a park of N ms can account for
    // N/10 of them - the same way the event bus's timer test judges lateness
    // beyond park time. What is never acceptable is a reading the sampler
    // itself lost (`lost`), or one that reached the app and not the file.
    const uint32_t dropped = e1.dropped - e0.dropped;
    const uint32_t park_ms = (uint32_t)((w1.held_total_us - w0.held_total_us) / 1000);
    const uint32_t explained = park_ms / 10;
    snprintf(d, sizeof(d), "%lu lines (app %lu), %lu bad, span %lu ms, gap <= %lu ms; "
             "flash busy %lu ms; lost %lu, dropped %lu of %lu a park explains",
             (unsigned long)lines, (unsigned long)app_lines, (unsigned long)bad,
             (unsigned long)(prev_t - first_t), (unsigned long)gap,
             (unsigned long)park_ms, (unsigned long)lost,
             (unsigned long)dropped, (unsigned long)explained);
    sdk_case("scope records 2 s", f && header && bad == 0 && lines == app_lines &&
             lost == 0 && dropped <= explained && gap <= 11 &&
             lines + dropped >= 195 && lines <= 205, d);
    app_stop(sc);
    pump(200);
}

#endif // SMARTOS_TEST_APPS
