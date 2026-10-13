#include "hal/lcd_panel.h"

#include <Arduino.h>
#include <string.h>

#include "board/pins.h"
#include "hal/lcd_bus.h"
#include "hal/log.h"

// --- init tables -----------------------------------------------------------
// Encoding: cmd, count, payload...   count bit 0x80 means "delay 150 ms after".
// A count of 0xFF terminates the table.
#define TBL_END   0xFF
#define TBL_DELAY 0x80

static const uint8_t init_ili9488[] = {
    0xE0, 15, 0x00,0x03,0x09,0x08,0x16,0x0A,0x3F,0x78,0x4C,0x09,0x0A,0x08,0x16,0x1A,0x0F,
    0xE1, 15, 0x00,0x16,0x19,0x03,0x0F,0x05,0x32,0x45,0x46,0x04,0x0E,0x0D,0x35,0x37,0x0F,
    0xC0,  2, 0x17,0x15,              // power control 1
    0xC1,  1, 0x41,                   // power control 2
    0xC5,  3, 0x00,0x12,0x80,         // VCOM
    0x3A,  1, 0x66,                   // pixel format: 18-bit, the only one SPI takes
    0xB0,  1, 0x80,                   // interface mode
    0xB1,  1, 0xA0,                   // frame rate ~60 Hz
    0xB4,  1, 0x02,                   // display inversion control
    0xB6,  2, 0x02,0x02,              // display function control
    0xE9,  1, 0x00,
    0xF7,  4, 0xA9,0x51,0x2C,0x82,    // adjust control 3
    0x11,  0 | TBL_DELAY,             // sleep out
    0x29,  0 | TBL_DELAY,             // display on
    TBL_END
};

static const uint8_t init_ili9486[] = {
    0x3A,  1, 0x66,
    0xC0,  2, 0x0E,0x0E,
    0xC1,  2, 0x41,0x00,
    0xC2,  1, 0x55,
    0xC5,  4, 0x00,0x00,0x00,0x00,
    0xE0, 15, 0x0F,0x1F,0x1C,0x0C,0x0F,0x08,0x48,0x98,0x37,0x0A,0x13,0x04,0x11,0x0D,0x00,
    0xE1, 15, 0x0F,0x32,0x2E,0x0B,0x0D,0x05,0x47,0x75,0x37,0x06,0x10,0x03,0x24,0x20,0x00,
    0xB6,  3, 0x00,0x22,0x3B,
    0x11,  0 | TBL_DELAY,
    0x29,  0 | TBL_DELAY,
    TBL_END
};

static const uint8_t init_st7796[] = {
    0xF0,  1, 0xC3,                   // unlock command set
    0xF0,  1, 0x96,
    0x3A,  1, 0x55,                   // ST7796 does take 16-bit over SPI - half the traffic
    0xB4,  1, 0x01,
    0xB7,  1, 0xC6,
    0xC0,  2, 0x80,0x45,
    0xC1,  1, 0x13,
    0xC2,  1, 0xA7,
    0xC5,  1, 0x0A,
    0xE8,  8, 0x40,0x8A,0x00,0x00,0x29,0x19,0xA5,0x33,
    0xF0,  1, 0x3C,                   // lock command set
    0xF0,  1, 0x69,
    0x11,  0 | TBL_DELAY,
    0x29,  0 | TBL_DELAY,
    TBL_END
};

// --- state -----------------------------------------------------------------
static lcd_ctrl_t s_ctrl   = LCD_CTRL_UNKNOWN;
static uint8_t    s_bpp    = 3;
static uint8_t    s_rot    = 1;
static uint16_t   s_w      = LCD_WIDTH;
static uint16_t   s_h      = LCD_HEIGHT;
static uint8_t   *s_line   = nullptr;    // one row of wire-format pixels, DMA-capable
static size_t     s_line_sz = 0;

static void run_table(const uint8_t *t)
{
    while (*t != TBL_END) {
        uint8_t cmd   = *t++;
        uint8_t count = *t++;
        uint8_t n     = count & 0x7F;
        lcd_cmd_p(cmd, t, n);
        t += n;
        if (count & TBL_DELAY) delay(150);
    }
}

static void hw_reset(void)
{
    digitalWrite(PIN_LCD_RST, HIGH); delay(10);
    digitalWrite(PIN_LCD_RST, LOW);  delay(20);
    digitalWrite(PIN_LCD_RST, HIGH); delay(150);
}

// MADCTL bits: MY 0x80, MX 0x40, MV 0x20, BGR 0x08
static uint8_t madctl_for(uint8_t rot)
{
    switch (rot & 3) {
        case 0:  return 0x48;   // portrait  320x480
        case 1:  return 0x28;   // landscape 480x320
        case 2:  return 0x88;   // portrait  flipped
        default: return 0xE8;   // landscape flipped
    }
}

