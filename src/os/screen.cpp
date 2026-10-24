#include "os/screen.h"

#include "esp_attr.h"

#include "kernel/kmbox.h"
#include "kernel/ktime.h"
#include "os/app.h"
#include "os/evbus.h"
#include "os/settings.h"
#include "os/syslog.h"
#include "s3hal/lcd_panel.h"

enum { SCREEN_JOB = KMBOX_OP_EXT + 16 };   // settings has +14

static volatile DRAM_ATTR uint32_t s_off;        // 1 while the backlight is down
static DRAM_ATTR uint32_t          s_offs;       // times it has gone off
static ev_t                        s_queue[8];
static int                         s_sub = -1;
static uint32_t                    s_last_ms;

static void backlight_on(void)
{
    lcd_backlight_pct((uint8_t)settings_get("backlight"));
}

// Buttons pressed: the idle clock starts again. This does NOT turn the
// backlight on. Core 0 sees the same presses as core 1, and if it woke the
// screen itself the app manager would no longer see the screen as off - so it
// would pass that press to the app as well as waking. (Seen on the first run:
// the counter went up by one when the press was only meant to wake it.)
static void touch(void) { s_last_ms = k_millis(); }

// Core 1 asks for this, through the mailbox, when a press arrives with the
// screen off - and swallows that press itself.
static void wake(void)
{
    touch();
    if (!s_off) return;
    backlight_on();
    s_off = 0;
    // Stage 6.4d: whatever was drawn while the panel was dark never reached it,
    // so ask the app in front for one more frame. The manager does the work on
    // core 1, where the flag it sets belongs.
    app_redraw_focused();
    slog("sys", "screen on");
}

static uint32_t h_screen(const k_job_t *, uint32_t *)
{
    wake();
    return 0;
}

void screen_init(void)
{
    s_sub     = evbus_subscribe("screen", EV_MASK(EV_INPUT), EV_Q(s_queue));
    s_last_ms = k_millis();
    kmbox_register(SCREEN_JOB, h_screen);
}

void screen_poll(void)
{
    // Any button, from the sampler or the shell, counts as being used.
    ev_t ev;
    bool used = false;
    while (evbus_try_recv(s_sub, &ev)) used = true;
    if (used) touch();

    const int32_t secs = settings_get("screenoff");
    if (secs <= 0 || s_off) return;
    if ((int32_t)(k_millis() - s_last_ms) < secs * 1000) return;

    lcd_backlight_pct(0);
    s_off = 1;
    s_offs++;
    slog("sys", "screen off after %ld s idle", (long)secs);
}

IRAM_ATTR bool screen_is_off(void) { return s_off != 0; }

void screen_wake_k(void)
{
    uint32_t r = 0;
    kmbox_call_timeout(SCREEN_JOB, 0, 0, 0, &r, 0x7FFFFFFFu);
}

uint32_t screen_offs(void) { return s_offs; }
