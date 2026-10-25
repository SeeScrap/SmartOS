// Snake. The arrows steer, OK starts again after a crash.
//
// Written as a template for small games on this system: a fixed-rate timer is
// the game loop, input only changes intent (the next direction), and draw()
// paints just the cells that changed since the last frame - a head in, a tail
// out, maybe a piece of food - which is a few hundred bytes to the panel per
// move instead of a whole screen.

#include "apps/apps.h"

#include "gfx/text.h"
#include "os/kfmt.h"
#include "os/fs.h"
#include "os/krand.h"
#include "board/input.h"

#define CELL      8
#define COLS      (LCD_WIDTH / 8)
#define TOP_ROW   3                // rows 0-2: the score line
#define ROWS      27               // 216 / 8
#define MAX_LEN   (COLS * (ROWS - TOP_ROW))
#define TICK_MS   120
#define DIRTY_MAX 16

enum { DIR_UP, DIR_DOWN, DIR_LEFT, DIR_RIGHT };

typedef struct {
    uint16_t body[MAX_LEN];        // ring of cell indices, head at [head]
    int      head, len;
    int      dir, next_dir;
    int      food;
    int      grow;
    bool     over;
    uint32_t score, moves;
    uint32_t best;                 // kept in the app's file "best" (stage 4.4)

    // Cells to repaint at the next draw. If more pile up than fit - frames
    // are waiting on the display - the whole board is repainted instead.
    int      dirty[DIRTY_MAX];
    int      ndirty;
    bool     board_dirty;
    bool     score_dirty;
} state_t;

static inline int cell(int col, int row) { return row * COLS + col; }
static inline int col_of(int c) { return c % COLS; }
static inline int row_of(int c) { return c / COLS; }

static void mark(state_t *s, int c)
{
    if (s->ndirty < DIRTY_MAX) s->dirty[s->ndirty++] = c;
    else s->board_dirty = true;
}

static bool on_snake(const state_t *s, int c)
{
    for (int i = 0; i < s->len; ++i) {
        if (s->body[(s->head - i + MAX_LEN) % MAX_LEN] == c) return true;
    }
    return false;
}

static void place_food(state_t *s)
{
    // The board is never close to full in practice, so trying random cells
    // until one is free ends quickly.
    int c;
    do {
        c = cell((int)k_rand_below(COLS), TOP_ROW + (int)k_rand_below(ROWS - TOP_ROW));
    } while (on_snake(s, c));
    s->food = c;
    mark(s, c);
}

static void reset(state_t *s)
{
    s->len = 3;
    s->head = 2;
    const int row = TOP_ROW + (ROWS - TOP_ROW) / 2;
    for (int i = 0; i < 3; ++i) s->body[i] = (uint16_t)cell(3 + i, row);
    s->dir = s->next_dir = DIR_RIGHT;
    s->grow = 0;
    s->over = false;
    s->score = 0;
    s->moves = 0;
    s->ndirty = 0;
    s->board_dirty = true;
    s->score_dirty = true;
    place_food(s);
}

static void init(app_ctx_t *c)
{
    state_t *s = (state_t *)c->user;
    reset(s);
    // The best score outlives the app: it is read back from the file on every
    // open. No file yet - a first game - simply leaves it at 0.
    size_t got = 0;
    if (fs_read(c, "best", &s->best, sizeof(s->best), &got) != FS_OK || got != sizeof(s->best)) {
        s->best = 0;
    }
    app_timer(c, TICK_MS, 1);
}

static void step(state_t *s)
{
    s->dir = s->next_dir;
    const int h = s->body[s->head];
    int col = col_of(h), row = row_of(h);
    switch (s->dir) {
        case DIR_UP:    row--; break;
        case DIR_DOWN:  row++; break;
        case DIR_LEFT:  col--; break;
        case DIR_RIGHT: col++; break;
    }
    const int nc = cell(col, row);
    if (col < 0 || col >= COLS || row < TOP_ROW || row >= ROWS || on_snake(s, nc)) {
        s->over = true;
        s->score_dirty = true;
        return;
    }

    s->head = (s->head + 1) % MAX_LEN;
    s->body[s->head] = (uint16_t)nc;
    mark(s, nc);
    s->moves++;

    if (nc == s->food) {
        s->grow += 2;
        s->score++;
        s->score_dirty = true;
        place_food(s);
    }
    if (s->grow > 0) {
        s->grow--;
        s->len++;
    } else {
        mark(s, s->body[(s->head - s->len + MAX_LEN) % MAX_LEN]);   // the tail leaves
    }
}

