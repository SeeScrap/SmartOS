// Tasks: what is running, and what it costs (stage 5.3).
//
// Two pages, LEFT switches between them.
//
// Processes:
//   UP / DOWN   choose an app
//   OK          ask it to close - it is killed if it has not in 2 s
//   RIGHT       twice: kill it now (the first press only arms it)
//
// Performance (stage 5.3b), laid out like the Performance tab of the Windows
// Task Manager: CPU, Memory and Flash down the left, each with a small graph;
// UP / DOWN picks one, and the right shows its last 60 seconds large, with its
// details under the graph.
//
// Refreshed once a second. CPU is each task's share of core 1 over the last
// second, from the kernel's own accounting (k_task_snapshot / k_cpu_time):
// the core's time splits into busy, idle and parked - held while core 0 wrote
// flash - and busy splits into tasks. Core 0 is not shown: it is FreeRTOS's,
// and switches tasks the kernel never sees.
//
// Stack is the high-water mark: the deepest the task has ever gone, of the
// 2 KB it has. "in" is input events received; "drop" events its queue had no
// room for.

#include "apps/apps.h"

#include <string.h>

#include "gfx/text.h"
#include "kernel/kheap.h"
#include "kernel/ktask.h"
#include "os/kfmt.h"
#include "os/sysinfo.h"
#include "s3hal/input.h"
#include "ui/ui.h"

#define ROWS_Y     64
#define ROW_H      12
#define MAX_ROWS   APP_MAX
#define HIST       60                       // seconds of history in the graphs

enum { PAGE_PROCESSES, PAGE_PERF, PAGES };
enum { RES_CPU, RES_KERNEL, RES_RAM, RES_PSRAM, RES_FLASH, RESOURCES };

typedef struct {
    k_task_info_t info[K_MAX_TASKS];
    int           n;
    uint64_t      prev_cycles[K_MAX_TASKS];
    uint32_t      prev_gen[K_MAX_TASKS];
    uint16_t      pm[K_MAX_TASKS];          // per mille of core 1, last second
    k_cpu_time_t  prev_core;
    uint16_t      busy_pm, idle_pm, park_pm;
    bool          have_prev;
    ui_list_t     apps;                     // the chosen open app, in id order
    ui_confirm_t  confirm;                  // RIGHT pressed once on an app (ui/ui.h)
    char          msg[54];

    // Performance page.
    int           page, drawn_page;
    ui_list_t     res;                      // the chosen resource, RES_*
    uint16_t      hist[RESOURCES][HIST];    // per mille, oldest first once full
    int           hist_n;                   // samples held, up to HIST
    uint32_t      switches_prev, switches_per_s;
    uint32_t      flash_ops_prev, flash_ops_per_s;
    sysinfo_t     sys;
    kheap_stats_t kin, kps;                 // the kernel's two pools (stage 5.3c)
    uint32_t      stack_bytes, app_bytes;   // of kin: task stacks, apps' state
} state_t;

// The two lists on these pages (stage 5.4c). Neither scrolls: both are drawn
// whole, so the box is exactly as tall as the rows in it.
static ui_rows_t app_rows(int n)
{
    return { n, ROWS_Y, ROWS_Y + MAX_ROWS * ROW_H, ROW_H, nullptr, nullptr, nullptr };
}

static ui_rows_t res_rows(void)
{
    return { RESOURCES, 24, 24 + RESOURCES * 36, 36, nullptr, nullptr, nullptr };
}

static uint16_t per_mille(uint64_t part, uint64_t whole)
{
    return whole ? (uint16_t)(part * 1000 / whole) : 0;
}

