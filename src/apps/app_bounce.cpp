// A box bouncing around the app area on a 20 ms timer - 50 moves a second.
//
// It is the frame-pacing test. Each move asks for a draw; the draw can only
// happen once the previous frame is on the panel. If the display ever fell
// behind, moves would be merged into one frame rather than queue up, because
// the erase always covers where the box was last DRAWN, not where it was last
// moved to.

#include "apps/apps.h"

#include "gfx/text.h"

#define BOX 24

typedef struct {
    int16_t  x, y, dx, dy;
    int16_t  drawn_x, drawn_y;
    bool     drawn;
    uint32_t inputs;
} state_t;

static void init(app_ctx_t *c)
{
    state_t *s = (state_t *)c->user;
    s->x = 10; s->y = 30; s->dx = 3; s->dy = 2;
    app_timer(c, 20, 1);
}

static void on_event(app_ctx_t *c, const ev_t *ev)
{
    state_t *s = (state_t *)c->user;

    if (ev->type == EV_INPUT) { s->inputs++; return; }
    if (ev->type != EV_TIMER) return;

    // b is how many periods this timer event stands for - more than one if
    // the service ran late - so the box moves as far as it should have.
    for (uint32_t k = 0; k < ev->b; ++k) {
        s->x += s->dx;
        s->y += s->dy;
        if (s->x < 0)                   { s->x = 0;                    s->dx = -s->dx; }
        if (s->x > LCD_WIDTH - BOX)     { s->x = LCD_WIDTH - BOX;            s->dx = -s->dx; }
        if (s->y < 20)                  { s->y = 20;                   s->dy = -s->dy; }
        if (s->y > APP_AREA_H - BOX)    { s->y = APP_AREA_H - BOX;     s->dy = -s->dy; }
    }
    app_invalidate(c);
}

static void draw(app_ctx_t *c, fb_t *fb, bool full)
{
    state_t *s = (state_t *)c->user;

    if (full) {
        fb_text(fb, 8, 2, "Bounce  50 Hz", FB_CYAN, 1);
        s->drawn = false;            // the area was just cleared
    }
    if (s->drawn) fb_fill_rect(fb, s->drawn_x, s->drawn_y, BOX, BOX, app_bg());
    fb_fill_rect(fb, s->x, s->y, BOX, BOX, fb_rgb(255, 160, 0));
    s->drawn_x = s->x;
    s->drawn_y = s->y;
    s->drawn   = true;
}

const app_t app_bounce = {
    .name      = "bounce",
    .about = "a box bouncing at 50 frames a second",
    .user_size = sizeof(state_t),
    .init      = init,
    .on_event  = on_event,
    .draw      = draw,
};
APP_REGISTER(app_bounce, 30);

uint32_t app_bounce_inputs(const app_ctx_t *c)
{
    return c && c->user ? ((const state_t *)c->user)->inputs : 0;
}
