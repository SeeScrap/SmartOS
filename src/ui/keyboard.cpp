#include "ui/keyboard.h"

#include <string.h>

#include "gfx/text.h"
#include "os/app.h"
#include "s3hal/input.h"
#include "ui/ui.h"

// The four character rows. Ten to a row, so the grid is even and the cursor's
// column means the same thing on every one of them.
static const char *PAGE[4] = {
    "qwertyuiop",
    "asdfghjkl-",
    "zxcvbnm,./",
    "1234567890",
};
static const char *PAGE_SHIFT[4] = {
    "QWERTYUIOP",
    "ASDFGHJKL_",
    "ZXCVBNM;:?",
    "!@#$%^&*()",
};
static const char *PAGE_SYM[4] = {
    "1234567890",
    "@#$%&*-+=/",
    "()[]{}<>\\|",
    "\"'`~;:,.?!",
};

// The bottom row: five keys, each two columns wide.
#define ACTIONS 5
enum { A_SHIFT, A_SPACE, A_DEL, A_SYM, A_DONE };
static const char *ACTION_NAME[ACTIONS] = { "Shift", "Space", "Del", "Sym", "Done" };

// Geometry. The grid sits at the bottom of the app area; everything above it
// belongs to whoever is using the keyboard.
#define KEY_W   30
#define KEY_H   22
#define GRID_X  6
#define GRID_H  (UI_KB_ROWS * KEY_H)

static int grid_y(const fb_t *fb) { return fb->h - GRID_H - 4; }

static const char *row_chars(const ui_kb_t *kb, int row)
{
    if (kb->sym)   return PAGE_SYM[row];
    if (kb->shift) return PAGE_SHIFT[row];
    return PAGE[row];
}

void ui_kb_init(ui_kb_t *kb, const char *start)
{
    memset(kb, 0, sizeof(*kb));
    if (start) {
        strncpy(kb->text, start, UI_KB_MAX);
        kb->text[UI_KB_MAX] = 0;
        kb->len = (uint8_t)strlen(kb->text);
    }
    kb->row = 1;            // the home row, where a hand would start
    kb->col = 0;
}

char ui_kb_current(const ui_kb_t *kb)
{
    if (kb->row >= 4) return 0;
    return row_chars(kb, kb->row)[kb->col];
}

const char *ui_kb_current_name(const ui_kb_t *kb)
{
    static char one[2];
    if (kb->row >= 4) return ACTION_NAME[kb->col / 2];
    one[0] = ui_kb_current(kb);
    one[1] = 0;
    return one;
}

static void insert(ui_kb_t *kb, char c)
{
    if (kb->len >= UI_KB_MAX) { kb->full = true; return; }
    kb->text[kb->len++] = c;
    kb->text[kb->len] = 0;
    kb->full = (kb->len >= UI_KB_MAX);
}

static void backspace(ui_kb_t *kb)
{
    if (!kb->len) return;
    kb->text[--kb->len] = 0;
    kb->full = false;
}

// OK on whatever the cursor is on.
static void press_key(ui_kb_t *kb)
{
    if (kb->row < 4) {
        insert(kb, ui_kb_current(kb));
        return;
    }
    switch (kb->col / 2) {
        case A_SHIFT: kb->shift = !kb->shift; kb->sym = false; break;
        case A_SPACE: insert(kb, ' ');                         break;
        case A_DEL:   backspace(kb);                           break;
        case A_SYM:   kb->sym = !kb->sym; kb->shift = false;   break;
        case A_DONE:  kb->done = true;                         break;
    }
}

bool ui_kb_event(ui_kb_t *kb, uint32_t btn, uint8_t kind)
{
    // A repeat moves the cursor - crossing the grid is one long press - but
    // does not type: holding OK would otherwise fill the line with one letter.
    const bool moving = (kind == INPUT_PRESS || kind == INPUT_REPEAT);
    const bool acting = (kind == INPUT_PRESS);

    if (moving && (btn == BTN_LEFT || btn == BTN_RIGHT)) {
        const int step = (btn == BTN_RIGHT ? 1 : -1) * (kb->row >= 4 ? 2 : 1);
        kb->col = (uint8_t)(((int)kb->col + step + UI_KB_COLS) % UI_KB_COLS);
        return true;
    }
    if (moving && (btn == BTN_UP || btn == BTN_DOWN)) {
        kb->row = (uint8_t)(((int)kb->row + (btn == BTN_DOWN ? 1 : -1) + UI_KB_ROWS) % UI_KB_ROWS);
        // The wide keys start on even columns, so the cursor lands on one
        // whole key rather than between two.
        if (kb->row >= 4) kb->col = (uint8_t)(kb->col & ~1u);
        return true;
    }
    if (acting && btn == BTN_OK)   { press_key(kb); return true; }
    if (acting && btn == BTN_BACK) { backspace(kb); return true; }
    return false;
}

// --- drawing -----------------------------------------------------------------

static void draw_key(fb_t *fb, int x, int y, int w, const char *label, bool here, bool on)
{
    const uint16_t face = on ? fb_rgb(40, 70, 40) : fb_rgb(28, 34, 46);
    fb_fill_rect(fb, x + 1, y + 1, w - 2, KEY_H - 2, here ? ui_hl() : face);
    fb_rect(fb, x + 1, y + 1, w - 2, KEY_H - 2, here ? FB_CYAN : fb_rgb(50, 58, 74));

    const int scale = (strlen(label) > 1) ? 1 : 2;
    const int tw = fb_text_width(label, scale);
    fb_text(fb, x + (w - tw) / 2, y + (KEY_H - (scale == 2 ? 16 : 8)) / 2,
            label, here ? FB_WHITE : fb_rgb(190, 200, 215), scale);
}

void ui_kb_draw(fb_t *fb, const ui_kb_t *kb, int y, const char *prompt)
{
    const uint16_t bg = app_bg();

    if (prompt) fb_text_bg(fb, 8, y - 12, prompt, FB_CYAN, bg, 1);

    // The line being typed, with a caret. Only the tail fits at this size, so
    // the tail is what is shown - the end is where the typing is happening.
    const int cols = (fb->w - 20) / (TEXT_CELL_W * 2);
    const char *from = kb->text;
    if (kb->len > cols - 1) from = kb->text + (kb->len - (cols - 1));
    fb_fill_rect(fb, 6, y - 2, fb->w - 12, 22, fb_rgb(10, 13, 18));
    fb_rect(fb, 6, y - 2, fb->w - 12, 22, fb_rgb(50, 58, 74));
    const int tw = fb_text(fb, 10, y + 1, from, kb->full ? FB_YELLOW : FB_WHITE, 2);
    fb_fill_rect(fb, 10 + tw, y + 1, 2, 16, FB_CYAN);          // the caret

    const int gy = grid_y(fb);
    for (int r = 0; r < 4; ++r) {
        const char *chars = row_chars(kb, r);
        for (int c = 0; c < UI_KB_COLS; ++c) {
            char label[2] = { chars[c], 0 };
            draw_key(fb, GRID_X + c * KEY_W, gy + r * KEY_H, KEY_W, label,
                     kb->row == r && kb->col == c, false);
        }
    }
    for (int a = 0; a < ACTIONS; ++a) {
        const bool on = (a == A_SHIFT && kb->shift) || (a == A_SYM && kb->sym);
        draw_key(fb, GRID_X + a * 2 * KEY_W, gy + 4 * KEY_H, KEY_W * 2,
                 ACTION_NAME[a], kb->row == 4 && kb->col / 2 == a, on);
    }
}
