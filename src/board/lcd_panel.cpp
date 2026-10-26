#include "board/lcd_panel.h"


#include "board/gpio.h"
#include "port/kport.h"
#include <stdlib.h>
#include <string.h>

#include "board/devkitc_n16r8.h"
#include "board/lcd_bus.h"
#include "board/log.h"

#include "esp_rom_gpio.h"
#include "soc/gpio_sig_map.h"
#include "soc/ledc_struct.h"
#include "soc/soc.h"
#include "soc/system_reg.h"

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
        if (count & TBL_DELAY) kport_yield_ms(150);
    }
}

static void hw_reset(void)
{
    gpio_hi(PIN_LCD_RST); kport_yield_ms(10);
    gpio_lo(PIN_LCD_RST);  kport_yield_ms(20);
    gpio_hi(PIN_LCD_RST); kport_yield_ms(150);
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

// --- the backlight ---------------------------------------------------------
//
// One PWM channel, written to the LEDC registers directly. It used to go
// through ESP-IDF's LEDC driver, which cost 11,304 bytes of the image - most
// of it DRAM, the scarcest memory on this board - to drive a single pin at a
// fixed frequency that never changes. The driver carries eight channels, four
// timers, fading with an interrupt, clock-source negotiation and power
// management; none of that is wanted here.
//
// The register sequence below is not invented. It is the one ESP-IDF's own
// low-level layer performs, read out of
// components/esp_hal_ledc/esp32s3/include/hal/ledc_ll.h and followed in the
// same order, because this is a place where guessing shows up as a dark
// screen rather than as a compiler error.
//
// Low-speed mode, timer 0, channel 0. On the ESP32-S3 the clock source is
// chosen once, globally, in LEDC.conf.apb_clk_sel - the per-timer tick_sel
// field that older chips use is not touched by IDF here, so it is not touched
// here either.

// (divider / 256) * 2^bits must divide the 80 MHz APB clock to the frequency
// we asked for, and the divider is a 10.8 fixed-point number.
// 64-bit on purpose: 80,000,000 * 256 is 20.5 billion and does not fit in
// a uint32_t. The first version of this line wrapped, and the
// static_assert below is the only reason that was noticed - it fired for
// an overflow rather than for the frequency it was written to guard.
#define BL_APB_X256 (80ull * 1000ull * 1000ull * 256ull)
#define BL_DIV_NUM  ((uint32_t)(BL_APB_X256 / \
                     ((uint64_t)LCD_BACKLIGHT_HZ << LCD_BACKLIGHT_BITS)))
static_assert(BL_APB_X256 %
              ((uint64_t)LCD_BACKLIGHT_HZ << LCD_BACKLIGHT_BITS) == 0,
              "LCD_BACKLIGHT_HZ and _BITS do not divide the 80 MHz APB clock "
              "exactly - the backlight would run at a frequency nobody chose");
static_assert(LCD_BACKLIGHT_MAX_DUTY == (1u << LCD_BACKLIGHT_BITS) - 1u,
              "LCD_BACKLIGHT_MAX_DUTY must be the largest duty the chosen "
              "resolution can express");
static_assert(BL_DIV_NUM < (1u << 18), "the LEDC clock divider is 18 bits");

static bool     s_bl_pwm;
static uint32_t s_bl_duty;

static void bl_start(void)
{
    // The peripheral: clock on, then out of reset.
    REG_SET_BIT(SYSTEM_PERIP_CLK_EN0_REG, SYSTEM_LEDC_CLK_EN);
    REG_SET_BIT(SYSTEM_PERIP_RST_EN0_REG, SYSTEM_LEDC_RST);
    REG_CLR_BIT(SYSTEM_PERIP_RST_EN0_REG, SYSTEM_LEDC_RST);

    LEDC.conf.apb_clk_sel = 1;          // 1 = APB, 80 MHz

    // Timer 0. low_speed_update is how a low-speed timer's new settings are
    // taken up; without it the registers hold the right numbers and the
    // hardware keeps using the old ones.
    LEDC.timer_group[0].timer[0].conf.duty_resolution = LCD_BACKLIGHT_BITS;
    LEDC.timer_group[0].timer[0].conf.clock_divider   = BL_DIV_NUM;
    LEDC.timer_group[0].timer[0].conf.pause           = 0;
    LEDC.timer_group[0].timer[0].conf.rst             = 1;
    LEDC.timer_group[0].timer[0].conf.rst             = 0;
    LEDC.timer_group[0].timer[0].conf.low_speed_update = 1;

    // Channel 0 follows timer 0, idle low, and starts its period at 0.
    LEDC.channel_group[0].channel[0].conf0.timer_sel = 0;
    LEDC.channel_group[0].channel[0].conf0.idle_lv   = 0;
    LEDC.channel_group[0].channel[0].hpoint.hpoint   = 0;

    // The pin is already an output: lcd_bus_init drove it as a plain GPIO to
    // get a picture up before any of this existed. Routing the channel's
    // signal through the GPIO matrix takes it over.
    esp_rom_gpio_connect_out_signal(PIN_LCD_BL, LEDC_LS_SIG_OUT0_IDX, false, false);
    s_bl_pwm = true;
}

void lcd_backlight_pct(uint8_t pct)
{
    if (pct > 100) pct = 100;
    if (!s_bl_pwm) bl_start();

    s_bl_duty = (uint32_t)pct * LCD_BACKLIGHT_MAX_DUTY / 100u;

    // duty is a 19-bit field holding the value shifted left by four: the low
    // four bits are the fractional part the fade hardware uses, and we do not
    // fade. conf1 is cleared so no fade is in progress, duty_start latches the
    // new value, and low_speed_update hands the whole channel to the hardware.
    LEDC.channel_group[0].channel[0].duty.duty = s_bl_duty << 4;
    LEDC.channel_group[0].channel[0].conf1.val = 0;
    LEDC.channel_group[0].channel[0].conf1.duty_start = 1;
    LEDC.channel_group[0].channel[0].conf0.sig_out_en = 1;
    LEDC.channel_group[0].channel[0].conf0.low_speed_update = 1;
}

// Read back from the hardware, not from s_bl_duty - a test that compares a
// variable with itself proves nothing. duty_read is what the channel is
// actually driving, in the same 4-bit-shifted form.
uint32_t lcd_backlight_duty(void)
{
    if (!s_bl_pwm) return 0;
    return LEDC.channel_group[0].channel[0].duty_rd.duty_read >> 4;
}

// For the self-test: is the channel really configured and driving the pin,
// rather than merely holding the numbers we wrote? Frequency in Hz, computed
// back out of the timer's own registers.
uint32_t lcd_backlight_hz(void)
{
    if (!s_bl_pwm || !LEDC.channel_group[0].channel[0].conf0.sig_out_en) return 0;
    const uint32_t div  = LEDC.timer_group[0].timer[0].conf.clock_divider;
    const uint32_t bits = LEDC.timer_group[0].timer[0].conf.duty_resolution;
    if (!div) return 0;
    return (uint32_t)(BL_APB_X256 / ((uint64_t)div << bits));
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