bool lcd_panel_init(lcd_ctrl_t ctrl)
{
    s_ctrl = ctrl;

    hw_reset();

    switch (ctrl) {
        case LCD_CTRL_ILI9486: run_table(init_ili9486); s_bpp = 3; break;
        case LCD_CTRL_ST7796:  run_table(init_st7796);  s_bpp = 2; break;
        case LCD_CTRL_ILI9488: run_table(init_ili9488); s_bpp = 3; break;
        default:
            // The module is labelled 480x320 and that family is almost always
            // ILI9488, so try it rather than refusing to boot.
            logln("[lcd] controller unknown - assuming ILI9488");
            run_table(init_ili9488);
            s_bpp = 3;
            break;
    }

    lcd_set_rotation(s_rot);

    size_t need = (size_t)LCD_WIDTH * 3;      // worst case: widest axis, 3 bytes/px
    if (s_line_sz < need) {
        if (s_line) free(s_line);
        s_line = (uint8_t *)lcd_dma_alloc(need);
        s_line_sz = s_line ? need : 0;
    }
    if (!s_line) {
        logln("[lcd] FATAL: no DMA-capable memory for the line buffer");
        return false;
    }
    return true;
}

void lcd_set_rotation(uint8_t rot)
{
    s_rot = rot & 3;
    uint8_t m = madctl_for(s_rot);
    lcd_cmd_p(0x36, &m, 1);

    if (s_rot & 1) { s_w = 480; s_h = 320; }
    else           { s_w = 320; s_h = 480; }
}

uint16_t lcd_width(void)          { return s_w; }
uint16_t lcd_height(void)         { return s_h; }
uint8_t  lcd_bytes_per_pixel(void){ return s_bpp; }

void lcd_backlight(bool on)
{
    digitalWrite(PIN_LCD_BL, on ? HIGH : LOW);
}

void lcd_set_window(uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
    uint16_t x1 = x + w - 1, y1 = y + h - 1;
    uint8_t buf[4];

    buf[0] = x  >> 8; buf[1] = x  & 0xFF;
    buf[2] = x1 >> 8; buf[3] = x1 & 0xFF;
    lcd_cmd_p(0x2A, buf, 4);                 // column address set

    buf[0] = y  >> 8; buf[1] = y  & 0xFF;
    buf[2] = y1 >> 8; buf[3] = y1 & 0xFF;
    lcd_cmd_p(0x2B, buf, 4);                 // page address set

    lcd_cmd(0x2C);                           // memory write
}

// RGB565 -> whatever the panel wants on the wire.
static inline size_t encode_run(uint8_t *dst, uint16_t color, uint16_t count)
{
    if (s_bpp == 2) {
        for (uint16_t i = 0; i < count; ++i) {
            *dst++ = color >> 8;
            *dst++ = color & 0xFF;
        }
        return count * 2;
    }
    // 18-bit: the panel uses the top 6 bits of each byte, so the RGB565
    // channels can be shifted straight up with no lookup table.
    uint8_t r = (color >> 8) & 0xF8;
    uint8_t g = (color >> 3) & 0xFC;
    uint8_t b = (color << 3) & 0xF8;
    for (uint16_t i = 0; i < count; ++i) {
        *dst++ = r; *dst++ = g; *dst++ = b;
    }
    return count * 3;
}

void lcd_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color)
{
    if (!s_line || w == 0 || h == 0) return;
    if (x >= s_w || y >= s_h) return;
    if (x + w > s_w) w = s_w - x;
    if (y + h > s_h) h = s_h - y;

    lcd_set_window(x, y, w, h);

    size_t row_bytes = encode_run(s_line, color, w);
    for (uint16_t row = 0; row < h; ++row) {
        lcd_data(s_line, row_bytes);         // same row over and over
    }
}

void lcd_fill_screen(uint16_t color)
{
    lcd_fill_rect(0, 0, s_w, s_h, color);
}

void lcd_push_pixels(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                     const uint16_t *src)
{
    if (!s_line || !src || w == 0 || h == 0) return;
    if (x + w > s_w || y + h > s_h) return;

    lcd_set_window(x, y, w, h);

    for (uint16_t row = 0; row < h; ++row) {
        const uint16_t *p = src + (size_t)row * w;
        uint8_t *d = s_line;
        if (s_bpp == 2) {
            for (uint16_t i = 0; i < w; ++i) { *d++ = p[i] >> 8; *d++ = p[i] & 0xFF; }
        } else {
            for (uint16_t i = 0; i < w; ++i) {
                uint16_t c = p[i];
                *d++ = (c >> 8) & 0xF8;
                *d++ = (c >> 3) & 0xFC;
                *d++ = (c << 3) & 0xF8;
            }
        }
        lcd_data(s_line, (size_t)w * s_bpp);
    }
}
