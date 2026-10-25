// Home: the launcher. Part of the system, not an app - it is the one thing
// that runs at boot, and the only way an app gets started.
//
//   UP / DOWN   choose a row
//   OK          open the app, or go back into one that is already running
//
// Inside an app: hold BACK to close it, or hold DOWN and press BACK to
// minimise it (stage 5.5). A minimised app keeps running - its timers, its
// sensor streams, its recording - and appears here under "Running", where OK
// brings it back to the screen.
//
// Before an app is opened, the system checks there is room for it
// (app_can_start): slots, kernel memory, and the PSRAM the app declared it may
// need. If there is not, nothing is started and this says why.

#include "smartos_app.h"

#include "gfx/wallpaper.h"

#define LIST_Y     32
#define LIST_END   170        // the line under it says what the app is (5.4)
#define ROW_H      24
#define HEAD_H     13

enum { ROW_HEAD, ROW_RUNNING, ROW_APP };

typedef struct {
    uint8_t      kind;
    const char  *label;      // headers
    const app_t *app;
    int          id;         // running apps
    int          list_i;     // index among the user apps, for the saved choice
} row_t;

typedef struct {
    ui_list_t list;          // the chosen row and the first one drawn (ui/ui.h)
    bool      bg_drawn;      // the background on the screen now is the wanted one
    int       saved;         // the app-list index the settings file remembers
    char      msg[48];       // one line of feedback
} state_t;

// The user apps, in registry order - everything but the system's own.
static int list_apps(const app_t **out, int max)
{
    int n = 0;
    for (int i = 0; i < app_registry_count() && n < max; ++i) {
        const app_t *a = app_registry_at(i);
        if (!(a->flags & APP_FLAG_SYSTEM)) out[n++] = a;
    }
    return n;
}

// The rows as they are shown: what is running first, then everything that can
// be opened. Headers are rows too, so the list scrolls as one.
static int build_rows(row_t *rows, int max)
{
    int n = 0;
    for (int i = 0; i < APP_MAX && n < max; ++i) {
        const app_ctx_t *c = app_get(i);
        if (!c || (c->app->flags & APP_FLAG_SYSTEM)) continue;
        if (n == 0) rows[n++] = { ROW_HEAD, "Running", nullptr, -1, -1 };
        rows[n++] = { ROW_RUNNING, nullptr, c->app, i, -1 };
    }
    if (n && n < max) rows[n++] = { ROW_HEAD, "Apps", nullptr, -1, -1 };

    const app_t *list[APP_REGISTRY_MAX];
    const int apps = list_apps(list, APP_REGISTRY_MAX);
    for (int i = 0; i < apps && n < max; ++i) rows[n++] = { ROW_APP, nullptr, list[i], -1, i };
    return n;
}

// The list, as the toolkit sees it (stage 5.4c): headers are rows too, a
// different height, and cannot be chosen. Wrapping, skipping them and
// scrolling so the chosen row fits are ui_list_move's job now.
static int  row_h_of(void *rows, int i) { return ((const row_t *)rows)[i].kind == ROW_HEAD ? HEAD_H : ROW_H; }
static bool row_pick(void *rows, int i) { return ((const row_t *)rows)[i].kind != ROW_HEAD; }

static ui_rows_t list_of(const row_t *rows, int n)
{
    return { n, LIST_Y, LIST_END, ROW_H, row_h_of, row_pick, (void *)rows };
}