static void sample(app_ctx_t *c)
{
    state_t *s = (state_t *)c->user;
    k_cpu_time_t now;
    k_cpu_time(1, &now);
    s->n = k_task_snapshot(s->info, K_MAX_TASKS);

    const uint64_t busy = now.busy - s->prev_core.busy;
    const uint64_t idle = now.idle - s->prev_core.idle;
    const uint64_t park = now.park - s->prev_core.park;
    const uint64_t all  = busy + idle + park;

    for (int i = 0; i < K_MAX_TASKS; ++i) s->pm[i] = 0;
    for (int i = 0; i < s->n; ++i) {
        const k_task_info_t *t = &s->info[i];
        // A slot reused since the last sample starts again from zero.
        const uint64_t before = (s->prev_gen[t->id] == t->gen) ? s->prev_cycles[t->id] : 0;
        if (s->have_prev) s->pm[t->id] = per_mille(t->cycles - before, all);
        s->prev_cycles[t->id] = t->cycles;
        s->prev_gen[t->id]    = t->gen;
    }
    if (s->have_prev) {
        s->busy_pm = per_mille(busy, all);
        s->idle_pm = per_mille(idle, all);
        s->park_pm = per_mille(park, all);
    }
    s->prev_core = now;

    // The performance history: one sample a second, the oldest dropped.
    sysinfo_get(&s->sys);
    kheap_stats_pool(KHEAP_INTERNAL, &s->kin);
    kheap_stats_pool(KHEAP_PSRAM, &s->kps);
    int with_stack = 0;
    for (int i = 0; i < s->n; ++i) if (s->info[i].state != K_DEAD) with_stack++;
    s->stack_bytes = (uint32_t)with_stack * (K_STACK_SIZE + K_STACK_GUARD);
    s->app_bytes = 0;
    for (int id = 0; id < APP_MAX; ++id) {
        s->app_bytes += (uint32_t)kheap_owner_bytes(KHEAP_INTERNAL, (uint16_t)(id + 1));
    }
    const uint32_t sw = k_cpu()->switches;             // this app is on core 1
    if (s->have_prev) {
        s->switches_per_s  = sw - s->switches_prev;
        s->flash_ops_per_s = s->sys.flash_ops - s->flash_ops_prev;
        // "In use" for RAM and PSRAM leaves out the kernel pools' spare room:
        // taken from ESP-IDF, but not holding anything yet.
        const uint32_t ram_use = s->sys.ram_total - s->sys.ram_free - (uint32_t)s->kin.free_bytes;
        const uint32_t ps_use  = s->sys.psram_total - s->sys.psram_free - (uint32_t)s->kps.free_bytes;
        const uint16_t v[RESOURCES] = {
            s->busy_pm,
            per_mille(s->kin.used, s->kin.total),
            per_mille(ram_use, s->sys.ram_total),
            per_mille(ps_use, s->sys.psram_total),
            s->park_pm,
        };
        if (s->hist_n == HIST) {
            for (int r = 0; r < RESOURCES; ++r) memmove(s->hist[r], s->hist[r] + 1, (HIST - 1) * 2);
            s->hist_n--;
        }
        for (int r = 0; r < RESOURCES; ++r) s->hist[r][s->hist_n] = v[r];
        s->hist_n++;
    }
    s->switches_prev  = sw;
    s->flash_ops_prev = s->sys.flash_ops;
    s->have_prev = true;
    app_invalidate(c);
}

static const k_task_info_t *task_of(const state_t *s, const app_ctx_t *a)
{
    if (!a || a->task < 0) return nullptr;
    for (int i = 0; i < s->n; ++i) {
        if (s->info[i].id == a->task && s->info[i].gen == a->task_gen) return &s->info[i];
    }
    return nullptr;
}

// The open apps, in id order. Returns how many.
static int open_apps(int *ids)
{
    int n = 0;
    for (int i = 0; i < APP_MAX; ++i) if (app_get(i)) ids[n++] = i;
    return n;
}

static void init(app_ctx_t *c)
{
    // The state is zeroed for us, and zero is right for both lists and the
    // confirmation: first app, CPU, nothing armed.
    sample(c);
    app_timer(c, 1000, 1);
}

