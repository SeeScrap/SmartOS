#pragma once
#include <stdint.h>
#include "board/lcd_probe.h"

#define RGB565(r, g, b) \
    ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

#define COLOR_BLACK   RGB565(0,   0,   0)
#define COLOR_WHITE   RGB565(255, 255, 255)
#define COLOR_RED     RGB565(255, 0,   0)
#define COLOR_GREEN   RGB565(0,   255, 0)
#define COLOR_BLUE    RGB565(0,   0,   255)
#define COLOR_YELLOW  RGB565(255, 255, 0)
#define COLOR_CYAN    RGB565(0,   255, 255)
#define COLOR_MAGENTA RGB565(255, 0,   255)

// Brings the panel out of reset and into a usable state.
// Pass LCD_CTRL_UNKNOWN to fall back to the ILI9488 sequence.
bool     lcd_panel_init(lcd_ctrl_t ctrl);

void     lcd_set_rotation(uint8_t rot);   // 0,2 = 320x480 portrait; 1,3 = 480x320 landscape
uint16_t lcd_width(void);
uint16_t lcd_height(void);

// Bytes actually sent per pixel. ILI9486/9488 accept only 18-bit RGB666 over
// 4-wire SPI, so this is 3 for them and 2 for ST7796. It is the number that
// decides our frame rate ceiling, so the OS above needs to see it.
uint8_t  lcd_bytes_per_pixel(void);

void     lcd_backlight(bool on);

// Stage 4.5: brightness, 0-100 %, by PWM on the backlight pin (LEDC, 5 kHz -
// well above flicker, well below what the module's backlight transistor can
// follow). Core 0 only: it uses the LEDC driver. lcd_backlight() is this at
// 0 or 100.
void     lcd_backlight_pct(uint8_t pct);
uint32_t lcd_backlight_duty(void);    // the duty actually set, 0-1023, for tests

void     lcd_set_window(uint16_t x, uint16_t y, uint16_t w, uint16_t h);
void     lcd_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color);
void     lcd_fill_screen(uint16_t color);

// Streams an RGB565 source buffer (which may live in PSRAM) into a window,
// converting to the panel's wire format on the way out.
void     lcd_push_pixels(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                         const uint16_t *src);

// Pushes bytes that are ALREADY in the panel's wire order - no conversion, just
// movement. This is the path the framebuffer uses; see the note in gfx/fb.h for
// why it stores pixels pre-swapped.
//
// stride_bytes is the distance between rows in the source, so a rectangle can
// be taken out of a larger image without copying it first.
//
// The two versions differ only in whether the bytes are staged through internal
// RAM on the way to the DMA engine. Direct is obviously preferable if the DMA
// engine will read PSRAM at all - which is exactly what stage 3.2 measures.
void     lcd_blit_wire(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                       const uint8_t *src, size_t stride_bytes);
void     lcd_blit_wire_direct(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                              const uint8_t *src, size_t stride_bytes);
