#include "tests/tests.h"

#ifdef SMARTOS_TEST_APPS

#include <stdio.h>
#include <string.h>

#include "esp_timer.h"

#include "apps/apps.h"
#include "os/app.h"
#include "os/evbus.h"
#include "s3hal/hal_time.h"
#include "s3hal/input.h"
#include "s3hal/log.h"
#include "system.h"
#include "tests/harness.h"

// ---------------------------------------------------------------------------
// Stage 4.3b - the app model, driven by script (moved out of main.cpp in 5.4b).
//
// These open apps, which on a normal boot only the user does, so the whole
// file is empty unless the build says -D SMARTOS_TEST_APPS.
// ---------------------------------------------------------------------------


// What the user does: choose an app in Home, open it, use it, close it.
void launcher_test(void)
{
    logln();
    logln("--- stage 4.3d: Home, driven by script ---");
    char d[96];

    // Home lists input, counter, bounce, ... in registry order, and since
    // stage 4.5 it starts on the row it last opened - remembered on flash
    // across reboots. The first run with that in place pressed DOWN once, as
    // this test always had, and opened bounce: Home had come up on counter.
    // So: note where Home came up, then walk to counter (row 1) from there.
    const app_ctx_t *H = app_get(app_home_id());
    const int booted_on = app_home_selected(H);
    logf("  Home came up on row %d (remembered from before this boot)\n", booted_on);
    for (int guard = 0; guard < 16 && app_home_selected(H) != 1; ++guard) {
        press(app_home_selected(H) > 1 ? 0 : 1, INPUT_PRESS);   // UP or DOWN
        pump(60);
    }
    press(4, INPUT_PRESS);                                // OK: open it
    pump(300);
    int ct = app_running(&app_counter);
    snprintf(d, sizeof(d), "counter id %d, focused: %s, apps open %d", ct,
             (ct >= 0 && app_focused() == ct) ? "yes" : "no", app_user_count());
    app_case("OK opens the chosen app", ct >= 0 && app_focused() == ct &&
             app_user_count() == 1, d);

    tap(0);                                               // UP: +1 inside the app
    pump(100);
    const int32_t v1 = ct >= 0 ? app_counter_value(app_get(ct)) : -1;

    press(5, INPUT_PRESS);                                // hold BACK
    press(5, INPUT_REPEAT);
    press(5, INPUT_RELEASE);
    pump(300);
    snprintf(d, sizeof(d), "value was %ld; now running: %s, focus %s, apps open %d",
             (long)v1, app_running(&app_counter) >= 0 ? "yes" : "no",
             app_focused() == app_home_id() ? "Home" : "an app", app_user_count());
    app_case("hold BACK closes it", v1 == 1 && app_running(&app_counter) < 0 &&
             app_focused() == app_home_id() && app_user_count() == 0, d);

    // Opening it again is a fresh start - nothing survived the close.
    press(4, INPUT_PRESS);                                // OK: same selection
    pump(300);
    ct = app_running(&app_counter);
    const int32_t v2 = ct >= 0 ? app_counter_value(app_get(ct)) : -1;
    snprintf(d, sizeof(d), "id %d, value %ld (was %ld before the close)", ct, (long)v2,
             (long)v1);
    app_case("reopened from scratch", ct >= 0 && v2 == 0, d);

    press(5, INPUT_PRESS);
    press(5, INPUT_REPEAT);
    press(5, INPUT_RELEASE);
    pump(300);
    app_case("closed again", app_user_count() == 0 && app_focused() == app_home_id(),
             app_user_count() == 0 ? "no apps open" : "an app is still open");
}


