// Test only, not registered: an app that logs far faster than it should -
// 200 lines as fast as the mailbox takes them - to show the system log's rate
// limit holding (os/syslog.h). init() does it all and then sits idle.

#include "apps/apps.h"

#include "os/syslog.h"

volatile bool g_logspam_done;

static void init(app_ctx_t *c)
{
    for (int i = 0; i < 200; ++i) app_log(c, "spam line %d of 200", i + 1);
    g_logspam_done = true;
}

static void draw(app_ctx_t *c, fb_t *fb, bool full)
{
    (void)c; (void)fb; (void)full;
}

const app_t app_logspam = {
    .name = "logspam",
    .init = init,
    .draw = draw,
};