static void open_row(app_ctx_t *c, const row_t *r)
{
    state_t *s = (state_t *)c->user;
    s->msg[0] = 0;

    if (r->kind == ROW_RUNNING) {        // already open: just go back into it
        app_focus(r->id);
        return;
    }

    int id = app_running(r->app);
    if (id < 0) {
        const app_can_t can = app_can_start(r->app);
        if (can != APP_OK) {
            k_snprintf(s->msg, sizeof(s->msg), "%s: %s", r->app->name, app_can_why(can));
            app_invalidate(c);
            return;
        }
        id = app_start(r->app);
        if (id < 0) {
            k_snprintf(s->msg, sizeof(s->msg), "could not start %s", r->app->name);
            app_invalidate(c);
            return;
        }
    }
    // Remembered across reboots (stage 4.5) - written only now, when the user
    // commits to a choice, never on each UP or DOWN on the way to it.
    if (r->list_i >= 0 && r->list_i != s->saved) {
        app_setting_set(c, "sel", r->list_i);
        s->saved = r->list_i;
    }
    app_focus(id);
}

static void init(app_ctx_t *c)
{
    state_t *s = (state_t *)c->user;
    row_t rows[APP_MAX + APP_REGISTRY_MAX + 2];
    const int n = build_rows(rows, APP_MAX + APP_REGISTRY_MAX + 2);
    s->saved    = app_setting_get(c, "sel", 0);
    s->list.sel = 0;
    for (int i = 0; i < n; ++i) {         // back to the app chosen last time
        if (rows[i].kind == ROW_APP && rows[i].list_i == s->saved) { s->list.sel = i; break; }
    }
    const ui_rows_t list = list_of(rows, n);
    ui_list_move(&s->list, &list, 0);
}

static void on_event(app_ctx_t *c, const ev_t *ev)
{
    state_t *s = (state_t *)c->user;

    // A fresh list whenever focus arrives or an app opens or ends.
    if (ev->type == EV_SYS && (ev->a == SYS_FOCUS_GAINED || ev->a == SYS_APPS_CHANGED)) {
        app_invalidate(c);
        return;
    }
    if (ev->type == EV_SYS && ev->a == SYS_SETTING_CHANGED) {
        s->bg_drawn = false;         // the wallpaper may have been turned off
        app_invalidate(c);
        return;
    }
    if (ev->type != EV_INPUT) return;
    if (ev->a != INPUT_PRESS && ev->a != INPUT_REPEAT) return;

    row_t rows[APP_MAX + APP_REGISTRY_MAX + 2];
    const int n = build_rows(rows, APP_MAX + APP_REGISTRY_MAX + 2);
    if (n == 0) return;
    const ui_rows_t list = list_of(rows, n);
    ui_list_move(&s->list, &list, 0);      // the rows may have changed under it

    switch (1u << ev->b) {
        case BTN_UP:   ui_list_move(&s->list, &list, -1); break;
        case BTN_DOWN: ui_list_move(&s->list, &list, 1);  break;
        case BTN_OK:
            if (ev->a == INPUT_PRESS) open_row(c, &rows[s->list.sel]);
            return;
        default:       return;
    }
    app_invalidate(c);
}

// Stage 6.2: the wallpaper, unless the setting says otherwise. It lives in
// flash (src/gfx/wallpaper_data.cpp, made by tools/mkimage.py) and is read
// from there as it is drawn, so it costs no RAM at all.
static const image_t *wanted_bg(void)
{
    return settings_get("wallpaper") ? &g_wallpaper : nullptr;
}