// Buttons are driven by injected events - there is no need for hands, and no
// way to get the timing of a real finger wrong. The manager cannot tell them
// from the sampler's: they arrive the same way, on the bus.
//
// Returns the id bounce ends up with, for the hog test that follows.
int app_model_test(int in, int ct, int bn)
{
    logln();
    logln("--- stage 4.3b: the app model, driven by script ---");
    char d[96];

    // 1. Starting apps does not take the screen from Home; unfocused apps
    //    never draw.
    pump(200);
    const app_ctx_t *I = app_get(in), *C = app_get(ct), *B = app_get(bn);
    snprintf(d, sizeof(d), "focus %s; frames drawn %lu / %lu / %lu",
             app_focused() == app_home_id() ? "Home" : "an app",
             (unsigned long)(I ? I->frames : 0), (unsigned long)(C ? C->frames : 0),
             (unsigned long)(B ? B->frames : 0));
    app_case("starting leaves Home focused", app_focused() == app_home_id() && I && C && B &&
             I->frames == 0 && C->frames == 0 && B->frames == 0, d);

    // 2. Focus by request.
    app_focus(ct);
    pump(100);
    snprintf(d, sizeof(d), "focus %d, counter drew %lu, input focused: %s",
             app_focused(), (unsigned long)C->frames, I->focused ? "yes" : "no");
    app_case("focus moves on request", app_focused() == ct && C->frames >= 1 &&
             C->focused && !I->focused, d);

    // 3. Input reaches the focused app and nobody else.
    const uint32_t in_before = app_input_inputs(I);
    tap(0); tap(0); tap(0);                              // UP x3
    pump(100);
    const int32_t counted = app_counter_value(C);
    snprintf(d, sizeof(d), "counter = %ld, input app got %lu new",
             (long)counted, (unsigned long)(app_input_inputs(I) - in_before));
    app_case("input goes to focus only", counted == 3 &&
             app_input_inputs(I) == in_before, d);

    // 4. Holding BACK closes the focused app and brings Home back. The press
    //    reaches the app; the repeat that makes it a hold, and the release,
    //    belong to the gesture. The counter's state is read above, BEFORE this:
    //    once it has closed, its memory is gone.
    const int users = app_user_count();
    press(5, INPUT_PRESS);
    press(5, INPUT_REPEAT);
    press(5, INPUT_REPEAT);
    press(5, INPUT_RELEASE);
    pump(200);
    snprintf(d, sizeof(d), "counter %s, focus %s, apps open %d -> %d, bounce got %lu",
             app_get(ct) ? "STILL RUNNING" : "closed",
             app_focused() == app_home_id() ? "Home" : "an app", users, app_user_count(),
             (unsigned long)app_bounce_inputs(B));
    app_case("hold BACK closes the app", !app_get(ct) && app_focused() == app_home_id() &&
             app_user_count() == users - 1 && app_bounce_inputs(B) == 0, d);

    // 5. Frame pacing. Bounce asks for 50 frames a second; every frame it
    //    draws must reach the panel, and none may be flushed twice.
    app_focus(bn);
    pump(200);
    const uint32_t f0 = app_frames_flushed(), b0 = B->frames;
    const int64_t  t0 = esp_timer_get_time();
    pump(1000);
    const uint32_t flushed = app_frames_flushed() - f0, drawn = B->frames - b0;
    const uint32_t ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
    snprintf(d, sizeof(d), "%lu frames in %lu ms (%lu drawn), %lu fps",
             (unsigned long)flushed, (unsigned long)ms, (unsigned long)drawn,
             (unsigned long)(flushed * 1000 / (ms ? ms : 1)));
    // Drawn and flushed may differ by one at either edge of the window: a
    // frame drawn just before it opened can be flushed inside it, and one
    // drawn just before it closed can still be on its way out. The first
    // version of this check allowed only the second, and failed on the first.
    const int32_t skew = (int32_t)drawn - (int32_t)flushed;
    app_case("frame pacing", flushed >= 45 && flushed <= 52 && skew >= -1 && skew <= 1, d);

    // 6. Stopping the focused app from the system side: it exits, its slot and
    //    task come back, and Home has the screen again.
    const int n0 = app_count();
    app_stop(bn);
    pump(200);
    snprintf(d, sizeof(d), "slot %s, focus %s, %d running",
             app_get(bn) ? "still taken" : "free",
             app_focused() == app_home_id() ? "Home" : "an app", app_count());
    app_case("stop the focused app", !app_get(bn) && app_focused() == app_home_id() &&
             app_count() == n0 - 1, d);

    // 7. And it starts again, in a free slot, from scratch.
    const int bn2 = app_start(&app_bounce);
    pump(100);
    snprintf(d, sizeof(d), "id %d, %d running", bn2, app_count());
    app_case("restart gets a slot", bn2 >= 0 && app_count() == n0, d);
    return bn2;
}


// The end of every test run: close whatever the tests opened, so the system
// is left the way a user would find it - Home, and nothing else running.
void close_user_apps(void)
{
    for (int i = 0; i < APP_MAX; ++i) {
        const app_ctx_t *c = app_get(i);
        if (c && !(c->app->flags & APP_FLAG_SYSTEM)) app_stop(i);
    }
    // One that cannot hear the request - the hog - is killed once the grace
    // runs out (stage 5.2), so wait that long if need be, not a fixed 300 ms.
    const int64_t t0 = esp_timer_get_time();
    while (app_user_count() && esp_timer_get_time() - t0 < (APP_STOP_GRACE_MS + 500) * 1000LL) {
        pump(20);
    }
    logf("  test apps closed: %d still open\n", app_user_count());
}

#ifdef SMARTOS_TEST_APPHOG
// An app that never yields, next to one that needs a steady 50 frames a
// second. Measured before and after the hog starts: bounce's frame rate and
// the input sampler's pass rate, which should not change.
void app_hog_test(int bn)
{
    logln();
    logln("--- stage 4.3b: an app that never yields ---");
    app_focus(bn);
    pump(200);

    uint32_t f0 = app_frames_flushed(), s0 = input_samples();
    pump(1000);
    const uint32_t fps_before = app_frames_flushed() - f0;
    const uint32_t smp_before = input_samples() - s0;

    const int hog = app_start(&app_hog);
    pump(200);
    f0 = app_frames_flushed(); s0 = input_samples();
    const uint32_t h0 = app_hog_spins();
    pump(1000);
    const uint32_t fps_after = app_frames_flushed() - f0;
    const uint32_t smp_after = input_samples() - s0;
    const uint32_t spins     = app_hog_spins() - h0;

    logf("  hog app id %d, spun %lu times in that second\n", hog, (unsigned long)spins);
    logf("  bounce frames/s  : %lu before, %lu with the hog %s\n",
         (unsigned long)fps_before, (unsigned long)fps_after,
         (fps_after + 3 >= fps_before) ? "OK" : "!! SLOWED");
    logf("  sampler passes/s : %lu before, %lu with the hog %s\n",
         (unsigned long)smp_before, (unsigned long)smp_after,
         (smp_after + 5 >= smp_before) ? "OK" : "!! SLOWED");
}
#endif // SMARTOS_TEST_APPHOG

#endif // SMARTOS_TEST_APPS
