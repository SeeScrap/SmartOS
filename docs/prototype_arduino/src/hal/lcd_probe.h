#pragma once
#include <stdint.h>

// Panel controllers we know how to drive.
typedef enum {
    LCD_CTRL_UNKNOWN = 0,
    LCD_CTRL_ILI9488,
    LCD_CTRL_ILI9486,
    LCD_CTRL_ILI9341,
    LCD_CTRL_ST7796,
} lcd_ctrl_t;

const char *lcd_ctrl_name(lcd_ctrl_t c);

// Bit-bangs the LCD's ID registers over the same pins the SPI peripheral will
// later use, and prints everything it saw to Serial.
//
// Reads are done by hand rather than with the SPI peripheral on purpose: the
// panel needs CS held low across "command then read", with a dummy clock in
// between, which the IDF SPI driver cannot express cleanly. Reads happen once
// at boot, so bit-banging costs us nothing.
//
// Must be called BEFORE lcd_bus_init(). Returns the best guess; returns
// LCD_CTRL_UNKNOWN if the panel did not answer (usually a missing MISO wire).
lcd_ctrl_t lcd_probe_run(void);