static void on_event(app_ctx_t *c, const ev_t *ev)
{
    state_t *s = (state_t *)c->user;
    if (ev->type == EV_TIMER) { sample(c); return; }
    if (ev->type != EV_INPUT || ev->a != INPUT_PRESS) return;
    const uint32_t btn = 1u << ev->b;

    if (btn == BTN_LEFT) {                          // the other page
        s->page = (s->page + 1) % PAGES;
        ui_confirm_cancel(&s->confirm, nullptr, 0, nullptr);
        s->msg[0] = 0;
        app_invalidate(c);
        return;
    }
    if (s->page == PAGE_PERF) {                     // nothing here closes anything
        const ui_rows_t res = res_rows();
        if (btn == BTN_UP)   ui_list_move(&s->res, &res, -1);
        if (btn == BTN_DOWN) ui_list_move(&s->res, &res, 1);
        app_invalidate(c);
        return;
    }

    int ids[APP_MAX];
    const int n = open_apps(ids);
    const ui_rows_t rows = app_rows(n);
    ui_list_move(&s->apps, &rows, 0);               // apps come and go under it

    if (btn == BTN_UP && n)   ui_list_move(&s->apps, &rows, -1);
    if (btn == BTN_DOWN && n) ui_list_move(&s->apps, &rows, 1);

    const int id = n ? ids[s->apps.sel] : -1;
    const app_ctx_t *a = app_get(id);
    const bool system = a && (a->app->flags & APP_FLAG_SYSTEM);

    if (btn == BTN_OK && a) {
        if (system) k_snprintf(s->msg, sizeof(s->msg), "%s is part of the system", a->app->name);
        else {
            app_stop(id);
            k_snprintf(s->msg, sizeof(s->msg), "asked %s to close", a->app->name);
        }
    }
    if (btn == BTN_RIGHT && a) {
        if (system) {
            k_snprintf(s->msg, sizeof(s->msg), "%s is part of the system", a->app->name);
        } else {
            char what[32];
            k_snprintf(what, sizeof(what), "kill %s", a->app->name);
            if (ui_confirm(&s->confirm, id, "RIGHT", what, s->msg, sizeof(s->msg))) {
                app_kill_request(id);
                k_snprintf(s->msg, sizeof(s->msg), "killed %s", a->app->name);
            }
        }
    } else if (btn == BTN_OK) {
        ui_confirm_cancel(&s->confirm, nullptr, 0, nullptr);     // OK's own line stands
    } else {
        ui_confirm_cancel(&s->confirm, s->msg, sizeof(s->msg), nullptr);
    }
    app_invalidate(c);
}

static void pct(char *out, size_t n, uint16_t pm)
{
    k_snprintf(out, n, "%3u.%u%%", (unsigned)(pm / 10), (unsigned)(pm % 10));
}

static void draw_processes(app_ctx_t *c, fb_t *fb, bool full)
{
    state_t *s = (state_t *)c->user;
    const uint16_t bg = app_bg();
    char line[64], a1[12], a2[12], a3[12];

    if (full) ui_title(fb, 4, "Tasks");
    if (full) fb_text(fb, 140, 8, "processes | LEFT: performance", FB_GREY, 1);

    pct(a1, sizeof(a1), s->busy_pm);
    pct(a2, sizeof(a2), s->park_pm);
    pct(a3, sizeof(a3), s->idle_pm);
    k_snprintf(line, sizeof(line), "core 1  busy %s  parked %s  idle %s", a1, a2, a3);
    fb_text_bg(fb, 8, 26, line, FB_WHITE, bg, 1);
    kheap_stats_t h;
    kheap_stats(&h);
    k_snprintf(line, sizeof(line), "kernel heap  %lu of %lu bytes used      ",
               (unsigned long)h.used, (unsigned long)h.total);
    fb_text_bg(fb, 8, 38, line, FB_GREY, bg, 1);

    fb_text_bg(fb, 8, 52, "app            cpu     stack     in   drop", FB_CYAN, bg, 1);

    int ids[APP_MAX];
    const int n = open_apps(ids);
    for (int r = 0; r < MAX_ROWS; ++r) {
        const int y = ROWS_Y + r * ROW_H;
        const app_ctx_t *a = (r < n) ? app_get(ids[r]) : nullptr;
        ui_row_bg(fb, nullptr, y - 2, ROW_H, a && r == s->apps.sel);
        if (!a) continue;
        const k_task_info_t *t = task_of(s, a);
        pct(a1, sizeof(a1), t ? s->pm[t->id] : 0);
        const evbus_stats_t st = evbus_stats(a->sub);
        const char *mark = a->killed ? " killed" : (a->stop_ms ? " closing" : "");
        k_snprintf(line, sizeof(line), "%-12.12s %s  %4lu/%lu  %5lu %6lu%s", a->app->name, a1,
                   (unsigned long)(t ? t->stack_used : 0), (unsigned long)(t ? t->stack_size : 0),
                   (unsigned long)a->inputs, (unsigned long)st.dropped, mark);
        fb_text(fb, 8, y, line, (a->app->flags & APP_FLAG_SYSTEM) ? FB_GREY : FB_WHITE, 1);
    }

    // The kernel's own tasks on core 1 - the ones that are not apps.
    int y = ROWS_Y + MAX_ROWS * ROW_H + 4;
    fb_fill_rect(fb, 4, y, fb->w - 8, 12 * 3, bg);
    int col = 0;
    for (int i = 0; i < s->n && y < 190; ++i) {
        const k_task_info_t *t = &s->info[i];
        if (t->affinity != 1 || t->state == K_DEAD || t->state == K_ZOMBIE) continue;
        bool is_app = false;
        for (int k = 0; k < n; ++k) {
            const app_ctx_t *a = app_get(ids[k]);
            if (a && a->task == t->id && a->task_gen == t->gen) is_app = true;
        }
        if (is_app) continue;
        pct(a1, sizeof(a1), s->pm[t->id]);
        k_snprintf(line, sizeof(line), "%-9.9s%s", t->name ? t->name : "?", a1);
        fb_text(fb, 8 + col * 104, y, line, FB_GREY, 1);
        if (++col == 3) { col = 0; y += 12; }
    }

    fb_fill_rect(fb, 4, 190, fb->w - 8, 10, bg);
    fb_text(fb, 8, 190, s->msg, s->confirm.armed ? FB_YELLOW : FB_GREEN, 1);
    if (full) ui_help(fb, 204, "UP/DOWN app  OK close  RIGHT x2 kill  LEFT page");
}

