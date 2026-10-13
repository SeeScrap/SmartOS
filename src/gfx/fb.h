#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// Stage 3.2 - the framebuffer everything draws into.
//
// 320x240 RGB565 in PSRAM: 153,600 bytes out of eight megabytes, so the cost is
// irrelevant and the win is that drawing never touches the panel. Nothing here
// talks SPI; fb_present and later the compositor are the only things that do.
//
// PIXELS ARE STORED IN WIRE ORDER
//
// The panel wants RGB565 high byte first. A uint16_t on this chip is little
// endian, so the obvious layout would need every pixel byte-swapped on the way
// out - 76,800 swaps for a full frame, on the critical path, every frame.
//
// Storing them already swapped moves that work to the draw calls instead, where
// it happens once per colour rather than once per pixel: fb_rgb() does the swap,
// a fill of ten thousand pixels does it once. What comes out the other end is
// bytes the panel can take unmodified, which is what lets a flush be a straight
// DMA out of PSRAM with no conversion pass in the middle.
//
// The consequence to remember: a value in fb->px is NOT a normal RGB565 number.
// Build colours with fb_rgb() or the FB_* constants, never by hand.
//
// --- ภาษาไทย ---------------------------------------------------------------
//
// stage 3.2 - framebuffer ที่ทุกอย่างวาดลงไป
//
// ขนาด 320x240 แบบ RGB565 อยู่ใน PSRAM กินพื้นที่ 153,600 ไบต์จากแปดเมกะไบต์
// ต้นทุนจึงแทบไม่มีความหมาย และสิ่งที่ได้คือการวาดไม่ต้องแตะจอเลย ไม่มีอะไรในนี้
// คุยกับ SPI มีแค่ fb_present และตัวประกอบภาพในภายหลังเท่านั้นที่คุย
//
// พิกเซลถูกเก็บในลำดับไบต์แบบที่สายส่งต้องการ
//
// จอต้องการ RGB565 โดยให้ไบต์สูงมาก่อน แต่ uint16_t บนชิปนี้เป็น little endian
// การเก็บแบบตรงไปตรงมาจึงต้องสลับไบต์ทุกพิกเซลตอนส่งออก - 76,800 ครั้งต่อหนึ่ง
// เฟรมเต็มจอ บนเส้นทางที่วิกฤตที่สุด ทุกเฟรม
//
// การเก็บแบบสลับไว้ตั้งแต่แรกย้ายงานนั้นไปอยู่ที่ตอนวาดแทน ซึ่งเกิดครั้งเดียวต่อสี
// ไม่ใช่ครั้งเดียวต่อพิกเซล - fb_rgb() เป็นตัวสลับ การถมสีหนึ่งหมื่นพิกเซลจึงสลับ
// แค่ครั้งเดียว สิ่งที่ออกไปอีกฝั่งคือไบต์ที่จอรับไปใช้ได้เลยโดยไม่ต้องแปลง
// นั่นคือสิ่งที่ทำให้การส่งออกจอเป็น DMA ตรงจาก PSRAM โดยไม่มีขั้นตอนแปลงคั่น
//
// ผลที่ต้องจำไว้: ค่าใน fb->px ไม่ใช่เลข RGB565 ปกติ ให้สร้างสีด้วย fb_rgb()
// หรือค่าคงที่ FB_* เท่านั้น ห้ามประกอบเอง
// ---------------------------------------------------------------------------

typedef struct {
    int16_t x, y, w, h;
} fb_rect_t;

// How many separate damaged regions are tracked before they start being merged
// together. Small on purpose: the list is walked on every draw call, and beyond
// a handful of rectangles the walk costs more than the transfers saved.
#define FB_DAMAGE_MAX 8

typedef struct {
    uint16_t *px;       // w*h pixels, wire order
    uint16_t  w;
    uint16_t  h;
    size_t    bytes;

    // What has changed since the last flush. Lives here rather than in a
    // separate structure so that a draw call needs no allocation and no lookup.
    fb_rect_t damage[FB_DAMAGE_MAX];
    uint8_t   ndamage;
    bool      all_damaged;

    // Where row 0 of this buffer sits on the panel. 0 for a whole screen; a
    // view made by fb_view() starts lower down. Only flushing looks at it -
    // drawing is always in the buffer's own coordinates.
    uint16_t  oy;
} fb_t;

