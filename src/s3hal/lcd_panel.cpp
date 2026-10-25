#include "s3hal/lcd_panel.h"

#include "driver/ledc.h"

#include "s3hal/gpio.h"
#include "s3hal/hal_time.h"
#include <stdlib.h>
#include <string.h>

#include "board/devkitc_n16r8.h"
#include "s3hal/lcd_bus.h"
#include "s3hal/log.h"

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

// 2.4" 240x320. Both of the controllers below accept 0x3A = 0x55, sixteen-bit
// RGB565, so neither needs the RGB666 conversion the ILI9488 forced on us.

static const uint8_t init_ili9341[] = {
    0xCF,  3, 0x00,0xC1,0x30,
    0xED,  4, 0x64,0x03,0x12,0x81,
    0xE8,  3, 0x85,0x00,0x78,
    0xCB,  5, 0x39,0x2C,0x00,0x34,0x02,
    0xF7,  1, 0x20,
    0xEA,  2, 0x00,0x00,
    0xC0,  1, 0x23,                   // power control 1
    0xC1,  1, 0x10,                   // power control 2
    0xC5,  2, 0x3E,0x28,              // VCOM 1
    0xC7,  1, 0x86,                   // VCOM 2
    0x3A,  1, 0x55,                   // 16-bit colour
    0xB1,  2, 0x00,0x18,              // frame rate
    0xB6,  3, 0x08,0x82,0x27,         // display function control
    0xF2,  1, 0x00,
    0x26,  1, 0x01,                   // gamma curve 1
    0xE0, 15, 0x0F,0x31,0x2B,0x0C,0x0E,0x08,0x4E,0xF1,0x37,0x07,0x10,0x03,0x0E,0x09,0x00,
    0xE1, 15, 0x00,0x0E,0x14,0x03,0x11,0x07,0x31,0xC1,0x48,0x08,0x0F,0x0C,0x31,0x36,0x0F,
    0x11,  0 | TBL_DELAY,             // sleep out
    0x29,  0 | TBL_DELAY,             // display on
    TBL_END
};

