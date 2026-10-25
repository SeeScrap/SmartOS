#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gfx/fb.h"
#include "gfx/image.h"

#include "smartos_config.h"

// ---------------------------------------------------------------------------
// Stage 5.4c - the pieces every app's screen is made of.
//
// Four apps had grown their own copy of the same three things: a list the user
// moves through with UP and DOWN, a bar showing a value against its range, and
// an action that asks before it does something it cannot undo. The copies had
// already drifted - Home skipped headers and scrolled, Tasks wrapped with a
// modulo and could not scroll at all, and the bug fixed in 5.4 (a row drawn
// past the bottom of the list) existed only in Home's copy, so nobody could
// tell whether the others had it too.
//
// This is that code, once. It draws nothing an app has not asked for: an app
// still owns its own screen, and these only take the parts that were the same
// in all of them. Nothing here talks to the kernel, so it is safe from core 1
// and core 0 alike; the app area's background colour comes from app_bg().
//
// WHAT IT IS NOT
//
// Not a widget tree, not a layout engine and not a theme. An app that wants
// something else draws it itself - see the graphs and composition bars on the
// task manager's Performance page, which are its own and stay that way.
//
// --- ภาษาไทย ---------------------------------------------------------------
//
// stage 5.4c - ชิ้นส่วนที่ประกอบเป็นหน้าจอของทุกแอป
//
// มีสี่แอปที่ต่างคนต่างเขียนของสามอย่างเดียวกันขึ้นมาเอง: รายการที่ผู้ใช้เลื่อนด้วย
// ปุ่มขึ้นลง แถบแสดงค่าเทียบกับช่วงของมัน และการกระทำที่ถามก่อนทำสิ่งที่ย้อนไม่ได้
// สำเนาเหล่านั้นเริ่มไม่ตรงกันแล้ว - Home ข้ามหัวข้อและเลื่อนรายการได้ ส่วน Tasks
// ใช้การหารเอาเศษและเลื่อนไม่ได้เลย และบั๊กที่แก้ไปใน 5.4 (แถวที่ถูกวาดเลยขอบ
// ล่างของรายการ) มีอยู่เฉพาะในสำเนาของ Home จึงไม่มีใครบอกได้ว่าตัวอื่นเป็นด้วย
// หรือเปล่า
//
// นี่คือโค้ดชุดนั้น เขียนไว้ชุดเดียว มันไม่วาดอะไรที่แอปไม่ได้สั่ง - แอปยังเป็น
// เจ้าของหน้าจอตัวเอง ส่วนนี้รับไปเฉพาะชิ้นที่เหมือนกันในทุกแอปเท่านั้น
// ไม่มีอะไรในนี้คุยกับเคอร์เนล จึงเรียกได้ปลอดภัยทั้งจาก core 1 และ core 0
// ส่วนสีพื้นหลังของพื้นที่แอปมาจาก app_bg()
//
// สิ่งที่มันไม่ใช่
//
// ไม่ใช่ระบบ widget ไม่ใช่ตัวจัดวางหน้าจอ และไม่ใช่ชุดธีม แอปที่อยากได้อย่างอื่น
// ให้วาดเอง - ดูกราฟและแถบองค์ประกอบในหน้า Performance ของตัวจัดการงาน
// ซึ่งเป็นของมันเองและจะเป็นแบบนั้นต่อไป
// ---------------------------------------------------------------------------

// --- a list the user moves through -----------------------------------------

// Where the list is on the screen, and what is in it. `height` and
// `selectable` are optional: without them every row is row_h tall and can be
// chosen. With them a list can hold headers, like Home's "Running" and "Apps".
typedef struct {
    int   n;                                 // rows in the list
    int   y, y_end;                          // the box it is drawn in
    int   row_h;                             // an ordinary row's height
    int (*height)(void *ctx, int i);         // null: row_h for every row
    bool (*selectable)(void *ctx, int i);    // null: every row can be chosen
    void *ctx;                               // handed back to both
} ui_rows_t;

// The app's own state: which row is chosen, and which is drawn first. Keep it
// in the app's state_t - the rows themselves are usually rebuilt each pass.
typedef struct {
    int sel;
    int top;
} ui_list_t;

