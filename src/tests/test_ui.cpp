#include "tests/tests.h"

#include <stdio.h>
#include <string.h>

#include "s3hal/log.h"
#include "tests/harness.h"
#include "ui/keyboard.h"
#include "ui/ui.h"
#include "s3hal/input.h"

// ---------------------------------------------------------------------------
// Stage 5.4c - the app toolkit's rules, checked by script every boot.
//
// These are the parts that used to be a copy per app, so they are worth
// holding to something: a list that wraps, skips its headers and scrolls only
// as far as it must; a row that does not fit is not drawn at all; a bar that
// clamps; and an action that needs the same key twice.
//
// No hardware beyond a framebuffer, and no kernel - it is arithmetic, so it
// runs before core 1 starts.
// ---------------------------------------------------------------------------

// A list of ten rows, every third one a header: 0 H, 3 H, 6 H, 9 H.
static int  th_height(void *ctx, int i)     { (void)ctx; return (i % 3) ? 20 : 10; }
static bool th_pick(void *ctx, int i)       { (void)ctx; return (i % 3) != 0; }

static void ui_case(const char *what, bool ok, const char *detail)
{
    logf("  %-28s %-46s %s\n", what, detail, ok ? "OK" : "FAIL");
}

void ui_logic_test(void)
{
    logln();
    logln("--- stage 5.4c: the app toolkit, fed a script ---");
    char d[160];          // the keyboard's line alone can be 64 characters

    // Ten rows in a box 100 tall: headers 10 px, rows 20. 1 is the first that
    // can be chosen.
    ui_rows_t rows = { 10, 0, 100, 20, th_height, th_pick, nullptr };
    ui_list_t l = { 0, 0 };

    ui_list_move(&l, &rows, 0);
    const int first = l.sel;
    ui_list_move(&l, &rows, 1);
    const int second = l.sel;
    snprintf(d, sizeof(d), "step 0 -> %d, DOWN -> %d (2 is a header's neighbour)", first, second);
    ui_case("headers are skipped", first == 1 && second == 2, d);

    // Wrapping: DOWN from the last row that can be chosen comes back to 1,
    // and UP from 1 goes to the last (8).
    l.sel = 8; l.top = 0;
    ui_list_move(&l, &rows, 1);
    const int wrapped = l.sel;
    l.sel = 1;
    ui_list_move(&l, &rows, -1);
    const int back = l.sel;
    snprintf(d, sizeof(d), "DOWN from 8 -> %d, UP from 1 -> %d", wrapped, back);
    ui_case("the ends wrap", wrapped == 1 && back == 8, d);

    // Scrolling: row 8 cannot be reached with top at 0 (rows 0-8 are 10+20+20
    // +10+20+20+10+20+20 = 150 px), so the list scrolls just far enough.
    l.sel = 8; l.top = 0;
    ui_list_move(&l, &rows, 0);
    int last_y = 0, drawn = 0, sel_drawn = 0;
    for (ui_iter_t it = ui_list_begin(&l, &rows); ui_list_next(&it); ) {
        drawn++;
        last_y = it.y + it.h;
        if (it.i == l.sel) sel_drawn = 1;
    }
    snprintf(d, sizeof(d), "top %d, %d rows drawn, last ends at %d of 100", l.top, drawn, last_y);
    ui_case("scrolls to the chosen row", sel_drawn && last_y <= 100 && l.top > 0, d);

    // A row that would cross the bottom is not drawn at all - stage 5.4's bug
    // in Home, now impossible for anyone using this.
    ui_rows_t tight = { 4, 0, 50, 20, nullptr, nullptr, nullptr };
    ui_list_t t2 = { 0, 0 };
    int fits = 0;
    for (ui_iter_t it = ui_list_begin(&t2, &tight); ui_list_next(&it); ) fits++;
    const int bottom = ui_list_bottom(&t2, &tight);
    snprintf(d, sizeof(d), "%d of 4 rows of 20 in a box of 50, bottom %d", fits, bottom);
    ui_case("a row that half fits is not drawn", fits == 2 && bottom == 40, d);

    // Scrolling without a selection: the log's window, clamped at both ends.
    int top = 0;
    ui_scroll(&top, 30, 19, -1);
    const int at_top = top;
    top = 0;
    for (int i = 0; i < 20; ++i) ui_scroll(&top, 30, 19, 1);
    snprintf(d, sizeof(d), "UP at the top %d, 20 x DOWN over 30 lines -> %d", at_top, top);
    ui_case("scrolling clamps", at_top == 0 && top == 11, d);

    // The bar: 0 %, 50 % and past the end all come out as a width, never
    // outside the box.
    fb_t fb;
    bool ok_bar = false;
    if (fb_init(&fb, 64, 16)) {
        fb_clear(&fb, FB_BLACK);
        ui_bar(&fb, 0, 0, 52, 12, 200, 0, 100, FB_GREY, FB_YELLOW, FB_BLACK);   // over the top
        const int full_px = count_colour(&fb, FB_YELLOW);
        fb_clear(&fb, FB_BLACK);
        ui_bar(&fb, 0, 0, 52, 12, 50, 0, 100, FB_GREY, FB_YELLOW, FB_BLACK);
        const int half_px = count_colour(&fb, FB_YELLOW);
        fb_clear(&fb, FB_BLACK);
        ui_bar(&fb, 0, 0, 52, 12, -5, 0, 100, FB_GREY, FB_YELLOW, FB_BLACK);    // under it
        const int none_px = count_colour(&fb, FB_YELLOW);
        ok_bar = full_px == 50 * 10 && half_px == 25 * 10 && none_px == 0;
        snprintf(d, sizeof(d), "200%% %d px, 50%% %d px, -5%% %d px", full_px, half_px, none_px);
        fb_free(&fb);
    } else {
        snprintf(d, sizeof(d), "no framebuffer");
    }
    ui_case("the bar clamps", ok_bar, d);

    // Confirmation: the same key twice does it, a different subject re-arms
    // rather than firing, and anything else cancels.
    ui_confirm_t cf = { 0, false };
    char msg[64] = "";
    const bool first_press = ui_confirm(&cf, 7, "RIGHT", "kill spin", msg, sizeof(msg));
    char armed[64];
    snprintf(armed, sizeof(armed), "%s", msg);
    const bool other = ui_confirm(&cf, 9, "RIGHT", "kill hog", msg, sizeof(msg));
    const bool done = ui_confirm(&cf, 9, "RIGHT", "kill hog", msg, sizeof(msg));
    const bool cancelled = ui_confirm_cancel(&cf, msg, sizeof(msg), "not killed");
    snprintf(d, sizeof(d), "\"%s\", other %d, second %d, cancel %d", armed, other, done, cancelled);
    ui_case("two presses, same subject", !first_press && !other && done && !cancelled &&
            strcmp(armed, "RIGHT again to kill spin") == 0, d);

    // --- the keyboard (stage 6.4a) -------------------------------------------
    // Driven the way an app drives it: a button mask and an event kind.
    {
        ui_kb_t kb;
        ui_kb_init(&kb, nullptr);
        const char start = ui_kb_current(&kb);

        // Two letters off the home row: OK types what the cursor is on.
        ui_kb_event(&kb, BTN_OK, INPUT_PRESS);
        ui_kb_event(&kb, BTN_RIGHT, INPUT_PRESS);
        ui_kb_event(&kb, BTN_OK, INPUT_PRESS);
        snprintf(d, sizeof(d), "starts on '%c', typed \"%s\"", start, kb.text);
        ui_case("it types what it points at", start == 'a' && strcmp(kb.text, "as") == 0, d);

        // A held arrow keeps moving; a held OK does not keep typing.
        const uint8_t col_before = kb.col;
        ui_kb_event(&kb, BTN_RIGHT, INPUT_REPEAT);
        ui_kb_event(&kb, BTN_RIGHT, INPUT_REPEAT);
        const uint8_t col_after = kb.col;
        ui_kb_event(&kb, BTN_OK, INPUT_REPEAT);
        snprintf(d, sizeof(d), "column %u -> %u on two repeats; text still \"%s\"",
                 col_before, col_after, kb.text);
        ui_case("held arrows move, held OK does not", col_after == col_before + 2 &&
                strcmp(kb.text, "as") == 0, d);

        // The edges wrap, so no corner is more than half a grid away.
        ui_kb_init(&kb, nullptr);
        ui_kb_event(&kb, BTN_LEFT, INPUT_PRESS);
        const uint8_t wrapped_col = kb.col;
        ui_kb_event(&kb, BTN_UP, INPUT_PRESS);
        ui_kb_event(&kb, BTN_UP, INPUT_PRESS);
        const uint8_t wrapped_row = kb.row;
        snprintf(d, sizeof(d), "LEFT from column 0 -> %u, UP twice from row 1 -> %u",
                 wrapped_col, wrapped_row);
        ui_case("the edges wrap", wrapped_col == UI_KB_COLS - 1 && wrapped_row == 4, d);

        // Shift, and back. The wide keys are two columns each, so the cursor
        // lands on a whole one.
        ui_kb_init(&kb, nullptr);
        for (int i = 0; i < 3; ++i) ui_kb_event(&kb, BTN_DOWN, INPUT_PRESS);   // to the wide row
        const char *on = ui_kb_current_name(&kb);
        ui_kb_event(&kb, BTN_OK, INPUT_PRESS);                                  // Shift
        for (int i = 0; i < 3; ++i) ui_kb_event(&kb, BTN_UP, INPUT_PRESS);      // back to the letters
        ui_kb_event(&kb, BTN_OK, INPUT_PRESS);
        snprintf(d, sizeof(d), "three downs land on \"%s\"; then typed \"%s\"", on, kb.text);
        ui_case("shift", strcmp(on, "Shift") == 0 && kb.shift && strcmp(kb.text, "A") == 0, d);

        // Delete, and the line's own limit.
        ui_kb_event(&kb, BTN_BACK, INPUT_PRESS);
        const bool emptied = kb.len == 0;
        ui_kb_init(&kb, nullptr);
        for (int i = 0; i < UI_KB_MAX + 8; ++i) ui_kb_event(&kb, BTN_OK, INPUT_PRESS);
        snprintf(d, sizeof(d), "BACK emptied it: %s; %u typed of %d, full flag %d",
                 emptied ? "yes" : "no", kb.len, UI_KB_MAX + 8, (int)kb.full);
        ui_case("delete, and a line that is full", emptied && kb.len == UI_KB_MAX && kb.full &&
                strlen(kb.text) == UI_KB_MAX, d);
    }
}
