// Stage 3.6's button screen, moved into the app model in stage 4.3b.
//
// What changed in the move: it no longer subscribes to the bus itself - the
// manager forwards it input only while it has focus - and it no longer owns the
// screen: it draws into the app area, 216 rows starting at panel row 24, so
// every y below is the old one less 24. The stats are redrawn on its own 250 ms
// timer, which keeps running while it is in the background; it simply does not
// get to draw until it has focus again.

#include "apps/apps.h"

#include "smartos_app.h"

#define LOG_LINES 9
#define LOG_Y     116

struct box_t { int16_t x, y, w, h; };

// A D-pad on the left, OK in the middle of it, BACK below and to the right.
static const box_t s_box[BTN_COUNT] = {
    {  52,  8, 44, 26 },   // UP
    {  52, 68, 44, 26 },   // DOWN
    {   4, 38, 44, 26 },   // LEFT
    { 100, 38, 44, 26 },   // RIGHT
    {  52, 38, 44, 26 },   // OK
    { 100, 68, 44, 26 },   // BACK
};

typedef struct {
    uint32_t inputs, lat, lat_max, held_ev;
    char     log[LOG_LINES][54];
    int      head;              // oldest line
} state_t;

static void log_push(state_t *s, const char *line)
{
    k_snprintf(s->log[s->head], sizeof(s->log[0]), "%-52s", line);
    s->head = (s->head + 1) % LOG_LINES;
}

static void mask_names(uint32_t m, char *out, size_t n)
{
    size_t used = 0;
    out[0] = 0;
    for (int b = 0; b < BTN_COUNT; ++b) {
        if (!(m & (1u << b))) continue;
        used += (size_t)k_snprintf(out + used, n - used, "%s%s", used ? " " : "",
                                   input_name((unsigned)b));
        if (used >= n) break;
    }
    if (!used) k_snprintf(out, n, "-");
}

static void init(app_ctx_t *c)
{
    state_t *s = (state_t *)c->user;
    for (int i = 0; i < LOG_LINES; ++i) k_snprintf(s->log[i], sizeof(s->log[0]), "%-52s", "");
    log_push(s, "  waiting for a button...");
    app_timer(c, 250, 1);
}

static void on_event(app_ctx_t *c, const ev_t *ev)
{
    state_t *s = (state_t *)c->user;

    if (ev->type == EV_TIMER) { app_invalidate(c); return; }
    if (ev->type != EV_INPUT) return;

    s->inputs++;
    s->lat = k_millis() - ev->t_ms;
    if (s->lat > s->lat_max) s->lat_max = s->lat;

    const uint32_t bit = 1u << ev->b;
    if (ev->a == INPUT_PRESS)   s->held_ev |=  bit;
    if (ev->a == INPUT_RELEASE) s->held_ev &= ~bit;

    char line[64];
    k_snprintf(line, sizeof(line), "%8lu  %-7s  %-6s %5u  %3lu ms",
               (unsigned long)ev->t_ms, input_kind_name((uint8_t)ev->a),
               input_name(ev->b), (unsigned)ev->seq, (unsigned long)s->lat);
    log_push(s, line);
    app_invalidate(c);
}

static void draw(app_ctx_t *c, fb_t *fb, bool full)
{
    state_t *s = (state_t *)c->user;
    const uint16_t bg    = app_bg();
    const uint16_t panel = fb_rgb(28, 34, 52);

    if (full) {
        fb_hline(fb, 0, 100, fb->w, fb_rgb(60, 60, 80));
        fb_text(fb, 4, 104, "    time  event    button   seq  latency", FB_CYAN, 1);
    }

    for (int b = 0; b < BTN_COUNT; ++b) {
        const bool   lit = (s->held_ev >> b) & 1u;
        const box_t &r   = s_box[b];
        fb_fill_rect(fb, r.x, r.y, r.w, r.h, lit ? FB_YELLOW : panel);
        fb_rect(fb, r.x, r.y, r.w, r.h, lit ? FB_WHITE : FB_CYAN);
        const char *name = input_name((unsigned)b);
        fb_text(fb, r.x + (r.w - fb_text_width(name, 1)) / 2,
                r.y + (r.h - TEXT_CELL_H) / 2 + 1, name, lit ? FB_BLACK : FB_WHITE, 1);
    }

    for (int i = 0; i < LOG_LINES; ++i) {
        fb_text_bg(fb, 4, LOG_Y + i * TEXT_CELL_H, s->log[(s->head + i) % LOG_LINES],
                   i == LOG_LINES - 1 ? FB_YELLOW : FB_WHITE, bg, 1);
    }

    // Two views of the same thing. The first is rebuilt from events alone; the
    // second is the driver's own state. If they disagree while nothing is
    // queued, an event was lost somewhere between the sampler and this app.
    char line[64], names[32];
    const int x = 156;
    const evbus_stats_t st = evbus_stats(c->sub);
    k_snprintf(line, sizeof(line), "inputs   %6lu", (unsigned long)s->inputs);
    fb_text_bg(fb, x, 8, line, FB_WHITE, bg, 1);
    k_snprintf(line, sizeof(line), "dropped  %6lu", (unsigned long)st.dropped);
    fb_text_bg(fb, x, 20, line, st.dropped ? FB_RED : FB_WHITE, bg, 1);
    k_snprintf(line, sizeof(line), "latency  %3lu ms max %3lu",
               (unsigned long)s->lat, (unsigned long)s->lat_max);
    fb_text_bg(fb, x, 32, line, FB_WHITE, bg, 1);

    mask_names(s->held_ev, names, sizeof(names));
    k_snprintf(line, sizeof(line), "held (events) %-12s", names);
    fb_text_bg(fb, x, 50, line, FB_GREEN, bg, 1);
    mask_names(input_held(), names, sizeof(names));
    k_snprintf(line, sizeof(line), "held (driver) %-12s", names);
    fb_text_bg(fb, x, 62, line, FB_GREEN, bg, 1);

    const uint32_t ms = k_millis();
    k_snprintf(line, sizeof(line), "kernel %5lu.%lu s", (unsigned long)(ms / 1000),
               (unsigned long)((ms / 100) % 10));
    fb_text_bg(fb, x, 80, line, FB_GREY, bg, 1);
}

const app_t app_input = {
    .name      = "input",
    .about = "buttons, as the system sees them",
    .user_size = sizeof(state_t),
    .init      = init,
    .on_event  = on_event,
    .draw      = draw,
};
APP_REGISTER(app_input, 10);

uint32_t app_input_inputs(const app_ctx_t *c)
{
    return c && c->user ? ((const state_t *)c->user)->inputs : 0;
}
