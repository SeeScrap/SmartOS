#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

// Write-only SPI transport for the panel.
//
// The display driver never needs to read, so this side of the HAL only pushes:
// command bytes with DC low, data bytes with DC high, straight into DMA.
// Panel ID reads live in lcd_probe.cpp and are bit-banged instead.

esp_err_t lcd_bus_init(uint32_t clock_hz);
void      lcd_bus_free(void);

void lcd_cmd(uint8_t cmd);
void lcd_data(const uint8_t *data, size_t len);
void lcd_cmd_p(uint8_t cmd, const uint8_t *params, size_t len);

// DMA can only read internal RAM, so pixel buffers must come from here.
void *lcd_dma_alloc(size_t bytes);
