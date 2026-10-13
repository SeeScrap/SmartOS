#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "gfx/fb.h"

// ---------------------------------------------------------------------------
// Stage 6.4a - a keyboard for six buttons.
//
// Everything typed into this board so far came from a computer, down the USB
// cable, through the shell. The board itself had no way to spell a word: six
// buttons, and nothing that turned them into letters.
//
// This is the usual answer - a grid of keys with a cursor on it - built for
// the buttons that exist:
//
//   UP DOWN LEFT RIGHT   move the cursor, wrapping at the edges; held, they
//                        repeat, so crossing the grid is one long press
//   OK                   type the key under the cursor
//   BACK (a tap)         delete the character before the caret
//
// BACK is a tap on purpose: holding it still closes the app, which is the
// system's rule everywhere and not one a keyboard gets to break. There is a
// Del key on the grid as well, for anyone who would rather not risk the hold.
//
// The bottom row is five wide keys - Shift, Space, Del, Sym and Done - and the
// cursor moves across them two grid columns at a time, so up and down still
// line up with the letters above.
//
// WHAT IT IS NOT
//
// Not a text editor: one line, no cursor inside the text, no selection. An app
// that needs more than a word or a password needs more than this.
//
// --- ภาษาไทย ---------------------------------------------------------------
//
// stage 6.4a - แป้นพิมพ์สำหรับปุ่มหกปุ่ม
//
// ทุกตัวอักษรที่เคยพิมพ์ลงบอร์ดนี้มาจากคอมพิวเตอร์ ผ่านสาย USB ผ่านเชลล์ทั้งนั้น
// ตัวบอร์ดเองสะกดคำไม่ได้เลย - มีปุ่มหกปุ่ม และไม่มีอะไรแปลงปุ่มเป็นตัวอักษร
//
// นี่คือคำตอบที่ใช้กันทั่วไป - ตารางปุ่มที่มีเคอร์เซอร์วิ่งอยู่บนนั้น - สร้างให้เข้ากับ
// ปุ่มเท่าที่มี:
//
//   ลูกศรสี่ทิศ      เลื่อนเคอร์เซอร์ วนรอบเมื่อถึงขอบ กดค้างแล้วเลื่อนต่อเนื่อง
//                   การข้ามทั้งตารางจึงเป็นการกดค้างครั้งเดียว
//   OK              พิมพ์ปุ่มที่เคอร์เซอร์ชี้อยู่
//   BACK (แตะ)      ลบตัวอักษรก่อนหน้าเคอร์เซอร์
//
// BACK ใช้การแตะโดยตั้งใจ เพราะการกดค้างยังหมายถึงปิดแอป ซึ่งเป็นกฎของระบบ
// ที่ใช้ทุกที่ และไม่ใช่กฎที่แป้นพิมพ์จะมาแหกได้ ในตารางจึงมีปุ่ม Del ไว้ให้ด้วย
// สำหรับคนที่ไม่อยากเสี่ยงกดค้าง
//
// แถวล่างสุดเป็นปุ่มกว้างห้าปุ่ม - Shift, Space, Del, Sym, Done - เคอร์เซอร์เลื่อน
// ผ่านมันครั้งละสองช่องของตาราง การกดขึ้นลงจึงยังตรงกับตัวอักษรที่อยู่ข้างบน
//
// สิ่งที่มันไม่ใช่
//
// ไม่ใช่โปรแกรมแก้ข้อความ - มีบรรทัดเดียว เลื่อนเคอร์เซอร์เข้าไปกลางข้อความไม่ได้
// และเลือกข้อความไม่ได้ แอปที่ต้องการมากกว่าคำเดียวหรือรหัสผ่านหนึ่งอัน
// ต้องการมากกว่านี้
// ---------------------------------------------------------------------------

#define UI_KB_COLS   10
#define UI_KB_ROWS   5          // four rows of characters, then the wide keys
#define UI_KB_MAX    64         // characters it will hold, plus the terminator

typedef struct {
    char    text[UI_KB_MAX + 1];
    uint8_t len;
    uint8_t col, row;           // where the cursor is
    bool    shift;
    bool    sym;                // the second page
    bool    done;               // Done was pressed
    bool    full;               // the text is at UI_KB_MAX
} ui_kb_t;

// Starts empty, or with `start` already in it (a password being corrected, a
// search being narrowed). The cursor begins on the home row.
// เริ่มจากว่างเปล่า หรือมีข้อความ `start` อยู่ในนั้นแล้ว (รหัสผ่านที่กำลังแก้ คำค้นที่
// กำลังปรับ) เคอร์เซอร์เริ่มที่แถวกลาง
void ui_kb_init(ui_kb_t *kb, const char *start);

// One button event, as the app received it: `btn` is a BTN_* mask and `kind`
// an INPUT_* kind. True if anything changed and the screen should be redrawn.
// Press and repeat both move; only a press types, so holding OK does not fill
// the line with one letter.
// รับ event ของปุ่มหนึ่งครั้งตามที่แอปได้รับมา: `btn` คือ bitmask แบบ BTN_* และ
// `kind` คือชนิดแบบ INPUT_* คืนค่า true เมื่อมีอะไรเปลี่ยนและควรวาดจอใหม่
// ทั้งการกดและการกดค้างทำให้เคอร์เซอร์เลื่อน แต่มีเฉพาะการกดที่พิมพ์ตัวอักษร
// การกด OK ค้างจึงไม่ทำให้ทั้งบรรทัดเต็มไปด้วยตัวอักษรเดียว
bool ui_kb_event(ui_kb_t *kb, uint32_t btn, uint8_t kind);

// The whole thing: the line being typed with its caret, then the grid. `y` is
// where the text line goes; the grid is always at the bottom of the view.
// `prompt` is drawn above the line - "note", "password" - or null for none.
// วาดทั้งหมด: บรรทัดที่กำลังพิมพ์พร้อมเคอร์เซอร์ แล้วตามด้วยตารางปุ่ม `y` คือตำแหน่ง
// ของบรรทัดข้อความ ส่วนตารางอยู่ล่างสุดของพื้นที่เสมอ `prompt` คือคำที่วาดเหนือ
// บรรทัด เช่น "note" หรือ "password" ใส่ null ถ้าไม่ต้องการ
void ui_kb_draw(fb_t *fb, const ui_kb_t *kb, int y, const char *prompt);

// What is under the cursor right now, as it would be typed: for tests, and for
// an app that wants to say what OK will do.
// ตัวอักษรที่เคอร์เซอร์ชี้อยู่ตอนนี้ ในรูปแบบที่จะถูกพิมพ์ออกมา - ใช้ในการทดสอบ
// และสำหรับแอปที่อยากบอกผู้ใช้ว่ากด OK แล้วจะได้อะไร
char ui_kb_current(const ui_kb_t *kb);      // 0 on one of the wide keys
const char *ui_kb_current_name(const ui_kb_t *kb);   // "Shift", "q", "Space"
