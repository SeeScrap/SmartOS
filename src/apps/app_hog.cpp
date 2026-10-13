// Test only: an app that never gives the core back. Its init() spins forever,
// so it never reaches the runtime loop, never reads an event and never draws.
//
// It is here to show what running apps on core 1 buys: preemption. This app
// gets its share of the core round-robin with the other apps and no more, the
// HIGH-priority system tasks stay ahead of it entirely, and core 0 does not
// notice it exists. On core 0, with no preemption, it would have taken USB,
// flash and the display down with it.

#include "apps/apps.h"

static volatile uint32_t s_spins;

static void init(app_ctx_t *c)
{
    (void)c;
    for (;;) s_spins = s_spins + 1;
}

static void draw(app_ctx_t *c, fb_t *fb, bool full)
{
    (void)c; (void)fb; (void)full;
}

// Deliberately NOT registered: it must never start on its own.
const app_t app_hog = {
    .name      = "hog",
    .init      = init,
    .draw      = draw,
};

uint32_t app_hog_spins(void) { return s_spins; }
