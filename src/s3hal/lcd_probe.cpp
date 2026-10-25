#include "s3hal/lcd_probe.h"

#include "s3hal/gpio.h"
#include "s3hal/hal_time.h"
#include "board/devkitc_n16r8.h"
#include "s3hal/log.h"

// How many clocks the panel wants between the command and its answer.
//
// One is the usual answer and what the ILI9xxx datasheets specify, but the
// number is a property of the controller, not of SPI, and getting it wrong
// shifts every byte - which is exactly what an ST7789V wired up correctly
// looked like here: structured, repeatable, and one bit out.
#ifndef LCD_READ_DUMMY_BITS
#define LCD_READ_DUMMY_BITS 1
#endif


// ~1 us per half clock -> about 500 kHz. Panels spec their read cycle at
// roughly 6 MHz max, so this is comfortably slow.
static inline void bb_delay(void) { esp_rom_delay_us(1); }

static void bb_pins_as_gpio(void)
{
    gpio_out(PIN_LCD_SCK);
    gpio_out(PIN_LCD_MOSI);
    gpio_in(PIN_LCD_MISO);
    gpio_out(PIN_LCD_CS);
    gpio_out(PIN_LCD_DC);
    gpio_out(PIN_LCD_RST);
    gpio_out(PIN_LCD_BL);

    // Park the other devices on the shared bus so they stay quiet.
    gpio_out(PIN_TP_CS); gpio_hi(PIN_TP_CS);
    gpio_out(PIN_SD_CS); gpio_hi(PIN_SD_CS);

    gpio_hi(PIN_LCD_CS);
    gpio_lo(PIN_LCD_SCK);   // SPI mode 0: clock idles low
}

static void bb_reset_panel(void)
{
    gpio_hi(PIN_LCD_RST); hal_delay_ms(10);
    gpio_lo(PIN_LCD_RST);  hal_delay_ms(20);
    gpio_hi(PIN_LCD_RST); hal_delay_ms(150);
}

static void bb_write_byte(uint8_t v)
{
    for (int i = 7; i >= 0; --i) {
        gpio_set(PIN_LCD_MOSI, (v >> i) & 1);
        bb_delay();
        gpio_hi(PIN_LCD_SCK);   // slave latches on the rising edge
        bb_delay();
        gpio_lo(PIN_LCD_SCK);
    }
}

// SPI mode 0 says the slave presents a bit on the falling edge and the master
// takes it on the rising one. That is what `rising` does. Some panels - and
// more often the clone boards - only settle in time to be read on the falling
// edge instead, so both are available rather than assumed.
static uint8_t bb_read_byte_edge(bool rising)
{
    uint8_t v = 0;
    for (int i = 7; i >= 0; --i) {
        gpio_hi(PIN_LCD_SCK);
        bb_delay();
        if (rising && gpio_rd(PIN_LCD_MISO)) v |= (1u << i);
        gpio_lo(PIN_LCD_SCK);
        bb_delay();
        if (!rising && gpio_rd(PIN_LCD_MISO)) v |= (1u << i);
    }
    return v;
}

static uint8_t bb_read_byte(void)
{
    return bb_read_byte_edge(true);
}

static void bb_clock_pulse(void)
{
    gpio_hi(PIN_LCD_SCK); bb_delay();
    gpio_lo(PIN_LCD_SCK);  bb_delay();
}

// One command byte (DC low), one dummy clock, then n reply bytes (DC high),
// all with CS held low for the whole exchange.
static void bb_read_reg_edge(uint8_t cmd, uint8_t *out, size_t n, int dummy,
                             bool rising)
{
    gpio_lo(PIN_LCD_CS);

    gpio_lo(PIN_LCD_DC);
    bb_write_byte(cmd);

    gpio_hi(PIN_LCD_DC);
    gpio_in(PIN_LCD_MISO);
    for (int d = 0; d < dummy; ++d) bb_clock_pulse();
    for (size_t i = 0; i < n; ++i) out[i] = bb_read_byte_edge(rising);

    gpio_hi(PIN_LCD_CS);
}

static void bb_read_reg_dummy(uint8_t cmd, uint8_t *out, size_t n, int dummy)
{
    gpio_lo(PIN_LCD_CS);

    gpio_lo(PIN_LCD_DC);
    bb_write_byte(cmd);

    gpio_hi(PIN_LCD_DC);
    gpio_in(PIN_LCD_MISO);
    for (int d = 0; d < dummy; ++d) bb_clock_pulse();
    for (size_t i = 0; i < n; ++i) out[i] = bb_read_byte();

    gpio_hi(PIN_LCD_CS);
}

static void bb_read_reg(uint8_t cmd, uint8_t *out, size_t n)
{
    bb_read_reg_dummy(cmd, out, n, LCD_READ_DUMMY_BITS);
}

static void dump(const char *label, uint8_t cmd, const uint8_t *b, size_t n)
{
    logf("  %-8s (0x%02X) ->", label, cmd);
    for (size_t i = 0; i < n; ++i) logf(" %02X", b[i]);
    logln();
}

// Undo bb_pins_as_gpio. Without this the SPI driver and the display driver both
// find these pins already claimed and log a conflict for every one of them -
// six warnings that say nothing except that we never tidied up.
//
// RST is left floating for the moment it takes lcd_bus_init to reconfigure it;
// that is harmless, because lcd_panel_init drives a hardware reset of its own
// before it says anything to the panel.
void lcd_probe_release(void)
{
    const int pins[] = {
        PIN_LCD_SCK, PIN_LCD_MOSI, PIN_LCD_MISO, PIN_LCD_CS,
        PIN_LCD_DC,  PIN_LCD_RST,  PIN_LCD_BL,
        PIN_TP_CS,   PIN_SD_CS,
    };
    for (unsigned i = 0; i < sizeof(pins) / sizeof(pins[0]); ++i) {
        gpio_reset_pin((gpio_num_t)pins[i]);
    }
}

