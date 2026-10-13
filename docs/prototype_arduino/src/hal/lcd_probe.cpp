#include "hal/lcd_probe.h"

#include <Arduino.h>
#include "board/pins.h"
#include "hal/log.h"

// ~1 us per half clock -> about 500 kHz. Panels spec their read cycle at
// roughly 6 MHz max, so this is comfortably slow.
static inline void bb_delay(void) { esp_rom_delay_us(1); }

static void bb_pins_as_gpio(void)
{
    pinMode(PIN_LCD_SCK,  OUTPUT);
    pinMode(PIN_LCD_MOSI, OUTPUT);
    pinMode(PIN_LCD_MISO, INPUT);
    pinMode(PIN_LCD_CS,   OUTPUT);
    pinMode(PIN_LCD_DC,   OUTPUT);
    pinMode(PIN_LCD_RST,  OUTPUT);
    pinMode(PIN_LCD_BL,   OUTPUT);

    // Park the other devices on the shared bus so they stay quiet.
    pinMode(PIN_TP_CS, OUTPUT); digitalWrite(PIN_TP_CS, HIGH);
    pinMode(PIN_SD_CS, OUTPUT); digitalWrite(PIN_SD_CS, HIGH);

    digitalWrite(PIN_LCD_CS,  HIGH);
    digitalWrite(PIN_LCD_SCK, LOW);   // SPI mode 0: clock idles low
}

static void bb_reset_panel(void)
{
    digitalWrite(PIN_LCD_RST, HIGH); delay(10);
    digitalWrite(PIN_LCD_RST, LOW);  delay(20);
    digitalWrite(PIN_LCD_RST, HIGH); delay(150);
}

static void bb_write_byte(uint8_t v)
{
    for (int i = 7; i >= 0; --i) {
        digitalWrite(PIN_LCD_MOSI, (v >> i) & 1);
        bb_delay();
        digitalWrite(PIN_LCD_SCK, HIGH);   // slave latches on the rising edge
        bb_delay();
        digitalWrite(PIN_LCD_SCK, LOW);
    }
}

static uint8_t bb_read_byte(void)
{
    uint8_t v = 0;
    for (int i = 7; i >= 0; --i) {
        digitalWrite(PIN_LCD_SCK, HIGH);   // we sample on the rising edge
        bb_delay();
        if (digitalRead(PIN_LCD_MISO)) v |= (1u << i);
        digitalWrite(PIN_LCD_SCK, LOW);
        bb_delay();
    }
    return v;
}

static void bb_clock_pulse(void)
{
    digitalWrite(PIN_LCD_SCK, HIGH); bb_delay();
    digitalWrite(PIN_LCD_SCK, LOW);  bb_delay();
}

// One command byte (DC low), one dummy clock, then n reply bytes (DC high),
// all with CS held low for the whole exchange.
static void bb_read_reg(uint8_t cmd, uint8_t *out, size_t n)
{
    digitalWrite(PIN_LCD_CS, LOW);

    digitalWrite(PIN_LCD_DC, LOW);
    bb_write_byte(cmd);

    digitalWrite(PIN_LCD_DC, HIGH);
    pinMode(PIN_LCD_MISO, INPUT);
    bb_clock_pulse();                       // the panel's dummy cycle
    for (size_t i = 0; i < n; ++i) out[i] = bb_read_byte();

    digitalWrite(PIN_LCD_CS, HIGH);
}

static void dump(const char *label, uint8_t cmd, const uint8_t *b, size_t n)
{
    logf("  %-8s (0x%02X) ->", label, cmd);
    for (size_t i = 0; i < n; ++i) logf(" %02X", b[i]);
    logln();
}

const char *lcd_ctrl_name(lcd_ctrl_t c)
{
    switch (c) {
        case LCD_CTRL_ILI9488: return "ILI9488";
        case LCD_CTRL_ILI9486: return "ILI9486";
        case LCD_CTRL_ILI9341: return "ILI9341";
        case LCD_CTRL_ST7796:  return "ST7796";
        default:               return "UNKNOWN";
    }
}

lcd_ctrl_t lcd_probe_run(void)
{
    logln("[lcd] probing panel ID (bit-banged)...");

    bb_pins_as_gpio();
    digitalWrite(PIN_LCD_BL, LOW);          // keep the backlight off while we poke
    bb_reset_panel();

    uint8_t d3[4] = {0}, id[3] = {0}, da = 0, db = 0, dc = 0;
    bb_read_reg(0xD3, d3, 4);               // RDID4 - the one that names the chip
    bb_read_reg(0x04, id, 3);               // RDDID
    bb_read_reg(0xDA, &da, 1);
    bb_read_reg(0xDB, &db, 1);
    bb_read_reg(0xDC, &dc, 1);

    dump("RDID4", 0xD3, d3, 4);
    dump("RDDID", 0x04, id, 3);
    dump("RDID1", 0xDA, &da, 1);
    dump("RDID2", 0xDB, &db, 1);
    dump("RDID3", 0xDC, &dc, 1);

    // Every ILI9xxx / ST77xx answers RDID4 as {00, 00, hi, lo}. All-zero,
    // all-ones or random noise all mean the same thing: nobody drove MISO.
    bool answered = (d3[0] == 0x00 && d3[1] == 0x00 &&
                     d3[2] != 0x00 && d3[2] != 0xFF);

    if (!answered) {
        logln("[lcd] no valid answer on MISO - the bytes above are floating noise.");
        logln("      Usual cause: these red 3.5\" modules ship with the SDO series");
        logln("      resistor unpopulated, so the panel never drives MISO back.");
        logln("      Not fatal - writing to the panel still works. Judge the panel");
        logln("      by what the colour test puts on the glass instead.");
        return LCD_CTRL_UNKNOWN;
    }

    // RDID4 answers as {00, 00, hi, lo} where hi/lo spell the part number.
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
