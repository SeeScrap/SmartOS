#pragma once

#include "smartos_config.h"
// ---------------------------------------------------------------------------
// Project S3 - board pin map
// ESP32-S3-DevKitC-1 N16R8 + 2.4" 240x320 SPI TFT (ST7789 or ILI9341) + buttons
//
// !! DO NOT USE GPIO 26..37 !!
//    26-32 = octal SPI flash, 33-37 = octal PSRAM on the R8 module.
//    Touching them makes the board fail to boot.
// Also reserved: 19/20 = native USB, 43/44 = UART0, 0/45/46 = strapping.
// Free for us: 1-18, 21, 38-42, 47, 48
// ---------------------------------------------------------------------------

// ---- LCD bus (chosen to land on the FSPI IOMUX pins -> allows 80 MHz) ------
#define PIN_LCD_SCK    12   // FSPICLK
#define PIN_LCD_MOSI   11   // FSPID   (module pin: SDI / MOSI)
#define PIN_LCD_MISO   13   // FSPIQ   (module pin: SDO / MISO) - needed to read the panel ID
#define PIN_LCD_CS     10   // FSPICS0
#define PIN_LCD_DC     14   // module pin: DC / RS
#define PIN_LCD_RST    21   // module pin: RESET
#define PIN_LCD_BL     47   // module pin: LED (backlight)

// ---- XPT2046 touch (shares SCK/MOSI/MISO, own CS; max ~2 MHz) -------------
#define PIN_TP_CS       9
#define PIN_TP_IRQ      8

// ---- microSD on the same module (own CS, shares the bus) ------------------
#define PIN_SD_CS       3

// ---- buttons: wire each one between the GPIO and GND, we use INPUT_PULLUP --
#define PIN_BTN_UP      4
#define PIN_BTN_DOWN    5
#define PIN_BTN_LEFT    6
#define PIN_BTN_RIGHT   7
#define PIN_BTN_OK     15
#define PIN_BTN_BACK   16

// ---- panel geometry (landscape) -------------------------------------------
// The panel is a 2.4" 240x320. That is its portrait, native orientation; we
// drive it landscape, so the numbers here are swapped relative to the glass.
//
// Both controllers that come on these modules take 16-bit RGB565 straight over
// SPI, which the 3.5" ILI9488 could not - it forced 18-bit, three bytes per
// pixel. A frame is now 153,600 bytes on the wire instead of 460,800.
// LCD_WIDTH, LCD_HEIGHT and LCD_SPI_HZ are in smartos_config.h, section 14.

// SPI clock used for pixel pushing.
// 80 MHz is the ceiling of SPI2 and is only reachable because SCK/MOSI/MISO/CS
// sit on the FSPI IOMUX pads - routing them through the GPIO matrix would cap
// us at 40 MHz.
// These red modules have long unbuffered traces, so if the panel shows noise or
// nothing once it is wired up, step back down to 40 MHz.

// ---- pins no app may touch (stage 4.3c) ------------------------------------
// Built from the defines above rather than written out, so moving a wire
// cannot leave this list pointing at the old pin. The device service
// (os/dev.h) refuses every pin in here; an app that could reconfigure the LCD
// clock or a flash line could take the whole system down with one call.
#define PIN_BIT(p) (1ULL << (p))

#define PINS_SYSTEM  (PIN_BIT(PIN_LCD_SCK) | PIN_BIT(PIN_LCD_MOSI) | PIN_BIT(PIN_LCD_MISO) | \
                      PIN_BIT(PIN_LCD_CS)  | PIN_BIT(PIN_LCD_DC)   | PIN_BIT(PIN_LCD_RST)  | \
                      PIN_BIT(PIN_LCD_BL)  | PIN_BIT(PIN_TP_CS)    | PIN_BIT(PIN_TP_IRQ)   | \
                      PIN_BIT(PIN_SD_CS)   | PIN_BIT(PIN_BTN_UP)   | PIN_BIT(PIN_BTN_DOWN) | \
                      PIN_BIT(PIN_BTN_LEFT)| PIN_BIT(PIN_BTN_RIGHT)| PIN_BIT(PIN_BTN_OK)   | \
                      PIN_BIT(PIN_BTN_BACK))

#define PINS_CHIP    (0x3FFC000000ULL      /* 26-37: octal flash and PSRAM   */ | \
                      PIN_BIT(19) | PIN_BIT(20)          /* native USB    */ | \
                      PIN_BIT(43) | PIN_BIT(44)          /* UART0         */ | \
                      PIN_BIT(0)  | PIN_BIT(45) | PIN_BIT(46) /* strapping */)

#define PINS_RESERVED (PINS_SYSTEM | PINS_CHIP)

// What that leaves for apps: 1, 2, 17, 18, 38-42, 48. Of those, 1 and 2 are
// ADC1 channels 0 and 1, and 17 and 18 are ADC2 channels 6 and 7.
#define PIN_MAX 48