// --- the performance page ------------------------------------------------------------

typedef struct {
    const char *name, *what;
    uint16_t    line, fill;
} res_look_t;

// Colours after the Windows Task Manager: CPU blue, memory purple (a shade per
// kind here), disk - flash - green.
static res_look_t look(int r)
{
    switch (r) {
        case RES_CPU:    return { "CPU",    "core 1",           fb_rgb(17, 125, 187), fb_rgb(10, 42, 66) };
        case RES_KERNEL: return { "Kernel", "kernel pool, 128 KB", fb_rgb(160, 60, 200), fb_rgb(48, 20, 62) };
        case RES_RAM:    return { "RAM",    "internal RAM",     fb_rgb(210, 90, 170), fb_rgb(62, 26, 50) };
        case RES_PSRAM:  return { "PSRAM",  "8 MB, external",   fb_rgb(120, 110, 230), fb_rgb(34, 30, 70) };
        default:         return { "Flash",  "flash busy",       fb_rgb(90, 180, 30), fb_rgb(24, 58, 10) };
    }
}

// A history graph, newest at the right edge, like Windows: HIST seconds across
// the whole width whether or not that many have been sampled yet. Values are
// per mille of the full height.
static void graph(fb_t *fb, int x, int y, int w, int h, const uint16_t *v, int n,
                  const res_look_t *lk, bool grid)
{
    const uint16_t back = fb_rgb(6, 10, 16), lines = fb_rgb(28, 40, 52);
    fb_fill_rect(fb, x, y, w, h, back);
    if (grid) {
        for (int q = 1; q < 4; ++q) fb_hline(fb, x + 1, y + h * q / 4, w - 2, lines);
        for (int t = 1; t < 6; ++t) fb_vline(fb, x + w * t / 6, y + 1, h - 2, lines);
    }
    fb_rect(fb, x, y, w, h, lk->line);
    if (n < 1) return;

    int prev_y = -1;
    for (int px = 1; px < w - 1; ++px) {
        // Where this column falls in the HIST-sample window, in 1/256ths.
        const int pos = (px * (HIST - 1) * 256) / (w - 1);
        const int i   = (pos >> 8) - (HIST - n);
        if (i < 0) continue;
        const int frac = pos & 255;
        const int a = v[i], b = (i + 1 < n) ? v[i + 1] : v[i];
        int val = a + (b - a) * frac / 256;
        if (val > 1000) val = 1000;
        const int yv = y + h - 2 - val * (h - 3) / 1000;
        fb_vline(fb, x + px, yv, y + h - 1 - yv, lk->fill);
        if (prev_y >= 0) fb_line(fb, x + px - 1, prev_y, x + px, yv, lk->line);
        else             fb_pixel(fb, x + px, yv, lk->line);
        prev_y = yv;
    }
}

