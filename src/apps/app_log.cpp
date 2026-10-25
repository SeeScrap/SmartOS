// The system log, as kept on flash: boots and why the last one ended, core 1
// faults, apps opened and closed, and whatever apps have written with app_log.
//
//   UP / DOWN     scroll a line; held, it repeats
//   LEFT / RIGHT  scroll sideways - long lines run past the screen
//   OK            read it again
//
// It opens at the newest line. The text comes from core 0 through the mailbox
// (slog_tail), the last few kilobytes of it, which is as far back as there is
// any point scrolling on a screen this size.

#include "apps/apps.h"

#include <string.h>

#include "gfx/text.h"
#include "os/kfmt.h"
#include "os/syslog.h"
#include "board/input.h"
#include "ui/ui.h"

#define TEXT_MAX  3072
#define LINES_MAX 96
#define VISIBLE   19                // 10 px a line, under a one-line header
#define COLS      53                // what fits across at scale 1
#define TOP_Y     18

typedef struct {
    char     text[TEXT_MAX];
    uint16_t line_at[LINES_MAX];    // where each line starts in text
    int      lines;
    int      top;                   // first line on screen
    int      col;                   // sideways scroll, in characters
} state_t;

static void load(app_ctx_t *c)
{
    state_t *s = (state_t *)c->user;
    const size_t n = slog_tail(s->text, sizeof(s->text));

    // Index the lines, keeping the newest LINES_MAX if there are more.
    s->lines = 0;
    size_t start = 0;
    for (size_t i = 0; i < n; ++i) {
        if (s->text[i] != '\n') continue;
        s->text[i] = 0;
        if (s->lines == LINES_MAX) {
            memmove(s->line_at, s->line_at + 1, (LINES_MAX - 1) * sizeof(s->line_at[0]));
            s->lines--;
        }
        s->line_at[s->lines++] = (uint16_t)start;
        start = i + 1;
    }
    s->top = s->lines > VISIBLE ? s->lines - VISIBLE : 0;    // the newest at the bottom
    s->col = 0;
    app_invalidate(c);
}

static void init(app_ctx_t *c)
{
    load(c);
}

static void on_event(app_ctx_t *c, const ev_t *ev)
{
    state_t *s = (state_t *)c->user;
    if (ev->type != EV_INPUT || (ev->a != INPUT_PRESS && ev->a != INPUT_REPEAT)) return;

    switch (1u << ev->b) {
        // Scrolling a window over the lines, clamped at both ends (ui/ui.h).
        case BTN_UP:    ui_scroll(&s->top, s->lines, VISIBLE, -1); break;
        case BTN_DOWN:  ui_scroll(&s->top, s->lines, VISIBLE, 1);  break;
        case BTN_LEFT:  if (s->col > 0) s->col -= 20;       break;
        case BTN_RIGHT: if (s->col < 80) s->col += 20;      break;
        case BTN_OK:    if (ev->a == INPUT_PRESS) load(c);  return;
        default:        return;
    }
    app_invalidate(c);
}

static void draw(app_ctx_t *c, fb_t *fb, bool full)
{
    state_t *s = (state_t *)c->user;
    const uint16_t bg = app_bg();
    (void)full;

    char head[64];
    k_snprintf(head, sizeof(head), "system log  %d lines   UP/DOWN  LEFT/RIGHT  OK reload",
               s->lines);
    fb_text_bg(fb, 4, 4, head, FB_CYAN, bg, 1);

    for (int row = 0; row < VISIBLE; ++row) {
        const int i = s->top + row;
        char shown[COLS + 1];
        const char *text = (i < s->lines) ? s->text + s->line_at[i] : "";
        const size_t len = strlen(text);
        const char *from = ((size_t)s->col < len) ? text + s->col : "";
        // Padded to the full width, so a short line clears whatever was there.
        k_snprintf(shown, sizeof(shown), "%-53.53s", from);
        const bool fault = strstr(text, " fault ") || strstr(text, "panic");
        fb_text_bg(fb, 4, TOP_Y + row * TEXT_CELL_H, shown,
                   fault ? FB_RED : FB_WHITE, bg, 1);
    }
}

// Not "app_log": that is the logging call apps use (os/syslog.h).
const app_t app_logview = {
    .name      = "log",
    .about = "the system log, newest at the bottom",
    .user_size = sizeof(state_t),
    .init      = init,
    .on_event  = on_event,
    .draw      = draw,
};
APP_REGISTER(app_logview, 80);

// For the test script.
int app_logview_lines(const app_ctx_t *c)
{
    return c && c->user ? ((const state_t *)c->user)->lines : 0;
}

const char *app_logview_line(const app_ctx_t *c, int from_end)
{
    if (!c || !c->user) return "";
    const state_t *s = (const state_t *)c->user;
    const int i = s->lines - 1 - from_end;
    return (i >= 0 && i < s->lines) ? s->text + s->line_at[i] : "";
}
