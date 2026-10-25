// A signal reader: one ADC pin, sampled 100 times a second, drawn as a
// sweeping trace like an oscilloscope, with the current reading and the
// minimum and maximum since the pin was chosen.
//
//   UP / DOWN   next / previous ADC pin the system lets apps use
//   OK          pull resistor: none -> up -> down. With nothing wired to the
//               pin, up reads near the top of the range and down near 0 V -
//               the whole path from the pin to this screen, testable bare.
//   RIGHT       record to the app's file "log.csv" (t_ms,mV per line) /
//               stop. Each start replaces the previous recording.
//
// Recording does not write every reading as it comes: 100 small writes a
// second would wear the flash and stall this app on each one. Lines collect in
// memory and go out once a second in one fs_append (stage 4.4).
//
// The samples do not come from here. The device service on core 0 reads the
// ADC on its own schedule and sends each reading to this app as an EV_SENSOR
// event; this app only draws them. See os/dev.h.

#include "apps/apps.h"

#include "gfx/text.h"
#include "os/dev.h"
#include "os/fs.h"
#include "os/kfmt.h"
#include "board/input.h"

#define PLOT_Y     40            // top of the trace area, within the app area
#define PLOT_H     160
#define PLOT_W     LCD_WIDTH
#define MV_TOP     3300
#define PERIOD_MS  10
#define PENDING    64            // samples that can wait between two frames

static const int s_pins[] = { 1, 2, 17, 18 };
#define N_PINS ((int)(sizeof(s_pins) / sizeof(s_pins[0])))

typedef struct {
    int      pin_i;
    int      pull;               // dev_pull_t
    uint32_t last_mv, min_mv, max_mv;
    uint32_t samples, rate, rate_count;

    // Readings that have arrived but are not drawn yet. The trace advances
    // one pixel per reading, so none may be skipped - but they may pile up
    // while this app waits for its turn to draw.
    uint16_t pending[PENDING];
    int      npending;
    int      x;                  // next column of the sweep
    int      prev_y;             // where the trace was at column x-1
    bool     restart;            // the pin changed: clear the plot

    bool     rec;                // recording to log.csv
    char     recbuf[2048];       // lines not yet written
    size_t   reclen;
    uint32_t rec_lines;
    int      rec_err;            // the last write's result, if it failed
} state_t;

static void rec_flush(app_ctx_t *c)
{
    state_t *s = (state_t *)c->user;
    if (!s->reclen) return;
    const int rc = fs_append(c, "log.csv", s->recbuf, s->reclen);
    if (rc != FS_OK) s->rec_err = rc;
    s->reclen = 0;
}

static int to_y(uint32_t mv)
{
    if (mv > MV_TOP) mv = MV_TOP;
    return PLOT_Y + PLOT_H - 1 - (int)(mv * (PLOT_H - 1) / MV_TOP);
}

static void choose(app_ctx_t *c, int pin_i)
{
    state_t *s = (state_t *)c->user;
    dev_adc_stream(c, s_pins[s->pin_i], 0);          // stop the old stream
    s->pin_i    = (pin_i + N_PINS) % N_PINS;
    s->pull     = DEV_PULL_NONE;
    s->min_mv   = UINT32_MAX;
    s->max_mv   = 0;
    s->npending = 0;
    s->x        = 0;
    s->restart  = true;
    dev_pin_pull(s_pins[s->pin_i], DEV_PULL_NONE);
    dev_adc_stream(c, s_pins[s->pin_i], PERIOD_MS);
    app_invalidate(c);
}

static void init(app_ctx_t *c)
{
    state_t *s = (state_t *)c->user;
    s->pin_i = 0;
    choose(c, 0);
    app_timer(c, 1000, 1);                           // samples-per-second count
}