// "12.3 KB" or "3.9 MB".
static void size_str(char *out, size_t n, uint32_t bytes)
{
    if (bytes >= 1024u * 1024u) {
        const uint32_t t = (uint32_t)((uint64_t)bytes * 10 / (1024u * 1024u));
        k_snprintf(out, n, "%lu.%lu MB", (unsigned long)(t / 10), (unsigned long)(t % 10));
    } else {
        const uint32_t t = bytes * 10u / 1024u;
        k_snprintf(out, n, "%lu.%lu KB", (unsigned long)(t / 10), (unsigned long)(t % 10));
    }
}

typedef struct {
    const char *label;
    uint32_t    bytes;
    uint16_t    colour;
} seg_t;

// What one memory is made of, as Windows draws "memory composition": a bar
// split in proportion, and a key under it. Four parts at most.
static int composition(const state_t *s, int r, seg_t *seg, uint32_t *total)
{
    const uint16_t free_c = fb_rgb(44, 48, 58), spare_c = fb_rgb(84, 60, 104);
    int n = 0;
    if (r == RES_KERNEL) {
        const uint32_t used  = (uint32_t)s->kin.used;
        const uint32_t apps  = s->app_bytes;
        const uint32_t stk   = s->stack_bytes;
        const uint32_t other = used > apps + stk ? used - apps - stk : 0;
        *total = (uint32_t)s->kin.total;
        seg[n++] = { "task stacks", stk,   fb_rgb(160, 60, 200) };
        seg[n++] = { "apps' state", apps,  fb_rgb(230, 130, 210) };
        seg[n++] = { "kernel",      other, fb_rgb(110, 90, 140) };
        seg[n++] = { "free",        (uint32_t)s->kin.free_bytes, free_c };
    } else if (r == RES_RAM) {
        const uint32_t t = s->sys.ram_total, f = s->sys.ram_free;
        const uint32_t pool = KHEAP_INTERNAL_SIZE;
        const uint32_t sys = t > f + pool ? t - f - pool : 0;
        *total = t;
        seg[n++] = { "kernel pool, used",  (uint32_t)s->kin.used,       fb_rgb(210, 90, 170) };
        seg[n++] = { "kernel pool, spare", (uint32_t)s->kin.free_bytes, spare_c };
        seg[n++] = { "ESP-IDF, core 0",    sys,                         fb_rgb(120, 70, 110) };
        seg[n++] = { "free",          f,                           free_c };
    } else {
        const uint32_t t = s->sys.psram_total, f = s->sys.psram_free;
        const uint32_t pool = KHEAP_PSRAM_SIZE;
        const uint32_t sys = t > f + pool ? t - f - pool : 0;
        *total = t;
        seg[n++] = { "apps (app_alloc)",   (uint32_t)s->kps.used,       fb_rgb(120, 110, 230) };
        seg[n++] = { "kernel pool, spare", (uint32_t)s->kps.free_bytes, spare_c };
        seg[n++] = { "screen, ESP-IDF",    sys,                         fb_rgb(70, 70, 130) };
        seg[n++] = { "free",          f,                           free_c };
    }
    return n;
}

static void draw_composition(fb_t *fb, int x, int y, int w, const seg_t *seg, int n, uint32_t total)
{
    // The bar, each part at least a pixel wide if it is there at all.
    int at = x;
    for (int i = 0; i < n; ++i) {
        int pw = total ? (int)((uint64_t)seg[i].bytes * w / total) : 0;
        if (seg[i].bytes && pw == 0) pw = 1;
        if (i == n - 1 || at + pw > x + w) pw = x + w - at;
        if (pw > 0) fb_fill_rect(fb, at, y, pw, 12, seg[i].colour);
        at += pw;
    }
    fb_rect(fb, x, y, w, 12, fb_rgb(90, 90, 110));

    // The key, one line each: the colour, what it is, and how much - the size
    // right-aligned, so the column of numbers lines up. (Two columns side by
    // side were tried first; at 105 px each the labels ran into each other,
    // seen in the stage 5.3c screenshots.)
    char sz[16];
    for (int i = 0; i < n; ++i) {
        const int ky = y + 16 + i * 11;
        fb_fill_rect(fb, x, ky + 1, 6, 6, seg[i].colour);
        fb_text(fb, x + 9, ky, seg[i].label, FB_GREY, 1);
        size_str(sz, sizeof(sz), seg[i].bytes);
        fb_text(fb, x + w - fb_text_width(sz, 1), ky, sz, FB_GREY, 1);
    }
}