static void draw(app_ctx_t *c, fb_t *fb, bool full)
{
    state_t *s = (state_t *)c->user;
    const image_t *bg_img = wanted_bg();

    // The runtime clears to the flat colour before a full draw; the picture
    // goes over it. A change to the setting repaints everything too - nothing
    // else can put a whole screen back.
    if (full || !s->bg_drawn) {
        if (image_ok(bg_img)) image_blit(fb, bg_img, 0, 0);
        else                  fb_fill_rect(fb, 0, 0, fb->w, fb->h, app_bg());
        s->bg_drawn = true;
        full = true;
    }

    // Over a picture every line of text gets a shadow, and the greys are
    // lifted: a name in FB_GREY over a bright sky cannot be read at all
    // (stage 6.2, seen on the first real wallpaper).
    const bool shade = image_ok(bg_img);

    if (full) {
        ui_title_on(fb, 6, "Home", shade);
        ui_help_on(fb, 196, "UP/DOWN choose   OK open", shade);
        ui_help_on(fb, 206, "in an app: hold BACK close, DOWN+BACK minimise", shade);
    }

    row_t rows[APP_MAX + APP_REGISTRY_MAX + 2];
    const int n = build_rows(rows, APP_MAX + APP_REGISTRY_MAX + 2);
    const ui_rows_t list = list_of(rows, n);

    for (ui_iter_t it = ui_list_begin(&s->list, &list); ui_list_next(&it); ) {
        const row_t *r = &rows[it.i];
        const bool here = (it.i == s->list.sel);
        ui_row_bg(fb, bg_img, it.y, it.h - 2, here);

        if (r->kind == ROW_HEAD) {
            ui_text(fb, 8, it.y + 2, r->label, FB_CYAN, 1, shade);
            fb_hline(fb, 8 + fb_text_width(r->label, 1) + 6, it.y + 6, fb->w - 24 -
                     fb_text_width(r->label, 1), fb_rgb(50, 60, 80));
        } else {
            const uint16_t dim_name = shade ? fb_rgb(205, 214, 226) : FB_GREY;
            ui_text(fb, 14, it.y + 1, r->app->name, here ? FB_WHITE : dim_name, 2, shade);
            if (r->kind == ROW_RUNNING) {
                const int f = app_focused();
                ui_text(fb, fb->w - 92, it.y + 7, f == r->id ? "in front" : "background",
                        FB_GREEN, 1, shade);
            } else if (app_running(r->app) >= 0) {
                ui_text(fb, fb->w - 92, it.y + 7, "running", FB_GREEN, 1, shade);
            }
        }
    }
    const int y = ui_list_bottom(&s->list, &list);
    ui_clear(fb, bg_img, 4, y, fb->w - 8, LIST_END - y);        // below the last row

    // What the chosen app is, in one line (stage 5.4).
    const int sel = s->list.sel;
    const char *about = (sel >= 0 && sel < n && rows[sel].kind != ROW_HEAD &&
                         rows[sel].app->about) ? rows[sel].app->about : "";
    ui_clear(fb, bg_img, 4, LIST_END + 4, fb->w - 8, TEXT_CELL_H + 2);
    ui_text(fb, 10, LIST_END + 5, about, FB_CYAN, 1, shade);

    ui_clear(fb, bg_img, 76, 10, fb->w - 80, TEXT_CELL_H);
    if (s->msg[0]) ui_text(fb, 76, 10, s->msg, shade ? fb_rgb(255, 120, 110) : FB_RED, 1, shade);
}

const app_t app_home = {
    .name      = "home",
    .about = "the launcher",
    .mask      = EV_MASK(EV_SYS),        // SYS_APPS_CHANGED, published to all
    .user_size = sizeof(state_t),
    .init      = init,
    .on_event  = on_event,
    .draw      = draw,
    .flags     = APP_FLAG_SYSTEM,
};
APP_REGISTER(app_home, 0);

// For the test script: which row is selected, and what it is.
int app_home_selected(const app_ctx_t *c)
{
    return c && c->user ? ((const state_t *)c->user)->list.sel : -1;
}

const char *app_home_selected_name(const app_ctx_t *c)
{
    if (!c || !c->user) return "";
    row_t rows[APP_MAX + APP_REGISTRY_MAX + 2];
    const int n = build_rows(rows, APP_MAX + APP_REGISTRY_MAX + 2);
    const int sel = ((const state_t *)c->user)->list.sel;
    if (sel < 0 || sel >= n || rows[sel].kind == ROW_HEAD) return "";
    return rows[sel].app->name;
}

const char *app_home_message(const app_ctx_t *c)
{
    return c && c->user ? ((const state_t *)c->user)->msg : "";
}