static const uint8_t init_st7789[] = {
    0x01,  0 | TBL_DELAY,             // software reset
    0x11,  0 | TBL_DELAY,             // sleep out
    0x3A,  1, 0x55,                   // 16-bit colour
    0x20,  0,                         // inversion OFF. Most ST7789 boards want
                                      // it ON (0x21) and this one does not -
                                      // with it on, black came out white and
                                      // every colour was its complement. If a
                                      // future panel looks like a negative,
                                      // this is the line, not the gamma tables.
    0xB2,  5, 0x0C,0x0C,0x00,0x33,0x33,   // porch control
    0xB7,  1, 0x35,                   // gate control
    0xBB,  1, 0x19,                   // VCOMS
    0xC0,  1, 0x2C,                   // LCM control
    0xC2,  1, 0x01,                   // VDV/VRH enable
    0xC3,  1, 0x12,                   // VRH set
    0xC4,  1, 0x20,                   // VDV set
    0xC6,  1, 0x0F,                   // frame rate, ~60 Hz
    0xD0,  2, 0xA4,0xA1,              // power control 1
    0xE0, 14, 0xD0,0x04,0x0D,0x11,0x13,0x2B,0x3F,0x54,0x4C,0x18,0x0D,0x0B,0x1F,0x23,
    0xE1, 14, 0xD0,0x04,0x0C,0x11,0x13,0x2C,0x3F,0x44,0x51,0x2F,0x1F,0x1F,0x20,0x23,
    0x29,  0 | TBL_DELAY,             // display on
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

// The BGR bit in MADCTL, or 0 for RGB order.
//
// Not a constant, because it is not a property of the command set - it is how
// the panel behind the controller happens to be wired. The ILI9xxx modules we
// started with are BGR; ST7789 boards are usually RGB. If red and blue come out
// swapped, this is the one line to change, and knowing that is worth more than
// getting it right first time.
static uint8_t    s_bgr    = 0x08;
static uint8_t    s_bpp    = 3;
static uint8_t    s_rot    = 1;
static uint16_t   s_w      = LCD_WIDTH;
static uint16_t   s_h      = LCD_HEIGHT;
static uint8_t   *s_line   = nullptr;    // one row of wire-format pixels, DMA-capable
static size_t     s_line_sz = 0;

// Stage 3.4's staging buffers - see the note above lcd_blit_wire for why there
// are two of them rather than one.
#define LCD_STAGE_BUFS 2
static uint8_t   *s_stage[LCD_STAGE_BUFS] = { nullptr, nullptr };

static void run_table(const uint8_t *t)
{
    while (*t != TBL_END) {
        uint8_t cmd   = *t++;
        uint8_t count = *t++;
        uint8_t n     = count & 0x7F;
        lcd_cmd_p(cmd, t, n);
        t += n;
        if (count & TBL_DELAY) hal_delay_ms(150);
    }
}

static void hw_reset(void)
{
    gpio_hi(PIN_LCD_RST); hal_delay_ms(10);
    gpio_lo(PIN_LCD_RST);  hal_delay_ms(20);
    gpio_hi(PIN_LCD_RST); hal_delay_ms(150);
}

// MADCTL bits: MY 0x80, MX 0x40, MV 0x20, BGR 0x08
static uint8_t madctl_for(uint8_t rot)
{
    // MY 0x80, MX 0x40, MV 0x20 - the orientation bits. Colour order is added
    // separately because it belongs to the board, not the rotation.
    // MX and MY mirror the panel's OWN axes - the native 240x320 portrait frame -
    // and MV exchanges them afterwards. So with MV set, the bit that flips what
    // the viewer sees as horizontal is MY, not MX. Reaching for the intuitive
    // one turned a left-right mirror into a 180 degree rotation, which is the
    // same mistake made twice rather than a new symptom.
    uint8_t m;
    switch (rot & 3) {
        case 0:  m = 0x00;      break;  // portrait  240x320
        case 1:  m = 0xA0;      break;  // landscape 320x240  (MV | MY)
        case 2:  m = 0xC0;      break;  // portrait  flipped  (MX | MY)
        default: m = 0x60;      break;  // landscape flipped  (MV | MX)
    }
    return (uint8_t)(m | s_bgr);
}

bool lcd_panel_init(lcd_ctrl_t ctrl)
{
    s_ctrl = ctrl;

    hw_reset();

    switch (ctrl) {
        case LCD_CTRL_ILI9486: run_table(init_ili9486); s_bpp = 3; break;
        case LCD_CTRL_ILI9488: run_table(init_ili9488); s_bpp = 3; break;
        case LCD_CTRL_ILI9341: run_table(init_ili9341); s_bpp = 2; break;
        case LCD_CTRL_ST7789:
            s_bgr = 0x00;                 // these boards are RGB order
            run_table(init_st7789);
            s_bpp = 2;
            break;
        case LCD_CTRL_ST7796:  run_table(init_st7796);  s_bpp = 2; break;
        default:
            // The module in hand is a 2.4" 240x320 with an ST part on it, and
            // ST7789 is what that almost always means. If the panel stays dark
            // or the colours are wrong, name the controller outright with
            // -D LCD_FORCE_CTRL rather than guessing again here.
            logln("[lcd] controller unknown - assuming ST7789");
            s_bgr = 0x00;
            run_table(init_st7789);
            s_bpp = 2;
            break;
    }

    lcd_set_rotation(s_rot);   // uses s_bgr, so it runs after the table above

    // The longest side, whichever way the panel is turned; 3 bytes/px worst case.
    size_t need = (size_t)(LCD_WIDTH > LCD_HEIGHT ? LCD_WIDTH : LCD_HEIGHT) * 3;
    if (s_line_sz < need) {
        if (s_line) free(s_line);
        s_line = (uint8_t *)lcd_dma_alloc(need);
        s_line_sz = s_line ? need : 0;
    }
    if (!s_line) {
        logln("[lcd] FATAL: no DMA-capable memory for the line buffer");
        return false;
    }

    // Stage 3.4's staging buffers. Internal RAM, because that is the only kind
    // the DMA engine reads at full speed, and two of them so a copy can overlap
    // a transfer. 16 KB total out of 320.
    for (int i = 0; i < LCD_STAGE_BUFS; ++i) {
        if (!s_stage[i]) s_stage[i] = (uint8_t *)lcd_dma_alloc(LCD_MAX_TRANSFER);
        if (!s_stage[i]) {
            logln("[lcd] FATAL: no DMA-capable memory for the staging buffers");
            return false;
        }
    }
    logf("[lcd] staging: %d x %u bytes, %u rows per transaction at full width\n",
         LCD_STAGE_BUFS, (unsigned)LCD_MAX_TRANSFER,
         (unsigned)(LCD_MAX_TRANSFER / ((size_t)s_w * s_bpp)));
    return true;
}

void lcd_set_rotation(uint8_t rot)
{
    s_rot = rot & 3;
    uint8_t m = madctl_for(s_rot);
    lcd_cmd_p(0x36, &m, 1);

    // 240x320 native, so landscape is 320x240. These are the PANEL's own
    // numbers, not LCD_WIDTH/LCD_HEIGHT: those are the rotated size the rest
    // of the system sees, and in the portrait branch below they are swapped.
    if (s_rot & 1) { s_w = 320; s_h = 240; }
    else           { s_w = 240; s_h = 320; }
}

uint16_t lcd_width(void)          { return s_w; }
uint16_t lcd_height(void)         { return s_h; }
uint8_t  lcd_bytes_per_pixel(void){ return s_bpp; }

static bool     s_bl_pwm;
static uint32_t s_bl_duty;

void lcd_backlight_pct(uint8_t pct)
{
    if (pct > 100) pct = 100;
    if (!s_bl_pwm) {
        // Taken over from the plain GPIO that lcd_bus_init set up: once a LEDC
        // channel is attached, the pin follows the channel.
        ledc_timer_config_t t = {};
        t.speed_mode      = LEDC_LOW_SPEED_MODE;
        t.duty_resolution = LEDC_TIMER_10_BIT;
        t.timer_num       = LEDC_TIMER_0;
        t.freq_hz         = 5000;
        t.clk_cfg         = LEDC_AUTO_CLK;
        ledc_timer_config(&t);

        ledc_channel_config_t ch = {};
        ch.gpio_num   = PIN_LCD_BL;
        ch.speed_mode = LEDC_LOW_SPEED_MODE;
        ch.channel    = LEDC_CHANNEL_0;
        ch.timer_sel  = LEDC_TIMER_0;
        ch.duty       = 0;
        ledc_channel_config(&ch);
        s_bl_pwm = true;
    }
    s_bl_duty = (uint32_t)pct * LCD_BACKLIGHT_MAX_DUTY / 100u;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, s_bl_duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

uint32_t lcd_backlight_duty(void)
{
    return s_bl_pwm ? ledc_get_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0) : 0;
}

void lcd_backlight(bool on)
{
    lcd_backlight_pct(on ? 100 : 0);
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
    if (!s_stage[0] || w == 0 || h == 0) return;
    if (x >= s_w || y >= s_h) return;
    if (x + w > s_w) w = s_w - x;
    if (y + h > s_h) h = s_h - y;

    lcd_set_window(x, y, w, h);

    // One row encoded, then replicated to fill a whole transaction. Unlike a
    // blit this needs no second buffer: every chunk is the same bytes, so
    // several transfers may point at the one buffer at once - nothing is going
    // to rewrite it underneath them.
    size_t row_bytes = encode_run(s_stage[0], color, w);

    size_t rows_per_chunk = LCD_MAX_TRANSFER / row_bytes;
    if (rows_per_chunk == 0) rows_per_chunk = 1;
    if (rows_per_chunk > h) rows_per_chunk = h;

    for (size_t i = 1; i < rows_per_chunk; ++i) {
        memcpy(s_stage[0] + i * row_bytes, s_stage[0], row_bytes);
    }

    uint16_t row = 0;
    while (row < h) {
        size_t n = rows_per_chunk;
        if (row + n > h) n = h - row;

        if (lcd_bus_mode() == LCD_XFER_QUEUED) lcd_data_async(s_stage[0], n * row_bytes);
        else                                   lcd_data(s_stage[0], n * row_bytes);

        row += (uint16_t)n;
    }
    lcd_bus_wait();
}

void lcd_fill_screen(uint16_t color)
{
    lcd_fill_rect(0, 0, s_w, s_h, color);
}

// ---------------------------------------------------------------------------
// Stage 3.4 - one transaction per rectangle, and two buffers to keep the wire
// busy while the next one is being built.
//
// Up to stage 3.3 a blit was one SPI transaction per row. The panel does not
// require that: CASET/RASET set an address window once, and everything that
// follows RAMWR pours into it as one continuous stream, wrapping to the next
// row on its own. Rows were a property of the driver, not of the panel, and
// they cost about 41 us each in fixed overhead - 9.8 ms of a 25 ms frame.
//
// So the stream is cut into chunks of whole rows instead, as many as fit in a
// staging buffer. A full frame goes from 240 transactions to 20.
//
// WHY TWO BUFFERS
//
// Staging is a memcpy out of PSRAM, and PSRAM reads at about 38 MB/s while the
// SPI wire takes bytes at 10. With a single buffer those two alternate: copy,
// then wait, then copy, then wait, adding roughly 4 ms to a full frame doing
// nothing. With two, the copy for chunk N+1 runs while chunk N is still going
// out, and the memcpy disappears under the transfer entirely.
//
// The ordering rule that makes it correct: reap BEFORE filling, never before
// sending. A buffer is only safe to write once the DMA engine has finished
// reading it, and lcd_bus_wait_one() is what establishes that.
// ---------------------------------------------------------------------------

// Sends one staged chunk, respecting whichever transfer mode is set.
//
// In polling mode this is synchronous and *inflight stays zero - the chunking
// win still applies, there is just no overlap. Keeping both paths here is what
// lets the two effects be measured apart.
static inline void stage_send(const uint8_t *buf, size_t len, int *inflight)
{
    if (lcd_bus_mode() == LCD_XFER_QUEUED) {
        lcd_data_async(buf, len);
        (*inflight)++;
    } else {
        lcd_data(buf, len);
    }
}

// Blocks until the buffer we are about to overwrite is no longer being read.
static inline void stage_reserve(int *inflight)
{
    if (*inflight >= LCD_STAGE_BUFS) {
        lcd_bus_wait_one();
        (*inflight)--;
    }
}

void lcd_blit_wire(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                   const uint8_t *src, size_t stride_bytes)
{
    if (!s_stage[0] || !src || w == 0 || h == 0) return;
    if (x + w > s_w || y + h > s_h) return;

    const size_t row_bytes = (size_t)w * s_bpp;

    // How many whole rows fit in one transaction. Always at least one: the
    // widest row this panel can produce is 960 bytes, well inside the cap, but
    // the clamp keeps the arithmetic honest if either ever changes.
    size_t rows_per_chunk = LCD_MAX_TRANSFER / row_bytes;
    if (rows_per_chunk == 0) rows_per_chunk = 1;

    lcd_set_window(x, y, w, h);

    int      inflight = 0;
    int      buf      = 0;
    uint16_t row      = 0;

    while (row < h) {
        size_t n = rows_per_chunk;
        if (row + n > h) n = h - row;

        stage_reserve(&inflight);
        uint8_t *dst = s_stage[buf];

        // A copy, not a conversion - the source is already in wire order. This
        // exists only because the DMA engine will not read PSRAM as quickly as
        // it reads internal RAM, and because a rectangle narrower than the
        // framebuffer is not contiguous at the source anyway.
        if (stride_bytes == row_bytes) {
            memcpy(dst, src + (size_t)row * stride_bytes, n * row_bytes);
        } else {
            for (size_t i = 0; i < n; ++i) {
                memcpy(dst + i * row_bytes,
                       src + (size_t)(row + i) * stride_bytes, row_bytes);
            }
        }

        stage_send(dst, n * row_bytes, &inflight);

        buf  = (buf + 1) % LCD_STAGE_BUFS;
        row += (uint16_t)n;
    }

    lcd_bus_wait();
}

void lcd_blit_wire_direct(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                          const uint8_t *src, size_t stride_bytes)
{
    if (!src || w == 0 || h == 0) return;
    if (x + w > s_w || y + h > s_h) return;

    const size_t row_bytes = (size_t)w * s_bpp;
    lcd_set_window(x, y, w, h);

    // Contiguous at the source, so the DMA engine can walk it without help and
    // the chunk boundaries need not fall on rows at all. This is the version
    // stage 3.2 found slower than staging - worth asking again now that a chunk
    // is thousands of bytes rather than one row.
    if (stride_bytes == row_bytes) {
        size_t left = row_bytes * h;
        while (left) {
            size_t n = left > LCD_MAX_TRANSFER ? LCD_MAX_TRANSFER : left;
            lcd_data(src, n);
            src  += n;
            left -= n;
        }
        return;
    }

    for (uint16_t row = 0; row < h; ++row) {
        lcd_data(src + (size_t)row * stride_bytes, row_bytes);
    }
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