static void draw_perf(app_ctx_t *c, fb_t *fb, bool full)
{
    state_t *s = (state_t *)c->user;
    const uint16_t bg = app_bg();
    char line[64], v1[16], v2[16];

    if (full) {
        ui_title(fb, 4, "Tasks");
        fb_text(fb, 140, 8, "performance | LEFT: processes", FB_GREY, 1);
        ui_help(fb, 206, "UP/DOWN pick one   LEFT page");
    }

    // Down the left: each resource, its value now and a small graph.
    for (int r = 0; r < RESOURCES; ++r) {
        const res_look_t lk = look(r);
        const int x = 4, y = 24 + r * 36, w = 96, h = 34;
        fb_fill_rect(fb, x, y, w, h, bg);
        fb_rect(fb, x, y, w, h, r == s->res.sel ? lk.line : fb_rgb(40, 48, 60));
        fb_text(fb, x + 5, y + 4, lk.name, FB_WHITE, 1);
        const uint16_t now = s->hist_n ? s->hist[r][s->hist_n - 1] : 0;
        pct(v1, sizeof(v1), now);
        fb_text(fb, x + w - 5 - fb_text_width(v1, 1), y + 4, v1, lk.line, 1);
        graph(fb, x + 5, y + 14, w - 10, h - 18, s->hist[r], s->hist_n, &lk, false);
    }

    // On the right, the chosen one, large. Memory gets a shorter graph, to
    // leave room for what it is made of.
    const res_look_t lk = look(s->res.sel);
    const bool memory = (s->res.sel == RES_KERNEL || s->res.sel == RES_RAM || s->res.sel == RES_PSRAM);
    const int gx = 106, gy = 48, gw = 210, gh = memory ? 64 : 96;
    fb_fill_rect(fb, gx, 22, gw, 202 - 22, bg);
    fb_text(fb, gx, 26, lk.name, FB_WHITE, 2);
    fb_text(fb, gx + gw - fb_text_width(lk.what, 1), 26, lk.what, FB_GREY, 1);
    fb_text(fb, gx + gw - fb_text_width("100%", 1), 38, "100%", FB_GREY, 1);
    graph(fb, gx, gy, gw, gh, s->hist[s->res.sel], s->hist_n, &lk, true);
    fb_text(fb, gx, gy + gh + 2, "60 seconds", FB_GREY, 1);
    fb_text(fb, gx + gw - fb_text_width("0", 1), gy + gh + 2, "0", FB_GREY, 1);
    const int sy = gy + gh + 14, cx2 = gx + 108;

    if (memory) {
        seg_t seg[4];
        uint32_t total = 0;
        const int n = composition(s, s->res.sel, seg, &total);
        draw_composition(fb, gx, sy, gw, seg, n, total);
        const kheap_stats_t *pool = (s->res.sel == RES_PSRAM) ? &s->kps : &s->kin;
        if (s->res.sel == RES_KERNEL || s->res.sel == RES_PSRAM) {
            size_str(v1, sizeof(v1), (uint32_t)pool->largest_free);
            k_snprintf(line, sizeof(line), "largest free block %s", v1);
        } else {
            size_str(v1, sizeof(v1), s->sys.ram_largest);
            size_str(v2, sizeof(v2), s->sys.ram_min);
            k_snprintf(line, sizeof(line), "biggest free %s, low %s", v1, v2);
        }
        fb_text(fb, gx, sy + 16 + n * 11 + 2, line, FB_WHITE, 1);
        return;
    }

    const uint32_t up_s = s->sys.uptime_ms / 1000;
    int tasks = 0;
    for (int i = 0; i < s->n; ++i) {
        if (s->info[i].state != K_DEAD && s->info[i].state != K_ZOMBIE) tasks++;
    }
    if (s->res.sel == RES_CPU) {
        pct(v1, sizeof(v1), s->busy_pm);
        k_snprintf(line, sizeof(line), "in use %s", v1);
        fb_text(fb, gx, sy, line, FB_WHITE, 1);
        pct(v1, sizeof(v1), s->idle_pm);
        k_snprintf(line, sizeof(line), "idle   %s", v1);
        fb_text(fb, cx2, sy, line, FB_GREY, 1);
        k_snprintf(line, sizeof(line), "tasks %d  apps %d", tasks, app_count());
        fb_text(fb, gx, sy + 12, line, FB_GREY, 1);
        k_snprintf(line, sizeof(line), "switch/s %lu", (unsigned long)s->switches_per_s);
        fb_text(fb, cx2, sy + 12, line, FB_GREY, 1);
        k_snprintf(line, sizeof(line), "up %lu:%02lu:%02lu", (unsigned long)(up_s / 3600),
                   (unsigned long)(up_s / 60 % 60), (unsigned long)(up_s % 60));
        fb_text(fb, gx, sy + 24, line, FB_GREY, 1);
        pct(v1, sizeof(v1), s->park_pm);
        k_snprintf(line, sizeof(line), "parked %s", v1);
        fb_text(fb, cx2, sy + 24, line, FB_GREY, 1);
    } else {
        pct(v1, sizeof(v1), s->park_pm);
        k_snprintf(line, sizeof(line), "busy %s", v1);
        fb_text(fb, gx, sy, line, FB_WHITE, 1);
        k_snprintf(line, sizeof(line), "flash ops/s %lu", (unsigned long)s->flash_ops_per_s);
        fb_text(fb, cx2, sy, line, FB_GREY, 1);
        k_snprintf(line, sizeof(line), "storage %lu KB of %lu KB used",
                   (unsigned long)(s->sys.fs_used / 1024), (unsigned long)(s->sys.fs_total / 1024));
        fb_text(fb, gx, sy + 12, line, FB_GREY, 1);
        k_snprintf(line, sizeof(line), "flash ops since boot %lu", (unsigned long)s->sys.flash_ops);
        fb_text(fb, gx, sy + 24, line, FB_GREY, 1);
    }
}

