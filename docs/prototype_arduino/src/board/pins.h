#pragma once
// ---------------------------------------------------------------------------
// Project S3 - board pin map
// ESP32-S3-DevKitC-1 N16R8 + 3.5" 480x320 SPI TFT (MSP3520 class) + buttons
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
#define LCD_WIDTH     480
#define LCD_HEIGHT    320

// SPI clock used for pixel pushing.
// 80 MHz is the ceiling of SPI2 and is only reachable because SCK/MOSI/MISO/CS
// sit on the FSPI IOMUX pads - routing them through the GPIO matrix would cap
// us at 40 MHz.
// These red modules have long unbuffered traces, so if the panel shows noise or
// nothing once it is wired up, step back down to 40 MHz.
#define LCD_SPI_HZ  (80 * 1000 * 1000)
