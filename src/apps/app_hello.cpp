// The smallest complete app - copy this file to start a new one.
//
// This file is the whole app. Nothing else in the tree mentions it: it is
// compiled because it sits in src/apps/, linked because the component is
// whole-archive, and it runs because the APP_REGISTER line at the bottom adds
// it to the registry before app_main starts. (After adding a new file, delete
// the build folder .pio/build/s3-n16r8/ once so the build notices it.)
//
// What the runtime gives an app, and the rules it has to live by, are in
// os/app.h. Hardware is in os/dev.h; text formatting in kernel/kfmt.h; random
// numbers in os/krand.h; drawing in gfx/fb.h and gfx/text.h.

#include "smartos_app.h"


// Everything the app remembers. The runtime allocates it zeroed and hands it
// back as c->user in every callback.
typedef struct {
    uint32_t presses;
} state_t;

// Once, when the app starts - before it has focus, so no drawing here.
static void init(app_ctx_t *c)
{
    (void)c;
}

// Every event: buttons (only while focused), timers, sensor readings, and
// system notices such as SYS_FOCUS_GAINED.
static void on_event(app_ctx_t *c, const ev_t *ev)
{
    state_t *s = (state_t *)c->user;
    if (ev->type == EV_INPUT && ev->a == INPUT_PRESS) {
        s->presses++;
        app_invalidate(c);           // ask for draw() to be called
    }
}

// Only when focused, and only after app_invalidate() - or with full = true
// when the app has just been given the screen and it is freshly cleared.
static void draw(app_ctx_t *c, fb_t *fb, bool full)
{
    state_t *s = (state_t *)c->user;
    if (full) fb_text(fb, 8, 8, "Hello", FB_CYAN, 3);

    char line[32];
    k_snprintf(line, sizeof(line), "buttons pressed: %lu", (unsigned long)s->presses);
    fb_text_bg(fb, 8, 60, line, FB_WHITE, app_bg(), 1);
}

static const app_t app_hello = {
    .name      = "hello",           // the status bar and Home show it
    .about = "the smallest app there is",
    .user_size = sizeof(state_t),   // bytes for c->user
    .init      = init,
    .on_event  = on_event,
    .draw      = draw,              // the only callback that is required
    // Also available: .mask (broadcast event types wanted besides the usual
    // ones), .stop (called on the way out) and .flags. Anything left out is 0.
};
APP_REGISTER(app_hello, 60);
