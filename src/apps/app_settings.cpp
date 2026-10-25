// Settings: the system's own settings, and a look at the system.
//
//   UP / DOWN     choose a row
//   LEFT / RIGHT  change the chosen setting (brightness: 10 % a step)
//   OK            on "clear the system log": do it
//
// A brightness change is visible at once - the hook runs on core 0 as the
// setting changes - and reaches flash once it has been left alone for a
// second (os/settings.h). The information rows are read fresh each time the
// app is opened and on a 2 s timer.

#include "apps/apps.h"

#include "smartos_app.h"

enum { ROW_BRIGHT, ROW_CLEAR, ROWS };

#define ROW_Y     34         // the first row
#define ROW_PITCH 28         // top to top
#define ROW_H     24         // the highlight's height

typedef struct {
    ui_list_t    list;       // the chosen row (ui/ui.h)
    ui_confirm_t confirm;    // "OK again" for the row below (5.4)
    uint32_t     fs_total, fs_used;
    char         msg[40];
} state_t;

static ui_rows_t rows_of(void)
{
    return { ROWS, ROW_Y, ROW_Y + ROWS * ROW_PITCH, ROW_PITCH, nullptr, nullptr, nullptr };
}

static void refresh(app_ctx_t *c)
{
    state_t *s = (state_t *)c->user;
    fs_usage(c, &s->fs_total, &s->fs_used);
    app_invalidate(c);
}

static void init(app_ctx_t *c)
{
    refresh(c);
    app_timer(c, 2000, 1);
}

static void on_event(app_ctx_t *c, const ev_t *ev)
{
    state_t *s = (state_t *)c->user;

    if (ev->type == EV_TIMER) { refresh(c); return; }
    if (ev->type == EV_SYS && ev->a == SYS_SETTING_CHANGED) { app_invalidate(c); return; }
    if (ev->type != EV_INPUT || (ev->a != INPUT_PRESS && ev->a != INPUT_REPEAT)) return;

    const uint32_t btn = 1u << ev->b;
    const ui_rows_t rows = rows_of();
    if (btn == BTN_UP)   ui_list_move(&s->list, &rows, -1);
    if (btn == BTN_DOWN) ui_list_move(&s->list, &rows, 1);

    if (s->list.sel == ROW_BRIGHT && (btn == BTN_LEFT || btn == BTN_RIGHT)) {
        const int32_t now = settings_get("backlight");
        settings_set_k("backlight", now + (btn == BTN_RIGHT ? 10 : -10));
    }
    // Clearing the log cannot be undone, so it takes two presses: the first
    // only arms it and says so, and any other key cancels (stage 5.4, on the
    // toolkit's ui_confirm since 5.4c).
    if (s->list.sel == ROW_CLEAR && btn == BTN_OK && ev->a == INPUT_PRESS) {
        if (ui_confirm(&s->confirm, ROW_CLEAR, "OK", "clear the log", s->msg, sizeof(s->msg))) {
            slog_clear();
            k_snprintf(s->msg, sizeof(s->msg), "system log cleared");
            refresh(c);
        }
    } else {
        ui_confirm_cancel(&s->confirm, s->msg, sizeof(s->msg), "not cleared");
    }
    app_invalidate(c);
}

static void draw(app_ctx_t *c, fb_t *fb, bool full)
{
    state_t *s = (state_t *)c->user;
    const uint16_t bg = app_bg();
    char line[64];

    if (full) ui_title(fb, 6, "Settings");

    // The settings, one row each; the chosen row is highlighted.
    const int32_t bright = settings_get("backlight");
    int32_t lo = 0, hi = 100;
    settings_range("backlight", &lo, &hi);
    const int y0 = ROW_Y, y1 = ROW_Y + ROW_PITCH;
    ui_row_bg(fb, nullptr, y0, ROW_H, s->list.sel == ROW_BRIGHT);
    // Label and value apart, so neither runs into the bar at the right.
    fb_text(fb, 12, y0 + 4, "Brightness", FB_WHITE, 2);
    k_snprintf(line, sizeof(line), "%3ld%%", (long)bright);
    fb_text(fb, 138, y0 + 4, line, FB_YELLOW, 2);
    // A bar, so the value reads at a glance.
    const int bw = 100, bx = fb->w - bw - 16;
    ui_bar(fb, bx, y0 + 6, bw, 12, bright, lo, hi, FB_GREY, FB_YELLOW, bg);

    ui_row_bg(fb, nullptr, y1, ROW_H, s->list.sel == ROW_CLEAR);
    fb_text(fb, 12, y1 + 4, "Clear the system log", FB_WHITE, 2);

    // Read-only: what the system is standing on.
    fb_hline(fb, 0, 96, fb->w, fb_rgb(60, 60, 80));
    k_snprintf(line, sizeof(line), "boot          #%lu", (unsigned long)fs_boot_number());
    fb_text_bg(fb, 12, 104, line, FB_GREY, bg, 1);
    k_snprintf(line, sizeof(line), "storage       %lu KB used of %lu KB",
               (unsigned long)(s->fs_used / 1024), (unsigned long)(s->fs_total / 1024));
    fb_text_bg(fb, 12, 116, line, FB_GREY, bg, 1);
    k_snprintf(line, sizeof(line), "apps open     %d", app_user_count());
    fb_text_bg(fb, 12, 128, line, FB_GREY, bg, 1);

    fb_text_bg(fb, 12, 150, s->msg, s->confirm.armed ? FB_YELLOW : FB_GREEN, bg, 1);
    ui_help(fb, 204, "UP/DOWN row   LEFT/RIGHT change   OK do");
}

const app_t app_settings = {
    .name      = "settings",
    .about = "brightness, the log, and this board",
    .mask      = EV_MASK(EV_SYS),     // hears SYS_SETTING_CHANGED from anyone
    .user_size = sizeof(state_t),
    .init      = init,
    .on_event  = on_event,
    .draw      = draw,
};
APP_REGISTER(app_settings, 70);

// For the test script.
const char *app_settings_message(const app_ctx_t *c)
{
    return c && c->user ? ((const state_t *)c->user)->msg : "";
}