static void on_event(app_ctx_t *c, const ev_t *ev)
{
    state_t *s = (state_t *)c->user;

    if (ev->type == EV_TIMER) {
        // Paused while in the background. An app's timers keep firing when it
        // is not focused - the first version of this game kept playing unseen
        // from boot, and had crashed into the wall before anyone switched to
        // it. A game that nobody can see must not advance.
        if (!c->focused) return;
        // b is how many ticks this event stands for; if the app fell behind,
        // it catches up in moves rather than silently running slow.
        for (uint32_t k = 0; k < ev->b && !s->over; ++k) step(s);
        if (s->over && s->score > s->best) {
            s->best = s->score;
            fs_write(c, "best", &s->best, sizeof(s->best));   // replaced atomically
        }
        app_invalidate(c);
        return;
    }
    if (ev->type != EV_INPUT) return;
    if (ev->a != INPUT_PRESS && ev->a != INPUT_REPEAT) return;

    const uint32_t btn = 1u << ev->b;
    if (s->over) {
        if (btn == BTN_OK) { reset(s); app_invalidate(c); }
        return;
    }
    // Never straight back into yourself: that is an instant, unfair death.
    if (btn == BTN_UP    && s->dir != DIR_DOWN)  s->next_dir = DIR_UP;
    if (btn == BTN_DOWN  && s->dir != DIR_UP)    s->next_dir = DIR_DOWN;
    if (btn == BTN_LEFT  && s->dir != DIR_RIGHT) s->next_dir = DIR_LEFT;
    if (btn == BTN_RIGHT && s->dir != DIR_LEFT)  s->next_dir = DIR_RIGHT;
}

static void paint_cell(fb_t *fb, const state_t *s, int c)
{
    const int x = col_of(c) * CELL, y = row_of(c) * CELL;
    uint16_t colr = app_bg();
    if (c == s->food)          colr = FB_RED;
    else if (on_snake(s, c))   colr = (c == s->body[s->head]) ? FB_YELLOW : FB_GREEN;
    fb_fill_rect(fb, x, y, CELL - 1, CELL - 1, colr);
}

static void draw(app_ctx_t *c, fb_t *fb, bool full)
{
    state_t *s = (state_t *)c->user;

    if (full || s->board_dirty) {
        fb_fill_rect(fb, 0, TOP_ROW * CELL, COLS * CELL, (ROWS - TOP_ROW) * CELL, app_bg());
        for (int i = 0; i < s->len; ++i) {
            paint_cell(fb, s, s->body[(s->head - i + MAX_LEN) % MAX_LEN]);
        }
        paint_cell(fb, s, s->food);
        fb_hline(fb, 0, TOP_ROW * CELL - 2, COLS * CELL, FB_GREY);
        s->board_dirty = false;
        s->score_dirty = true;
    } else {
        for (int i = 0; i < s->ndirty; ++i) paint_cell(fb, s, s->dirty[i]);
    }
    s->ndirty = 0;

    if (full || s->score_dirty) {
        char line[64];
        if (s->over) k_snprintf(line, sizeof(line), "GAME OVER  score %-4lu best %-4lu OK: again",
                                (unsigned long)s->score, (unsigned long)s->best);
        else         k_snprintf(line, sizeof(line), "Snake      score %-4lu best %-4lu          ",
                                (unsigned long)s->score, (unsigned long)s->best);
        fb_text_bg(fb, 4, 6, line, s->over ? FB_RED : FB_WHITE, app_bg(), 1);
        s->score_dirty = false;
    }
}

const app_t app_snake = {
    .name      = "snake",
    .about = "the game - it pauses when it is not in front",
    .user_size = sizeof(state_t),
    .init      = init,
    .on_event  = on_event,
    .draw      = draw,
};
APP_REGISTER(app_snake, 50);

bool     app_snake_over(const app_ctx_t *c)  { return c && c->user && ((const state_t *)c->user)->over; }
uint32_t app_snake_moves(const app_ctx_t *c) { return c && c->user ? ((const state_t *)c->user)->moves : 0; }
int      app_snake_head_row(const app_ctx_t *c)
{
    if (!c || !c->user) return -1;
    const state_t *s = (const state_t *)c->user;
    return row_of(s->body[s->head]);
}