// Moves the selection by step (-1 for UP, +1 for DOWN), wrapping at the ends,
// skipping rows that cannot be chosen, and scrolls so the chosen row fits
// inside the box. step 0 only puts things right - call it when the rows have
// changed underneath.
void ui_list_move(ui_list_t *l, const ui_rows_t *rows, int step);

// Walks the rows that fit in the box, the first drawn one first:
//
//     for (ui_iter_t it = ui_list_begin(&s->list, &rows); ui_list_next(&it); )
//         ... it.i, it.y, it.h ...
//
// A row that would cross y_end ends the walk - it is not drawn at all, which
// is the bug stage 5.4 found in Home.
typedef struct {
    const ui_rows_t *rows;
    int i, y, h;
    bool started;
} ui_iter_t;

ui_iter_t ui_list_begin(const ui_list_t *l, const ui_rows_t *rows);
bool      ui_list_next(ui_iter_t *it);

// Where the last drawn row ended - what is between this and y_end is the
// app's to clear.
int  ui_list_bottom(const ui_list_t *l, const ui_rows_t *rows);

// Scrolling without a selection, for a window of `visible` rows onto `n` -
// the system log. Clamps; does not wrap.
void ui_scroll(int *top, int n, int visible, int step);

// --- drawing ----------------------------------------------------------------

uint16_t ui_hl(void);                        // the chosen row's background

// What an area sits on: a picture, or null for the flat app background
// colour. Stage 6.2 gave Home a wallpaper; everything that paints an area
// takes one of these, so an app with a picture behind it and an app without
// use the same code, and a row drawn over a picture puts the picture back
// rather than a colour.
void ui_clear(fb_t *fb, const image_t *bg, int x, int y, int w, int h);

// A row's background, across the usual margin. The chosen row is the
// highlight colour - solid over a flat background, blended over a picture, so
// the picture still shows through it.
void ui_row_bg(fb_t *fb, const image_t *bg, int y, int h, bool selected);
// UI_SELECT_PCT is in smartos_config.h, section 18.

// Text with an edge under it, for text that sits on a picture.
//
// Grey names over a bright sky were unreadable the first time Home had a
// photograph behind them (stage 6.2) - and dimming the picture far enough to
// fix that made the picture not worth having. A shadow costs one extra pass of
// the same string, one pixel down and right in near-black, and works whatever
// the picture turns out to be. `shadow` false draws exactly what fb_text
// would, so an app on a flat background is unchanged.
int ui_text(fb_t *fb, int x, int y, const char *s, uint16_t colour, int scale, bool shadow);

// An app's title, top left, and a line of help in grey.
void ui_title(fb_t *fb, int y, const char *title);
void ui_help(fb_t *fb, int y, const char *line);

// The same two over a picture: a shadow, and a lighter grey for the help line
// than the one that reads well on the flat background.
void ui_title_on(fb_t *fb, int y, const char *title, bool shadow);
void ui_help_on(fb_t *fb, int y, const char *line, bool shadow);

// A value against its range: an outlined box filled from the left. `v` is
// clamped to lo..hi. The part not filled is painted `back`, so a bar that
// shrinks leaves nothing of the old one behind.
void ui_bar(fb_t *fb, int x, int y, int w, int h, int32_t v, int32_t lo, int32_t hi,
            uint16_t frame, uint16_t fill, uint16_t back);

// --- an action that cannot be undone ----------------------------------------
//
// The first press only arms it and says so; the second does it. Anything else
// cancels. `id` is what is being acted on - an app id in the task manager, and
// anything at all where there is only one thing to act on.
//
//     if (ui_confirm(&s->cf, id, "RIGHT", "kill spin", s->msg, sizeof(s->msg)))
//         app_kill_request(id);             // the second press
//
// writes "RIGHT again to kill spin" on the first press and returns false.

typedef struct {
    int  id;
    bool armed;
} ui_confirm_t;

bool ui_confirm(ui_confirm_t *cf, int id, const char *key, const char *what,
                char *msg, size_t msg_len);

// Disarms. True if anything was armed - so an app can leave its message alone
// when nothing was. `cancelled` may be null, which clears the message, and
// `msg` may be null too, which disarms and leaves whatever is there: the task
// manager's OK writes its own line and must keep it.
bool ui_confirm_cancel(ui_confirm_t *cf, char *msg, size_t msg_len, const char *cancelled);