static void on_event(app_ctx_t *c, const ev_t *ev)
{
    state_t *s = (state_t *)c->user;

    if (ev->type == EV_SENSOR) {
        if ((int)ev->a != s_pins[s->pin_i]) return;  // a stream we already stopped
        s->last_mv = ev->b;
        if (ev->b < s->min_mv) s->min_mv = ev->b;
        if (ev->b > s->max_mv) s->max_mv = ev->b;
        s->samples++;
        s->rate_count++;
        if (s->rec) {
            if (s->reclen + 24 > sizeof(s->recbuf)) rec_flush(c);   // never drop a line
            s->reclen += (size_t)k_snprintf(s->recbuf + s->reclen,
                                            sizeof(s->recbuf) - s->reclen, "%lu,%lu\n",
                                            (unsigned long)ev->t_ms, (unsigned long)ev->b);
            s->rec_lines++;
        }
        if (s->npending < PENDING) s->pending[s->npending++] = (uint16_t)ev->b;
        app_invalidate(c);
        return;
    }
    if (ev->type == EV_TIMER) {
        s->rate = s->rate_count;
        s->rate_count = 0;
        if (s->rec) rec_flush(c);                   // once a second
        return;
    }
    if (ev->type != EV_INPUT || ev->a != INPUT_PRESS) return;

    switch (1u << ev->b) {
        case BTN_UP:   choose(c, s->pin_i + 1); break;
        case BTN_DOWN: choose(c, s->pin_i - 1); break;
        case BTN_RIGHT:
            if (!s->rec) {
                static const char head[] = "t_ms,mV\n";
                s->rec_err   = fs_write(c, "log.csv", head, sizeof(head) - 1);
                s->rec_lines = 0;
                s->reclen    = 0;
                s->rec       = (s->rec_err == FS_OK);
            } else {
                rec_flush(c);
                s->rec = false;
            }
            app_invalidate(c);
            break;
        case BTN_OK:
            s->pull = (s->pull + 1) % 3;
            dev_pin_pull(s_pins[s->pin_i], (dev_pull_t)s->pull);
            s->min_mv = UINT32_MAX;
            s->max_mv = 0;
            app_invalidate(c);
            break;
        default: break;
    }
}

static void draw(app_ctx_t *c, fb_t *fb, bool full)
{
    state_t *s = (state_t *)c->user;
    const uint16_t bg   = app_bg();
    const uint16_t grid = fb_rgb(40, 48, 64);

    if (full || s->restart) {
        fb_fill_rect(fb, 0, PLOT_Y, PLOT_W, PLOT_H, bg);
        for (int mv = 0; mv <= MV_TOP; mv += 1000) fb_hline(fb, 0, to_y(mv), PLOT_W, grid);
        s->x = 0;
        s->restart = false;
    }

    // The sweep: for each waiting reading, clear a short band ahead of the
    // cursor and draw the segment from the previous reading to this one.
    for (int i = 0; i < s->npending; ++i) {
        const int y = to_y(s->pending[i]);
        fb_vline(fb, s->x, PLOT_Y, PLOT_H, bg);
        if ((s->x % 20) == 0) {
            for (int mv = 0; mv <= MV_TOP; mv += 1000) fb_hline(fb, s->x, to_y(mv), 1, grid);
        }
        if (s->x > 0) fb_line(fb, s->x - 1, s->prev_y, s->x, y, FB_GREEN);
        else          fb_hline(fb, 0, y, 1, FB_GREEN);
        fb_vline(fb, (s->x + 1) % PLOT_W, PLOT_Y, PLOT_H, FB_GREY);   // the cursor
        s->prev_y = y;
        s->x = (s->x + 1) % PLOT_W;
    }
    s->npending = 0;

    static const char *pulls[] = { "none", "up", "down" };
    char line[64];
    k_snprintf(line, sizeof(line), "GPIO%-2d  %4lu mV  pull %-4s  %3lu/s",
               s_pins[s->pin_i], (unsigned long)s->last_mv, pulls[s->pull],
               (unsigned long)s->rate);
    fb_text_bg(fb, 4, 4, line, FB_WHITE, bg, 1);
    k_snprintf(line, sizeof(line), "min %4lu  max %4lu mV   UP/DOWN pin  OK pull",
               (unsigned long)(s->min_mv == UINT32_MAX ? 0 : s->min_mv),
               (unsigned long)s->max_mv);
    fb_text_bg(fb, 4, 18, line, FB_GREY, bg, 1);

    if (s->rec_err)   k_snprintf(line, sizeof(line), "log.csv: write failed (%d)      ", s->rec_err);
    else if (s->rec)  k_snprintf(line, sizeof(line), "REC log.csv  %6lu lines        ", (unsigned long)s->rec_lines);
    else              k_snprintf(line, sizeof(line), "RIGHT: record to log.csv       ");
    fb_text_bg(fb, 4, 204, line, s->rec ? FB_RED : FB_GREY, bg, 1);
}

const app_t app_scope = {
    .name      = "scope",
    .about = "reads a pin, plots it, records to file",
    .user_size = sizeof(state_t),
    .init      = init,
    .on_event  = on_event,
    .draw      = draw,
};
APP_REGISTER(app_scope, 40);

uint32_t app_scope_mv(const app_ctx_t *c)      { return c && c->user ? ((const state_t *)c->user)->last_mv : 0; }
uint32_t app_scope_samples(const app_ctx_t *c) { return c && c->user ? ((const state_t *)c->user)->samples : 0; }
int      app_scope_pin(const app_ctx_t *c)     { return c && c->user ? s_pins[((const state_t *)c->user)->pin_i] : -1; }
uint32_t app_scope_rec_lines(const app_ctx_t *c) { return c && c->user ? ((const state_t *)c->user)->rec_lines : 0; }