// Packs to RGB565 and swaps to wire order in one step.
static inline uint16_t fb_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    uint16_t c = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
    return (uint16_t)((c >> 8) | (c << 8));
}

#define FB_BLACK   fb_rgb(0,   0,   0)
#define FB_WHITE   fb_rgb(255, 255, 255)
#define FB_RED     fb_rgb(255, 0,   0)
#define FB_GREEN   fb_rgb(0,   255, 0)
#define FB_BLUE    fb_rgb(0,   0,   255)
#define FB_YELLOW  fb_rgb(255, 255, 0)
#define FB_CYAN    fb_rgb(0,   255, 255)
#define FB_MAGENTA fb_rgb(255, 0,   255)
#define FB_GREY    fb_rgb(128, 128, 128)

// Allocates w*h pixels in PSRAM. False if there is not enough room.
bool fb_init(fb_t *fb, uint16_t w, uint16_t h);

// A window onto rows [y, y+h) of parent, sharing its pixels. Full width only,
// which is what lets it reuse every primitive unchanged: a row of the view is
// a row of the parent, so the stride is still w. Its damage record is its
// own, and every primitive clips to it - so a view is also a fence. Stage 4.3b
// hands one to apps for the area below the status bar.
void fb_view(fb_t *view, const fb_t *parent, uint16_t y, uint16_t h);
void fb_free(fb_t *fb);

// Every one of these clips to the framebuffer, so a caller may pass anything -
// negative origins, oversized rectangles, coordinates entirely off-screen - and
// get the sane result rather than a corrupted heap.
void fb_clear(fb_t *fb, uint16_t color);
void fb_pixel(fb_t *fb, int x, int y, uint16_t color);
void fb_fill_rect(fb_t *fb, int x, int y, int w, int h, uint16_t color);
// Stage 6.2: mixes `color` into what is already there, `pct` of the way. For
// drawing over a picture - a selected row that lets the background show
// through - which a solid fill cannot do. Costs a read and a little
// arithmetic per pixel, so it is for small areas, not whole screens.
void fb_blend_rect(fb_t *fb, int x, int y, int w, int h, uint16_t color, uint8_t pct);

void fb_hline(fb_t *fb, int x, int y, int w, uint16_t color);
void fb_vline(fb_t *fb, int x, int y, int h, uint16_t color);
void fb_rect(fb_t *fb, int x, int y, int w, int h, uint16_t color);   // outline
void fb_blit(fb_t *fb, int x, int y, int w, int h, const uint16_t *src);

// Stage 4.3c, for apps that plot and games that draw. Like text, these write
// pixels directly and mark ONE damage region - the shape's bounding box - not
// one per pixel, which would push hundreds of one-pixel rectangles through
// the merge logic for a single diagonal line.
void fb_line(fb_t *fb, int x0, int y0, int x1, int y1, uint16_t color);
void fb_circle(fb_t *fb, int cx, int cy, int r, uint16_t color);        // outline
void fb_fill_circle(fb_t *fb, int cx, int cy, int r, uint16_t color);

// Pushes the whole framebuffer to the panel, changed or not, and clears the
// damage record with it - everything outstanding has just been sent.
void fb_present(fb_t *fb);

// ---------------------------------------------------------------------------
// Damage tracking.
//
// Every primitive above records the region it touched. fb_flush then sends only
// those regions, which is the whole point of the compositor: a full frame is
// 25 ms, and almost nothing an interface does is a full frame.
//
// WHEN TWO REGIONS BECOME ONE
//
// Entirely a cost question - see should_merge() in fb.cpp for the arithmetic
// and for the mistake that first version made. In short: a transfer carries
// about 41 us of fixed overhead, which at the panel's 6.1 MB/s is worth about
// 125 pixels, so two regions merge when their union adds fewer than that, or
// when it inflates the transfer by less than a quarter.
//
// Overlap gets no special treatment. Regions are read out of the framebuffer
// at flush time, so two overlapping regions send identical bytes for the pixels
// they share - it costs time, never correctness.
// ---------------------------------------------------------------------------
void        fb_damage(fb_t *fb, int x, int y, int w, int h);
void        fb_damage_all(fb_t *fb);
void        fb_damage_reset(fb_t *fb);
int         fb_damage_count(const fb_t *fb);
size_t      fb_damage_pixels(const fb_t *fb);
const fb_rect_t *fb_damage_at(const fb_t *fb, int i);

// Sends only what changed, then clears the record.
void        fb_flush(fb_t *fb);