static void draw(app_ctx_t *c, fb_t *fb, bool full)
{
    state_t *s = (state_t *)c->user;
    // A page change repaints everything: the two share no layout.
    if (s->drawn_page != s->page) {
        fb_fill_rect(fb, 0, 0, fb->w, fb->h, app_bg());
        full = true;
        s->drawn_page = s->page;
    }
    if (s->page == PAGE_PERF) draw_perf(c, fb, full);
    else                      draw_processes(c, fb, full);
}

const app_t app_tasks = {
    .name      = "tasks",
    .about = "what is running, and what it costs",
    .user_size = sizeof(state_t),
    .init      = init,
    .on_event  = on_event,
    .draw      = draw,
};
APP_REGISTER(app_tasks, 75);

// --- for tests ---------------------------------------------------------------------

int app_tasks_pm(const app_ctx_t *c, const char *task_name)
{
    if (!c || !c->user) return -1;
    const state_t *s = (const state_t *)c->user;
    for (int i = 0; i < s->n; ++i) {
        const k_task_info_t *t = &s->info[i];
        if (t->name && strcmp(t->name, task_name) == 0 &&
            t->state != K_DEAD && t->state != K_ZOMBIE) return s->pm[t->id];
    }
    return -1;
}

void app_tasks_core_pm(const app_ctx_t *c, int *busy, int *park, int *idle)
{
    const state_t *s = c && c->user ? (const state_t *)c->user : nullptr;
    if (busy) *busy = s ? s->busy_pm : -1;
    if (park) *park = s ? s->park_pm : -1;
    if (idle) *idle = s ? s->idle_pm : -1;
}

const char *app_tasks_message(const app_ctx_t *c)
{
    return c && c->user ? ((const state_t *)c->user)->msg : "";
}

int app_tasks_page(const app_ctx_t *c)
{
    return c && c->user ? ((const state_t *)c->user)->page : -1;
}

// The newest sample of resource r (0 CPU, 1 memory, 2 flash), per mille; and
// how many samples the graphs hold.
int app_tasks_history(const app_ctx_t *c, int r, int *samples)
{
    const state_t *s = c && c->user ? (const state_t *)c->user : nullptr;
    if (samples) *samples = s ? s->hist_n : 0;
    if (!s || r < 0 || r >= RESOURCES || !s->hist_n) return -1;
    return s->hist[r][s->hist_n - 1];
}

int app_tasks_selected(const app_ctx_t *c)
{
    if (!c || !c->user) return -1;
    int ids[APP_MAX];
    const int n = open_apps(ids);
    const int sel = ((const state_t *)c->user)->apps.sel;
    return (sel >= 0 && sel < n) ? ids[sel] : -1;
}
