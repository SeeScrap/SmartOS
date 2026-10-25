#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "gfx/fb.h"

// ---------------------------------------------------------------------------
// Stage 6.2 - pictures.
//
// A picture is width, height and pixels, in the panel's own wire order (see
// the note in fb.h), so drawing one is a row copy and nothing else - no
// decoding, no conversion, no allocation. tools/mkimage.py prepares them on a
// computer: scale, crop, dim, convert, and out comes a C file whose array
// lives in flash. Home's background is read from flash as it is drawn and
// never copied into RAM.
//
// WHY NOT A JPEG ON THE FILESYSTEM
//
// A decoder is code, working memory and time on every draw, for a picture that
// changes when the firmware does. When a picture has to be changeable without
// a rebuild - a photo the user chose - the same image_t can point at a buffer
// loaded from a file instead, and everything below still works.
//
// รูปคือความกว้าง ความสูง และพิกเซล เรียงตามลำดับสายของจอโดยตรง การวาดรูป
// จึงเป็นแค่การคัดลอกทีละแถว ไม่มีการถอดรหัส ไม่มีการแปลง ไม่มีการจองหน่วยความจำ
//
// tools/mkimage.py เตรียมรูปบนคอมพิวเตอร์: ย่อ ตัด หรี่ แปลง แล้วได้ไฟล์ C
// ที่อาเรย์อยู่ในแฟลช ภาพพื้นหลังของ Home ถูกอ่านจากแฟลชขณะวาด
// และไม่เคยถูกคัดลอกเข้า RAM เลย
// ---------------------------------------------------------------------------

typedef struct {
    uint16_t        w, h;
    const uint16_t *px;      // w*h pixels, wire order; usually flash
} image_t;

// The whole picture at (x, y), clipped to the framebuffer. Damage is marked
// for exactly what was drawn.
void image_blit(fb_t *fb, const image_t *img, int x, int y);

// One rectangle of it, taken from the same place in the picture - what a row
// uses to put the background back before drawing over it.
void image_blit_rect(fb_t *fb, const image_t *img, int x, int y, int w, int h);

// Is this picture usable? (A generated file always is; this is for the case
// where there is none.)
static inline bool image_ok(const image_t *img)
{
    return img && img->px && img->w && img->h;
}