const char *lcd_ctrl_name(lcd_ctrl_t c)
{
    switch (c) {
        case LCD_CTRL_ILI9488: return "ILI9488";
        case LCD_CTRL_ILI9486: return "ILI9486";
        case LCD_CTRL_ILI9341: return "ILI9341";
        case LCD_CTRL_ST7796:  return "ST7796";
        case LCD_CTRL_ST7789:  return "ST7789";
        default:               return "UNKNOWN";
    }
}

#ifdef LCD_PROBE_SCAN_DUMMY
// Reads the ID register at every plausible dummy-cycle count and prints each,
// so the right one can be read off rather than guessed. ST7789 answers 85 85 52.
static void bb_scan_dummy(void)
{
    logln("[lcd] scanning dummy-cycle counts for RDDID (0x04), want 85 85 52:");
    for (int d = 0; d <= 9; ++d) {
        uint8_t v[3] = {0};
        bb_read_reg_dummy(0x04, v, 3, d);
        logf("      %d dummy -> %02X %02X %02X\n", d, v[0], v[1], v[2]);
    }
    logln("[lcd] same command, sampling on the FALLING edge instead:");
    for (int d = 0; d <= 9; ++d) {
        uint8_t v[3] = {0};
        bb_read_reg_edge(0x04, v, 3, d, false);
        logf("      %d dummy -> %02X %02X %02X\n", d, v[0], v[1], v[2]);
    }

    logln("[lcd] RDID1/2/3 (0xDA/DB/DC) falling edge, want 85 / 85 / 52:");
    for (int d = 0; d <= 3; ++d) {
        uint8_t a = 0, b = 0, c = 0;
        bb_read_reg_edge(0xDA, &a, 1, d, false);
        bb_read_reg_edge(0xDB, &b, 1, d, false);
        bb_read_reg_edge(0xDC, &c, 1, d, false);
        logf("      %d dummy -> %02X / %02X / %02X\n", d, a, b, c);
    }
}
#endif

lcd_ctrl_t lcd_probe_run(void)
{
    logln("[lcd] probing panel ID (bit-banged)...");

    bb_pins_as_gpio();
    gpio_lo(PIN_LCD_BL);          // keep the backlight off while we poke
    bb_reset_panel();

    uint8_t d3[4] = {0}, id[3] = {0}, da = 0, db = 0, dc = 0;
    bb_read_reg(0xD3, d3, 4);               // RDID4 - the one that names the chip
    bb_read_reg(0x04, id, 3);               // RDDID
    bb_read_reg(0xDA, &da, 1);
    bb_read_reg(0xDB, &db, 1);
    bb_read_reg(0xDC, &dc, 1);

#ifdef LCD_PROBE_SCAN_DUMMY
    bb_scan_dummy();
#endif

    dump("RDID4", 0xD3, d3, 4);
    dump("RDDID", 0x04, id, 3);
    dump("RDID1", 0xDA, &da, 1);
    dump("RDID2", 0xDB, &db, 1);
    dump("RDID3", 0xDC, &dc, 1);

    // ST7789 has no RDID4 at all - it answers the one-byte 0xDA/0xDB/0xDC
    // registers with 85/85/52 and nothing useful on 0xD3. Check that before
    // concluding the panel is silent, or an ST7789 looks exactly like an
    // unwired MISO.
    if (da == 0x85 && db == 0x85 && dc == 0x52) {
        lcd_probe_release();
        return LCD_CTRL_ST7789;
    }

    // Every ILI9xxx answers RDID4 as {00, 00, hi, lo}. All-zero, all-ones or
    // random noise all mean the same thing: nobody drove MISO.
    bool answered = (d3[0] == 0x00 && d3[1] == 0x00 &&
                     d3[2] != 0x00 && d3[2] != 0xFF);

    if (!answered) {
        logln("[lcd] no valid answer on MISO.");
        logln("      On this board the bytes are not noise - they are repeatable");
        logln("      and they track the command just sent, which is MOSI coupling");
        logln("      into an undriven line, not the panel replying. Scanning every");
        logln("      dummy-cycle count and both sampling edges produced the same");
        logln("      stream one bit apart, so it is not a timing problem either.");
        logln("      These modules commonly leave the SDO series resistor off, so");
        logln("      the header pin exists but never reaches the controller.");
        logln("      Harmless: writing to the panel does not use MISO. Name the");
        logln("      controller with -D LCD_FORCE_CTRL and judge it by the glass.");
        logln("      Not fatal - writing to the panel still works. Judge the panel");
        logln("      by what the colour test puts on the glass instead.");
        lcd_probe_release();
        return LCD_CTRL_UNKNOWN;
    }

    // RDID4 answers as {00, 00, hi, lo} where hi/lo spell the part number.
    lcd_probe_release();

    uint16_t sig = ((uint16_t)d3[2] << 8) | d3[3];
    switch (sig) {
        case 0x9488: return LCD_CTRL_ILI9488;
        case 0x9486: return LCD_CTRL_ILI9486;
        case 0x9341: return LCD_CTRL_ILI9341;
        case 0x7796: return LCD_CTRL_ST7796;
        default:
            logf("[lcd] unrecognised signature 0x%04X\n", sig);
            return LCD_CTRL_UNKNOWN;
    }
}
