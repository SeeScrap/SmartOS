#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

// Write-only SPI transport for the panel.
//
// The display driver never needs to read, so this side of the HAL only pushes:
// command bytes with DC low, data bytes with DC high, straight into DMA.
// Panel ID reads live in lcd_probe.cpp and are bit-banged instead.

// How a bulk transfer waits for the hardware.
//
// POLLING busy-waits inside the driver: the CPU sits in a loop for the whole
// transfer and does nothing else. That is the right trade for a few bytes,
// where an interrupt would cost more than the transfer, and completely wrong
// for a frame - which at 320x240 is 20 ms of a core spent watching a FIFO.
//
// QUEUED hands the transaction to the driver and blocks on its semaphore, so
// the core is free until the interrupt says it is done. It also allows several
// transfers to be in flight, which is what lets the compositor build the next
// rectangle while the last one is still going out.
typedef enum {
    LCD_XFER_POLLING = 0,
    LCD_XFER_QUEUED,
} lcd_xfer_mode_t;

// Largest single SPI transaction, and therefore the size of each staging
// buffer in lcd_panel.cpp.
//
// Was 4096 through stage 3.3, which did not matter then because the blit sent
// one transaction per ROW - 640 bytes on this panel, nowhere near the cap. What
// made it matter is stage 3.4 sending a whole rectangle at once: the cap is now
// what decides how many transactions a frame costs. 8 KB puts a full frame at
// 20 of them instead of 240, which is 0.8 ms of overhead against 15.4 ms of
// wire time - far enough down that doubling the buffers again would buy almost
// nothing and cost real internal RAM.
#define LCD_MAX_TRANSFER  8192

esp_err_t lcd_bus_init(uint32_t clock_hz);
void      lcd_bus_free(void);

void            lcd_bus_set_mode(lcd_xfer_mode_t mode);
lcd_xfer_mode_t lcd_bus_mode(void);

// Queues a transfer and returns. The buffer must stay valid and untouched until
// lcd_bus_wait() has returned - the DMA engine reads it, not the CPU.
void lcd_data_async(const uint8_t *data, size_t len);

// Drains everything queued. Cheap when nothing is outstanding.
void lcd_bus_wait(void);

// Reaps exactly one completed transfer, blocking if none has finished yet.
// This is what makes double buffering possible: the caller needs to know that
// one specific staging buffer is free again, not that the bus has gone idle.
void lcd_bus_wait_one(void);

// How many transfers the hardware still owns.
int  lcd_bus_pending(void);

void lcd_cmd(uint8_t cmd);
void lcd_data(const uint8_t *data, size_t len);
void lcd_cmd_p(uint8_t cmd, const uint8_t *params, size_t len);

// DMA can only read internal RAM, so pixel buffers must come from here.
void *lcd_dma_alloc(size_t bytes);
