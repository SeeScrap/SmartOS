// A number you can change: UP adds one, DOWN takes one away, OK resets, and a
// held key repeats. A cursor blinks on its own 500 ms timer. Small on purpose
// - it exists to show that input reaches exactly one app, and that an app's
// state survives losing focus and getting it back.

#include "apps/apps.h"

#include "smartos_app.h"

typedef struct {
    int32_t  value;
    uint32_t inputs;
    bool     blink;
} state_t;

static void init(app_ctx_t *c)
{
    app_timer(c, 500, 1);
}

static void on_event(app_ctx_t *c, const ev_t *ev)
{
    state_t *s = (state_t *)c->user;

    if (ev->type == EV_TIMER) {
        s->blink = !s->blink;
        app_invalidate(c);
        return;
    }
    if (ev->type != EV_INPUT) return;

    s->inputs++;
    if (ev->a != INPUT_PRESS && ev->a != INPUT_REPEAT) return;
    switch (1u << ev->b) {
        case BTN_UP:   s->value++;    break;
        case BTN_DOWN: s->value--;    break;
        case BTN_OK:   s->value = 0;  break;
        default:       return;
    }
    app_invalidate(c);
}

static void draw(app_ctx_t *c, fb_t *fb, bool full)
{
    state_t *s = (state_t *)c->user;
    const uint16_t bg = app_bg();

    if (full) {
        fb_text(fb, 8, 8, "Counter", FB_CYAN, 2);
        fb_text(fb, 8, 190, "UP +1   DOWN -1   OK reset   hold BACK: next app",
                FB_GREY, 1);
    }

    // Fixed width and opaque, so a shorter number overwrites a longer one
    // completely - no erase pass needed.
    char num[16];
    k_snprintf(num, sizeof(num), "%6ld", (long)s->value);
    const int scale = 5;
    const int w = fb_text_width(num, scale);
    const int x = (fb->w - w) / 2;
    const int y = 70;
    fb_text_bg(fb, x, y, num, FB_WHITE, bg, scale);

    // The cursor: its own small region, so a blink costs one tiny flush.
    fb_fill_rect(fb, x + w + 6, y, 6, TEXT_CELL_H * scale - 10,
                 s->blink ? FB_YELLOW : bg);
}

const app_t app_counter = {
    .name      = "counter",
    .about = "a number; UP, DOWN and OK change it",
    .user_size = sizeof(state_t),
    .init      = init,
    .on_event  = on_event,
    .draw      = draw,
};
APP_REGISTER(app_counter, 20);

int32_t app_counter_value(const app_ctx_t *c)
{
    return c && c->user ? ((const state_t *)c->user)->value : 0;
}

uint32_t app_counter_inputs(const app_ctx_t *c)
{
    return c && c->user ? ((const state_t *)c->user)->inputs : 0;
}
