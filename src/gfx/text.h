#pragma once
#include "gfx/fb.h"

#include "smartos_config.h"

// ---------------------------------------------------------------------------
// Stage 3.5 - text.
//
// A 5x8 bitmap font drawn in a 6x10 cell, scaled by whole numbers. At scale 1
// that is 53 columns by 24 lines on a 320x240 panel; at scale 2, 26 by 12.
// The glyphs live in tools/font5x8.txt as pictures and are compiled into
// font5x8.h by tools/gen_font.py - edit the pictures, never the header.
//
// ONE DAMAGE REGION PER CALL
//
// Text writes pixels directly rather than through fb_pixel(), and that is the
// design decision in this file, not an optimisation. fb_pixel() marks damage
// per pixel, so a forty-character string would feed some 2,400 one-pixel
// rectangles through the merge logic in stage 3.3 - correct in the end, and
// thousands of times more work than the drawing itself. Here the whole string
// is marked once, as its bounding box.
//
// OPAQUE OR TRANSPARENT
//
// fb_text_bg() paints the whole cell, gaps included, so rewriting a number in
// place leaves nothing of the previous one behind - the normal case for a
// status line or a counter. fb_text() paints ink only, for text over a picture;
// changing it means redrawing whatever was underneath first.
//
// ฟอนต์บิตแมป 5x8 วาดในช่อง 6x10 ขยายด้วยจำนวนเต็ม ที่ scale 1 ได้ 53 คอลัมน์
// 24 บรรทัดบนจอ 320x240 ตัวอักษรอยู่ใน tools/font5x8.txt เป็นรูป แล้วถูกคอมไพล์
// เป็น font5x8.h ด้วย tools/gen_font.py — แก้ที่รูป ห้ามแก้ที่เฮดเดอร์
//
// **หนึ่ง damage region ต่อการเรียกหนึ่งครั้ง** ข้อความเขียนพิกเซลตรงๆ ไม่ผ่าน
// fb_pixel() และนี่คือการตัดสินใจเชิงออกแบบ ไม่ใช่การปรับแต่ง เพราะ fb_pixel()
// ทำเครื่องหมาย damage ทีละพิกเซล สตริงสี่สิบตัวอักษรจะกลายเป็นสี่เหลี่ยมขนาด
// หนึ่งพิกเซลราวสองพันสี่ร้อยอัน
// ---------------------------------------------------------------------------

// TEXT_CELL_W, TEXT_CELL_H and TEXT_MAX_SCALE are in smartos_config.h,
// section 16.

// Both return the x just past the last character drawn, so calls can be
// chained along a line. '\n' returns to the starting x one line down.
// Characters outside printable ASCII draw as '?'. Scale is clamped to
// 1..TEXT_MAX_SCALE.
int fb_text(fb_t *fb, int x, int y, const char *s, uint16_t fg, int scale);
int fb_text_bg(fb_t *fb, int x, int y, const char *s,
               uint16_t fg, uint16_t bg, int scale);

// Size of the box a string will occupy - its longest line, and every line.
int fb_text_width(const char *s, int scale);
int fb_text_height(const char *s, int scale);
