#include "hal/lcd_bus.h"

#include <string.h>

#include "board/pins.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"

#define LCD_SPI_HOST      SPI2_HOST      // FSPI - the host wired to the IOMUX pins we chose
#define LCD_MAX_TRANSFER  4096

static spi_device_handle_t s_dev = nullptr;
static bool s_bus_up = false;

// The DC line has to be right before the first clock edge of every transfer,
// so it is flipped from the driver's pre-transfer hook rather than by us.
// t->user carries the level we want.
static void IRAM_ATTR lcd_pre_cb(spi_transaction_t *t)
{
    gpio_set_level((gpio_num_t)PIN_LCD_DC, (int)(intptr_t)t->user);
}

esp_err_t lcd_bus_init(uint32_t clock_hz)
{
    if (s_bus_up) return ESP_OK;

    gpio_config_t io = {};
    io.pin_bit_mask = (1ULL << PIN_LCD_DC) | (1ULL << PIN_LCD_RST) | (1ULL << PIN_LCD_BL);
    io.mode         = GPIO_MODE_OUTPUT;
    esp_err_t err = gpio_config(&io);
    if (err != ESP_OK) return err;

    spi_bus_config_t bus = {};
    bus.mosi_io_num     = PIN_LCD_MOSI;
    bus.miso_io_num     = PIN_LCD_MISO;
    bus.sclk_io_num     = PIN_LCD_SCK;
    bus.quadwp_io_num   = -1;
    bus.quadhd_io_num   = -1;
    bus.max_transfer_sz = LCD_MAX_TRANSFER;

    err = spi_bus_initialize(LCD_SPI_HOST, &bus, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) return err;

    spi_device_interface_config_t dev = {};
    dev.clock_speed_hz = (int)clock_hz;
    dev.mode           = 0;
    dev.spics_io_num   = PIN_LCD_CS;
    dev.queue_size     = 4;
    dev.pre_cb         = lcd_pre_cb;
    dev.flags          = SPI_DEVICE_NO_DUMMY;

    err = spi_bus_add_device(LCD_SPI_HOST, &dev, &s_dev);
    if (err != ESP_OK) {
        spi_bus_free(LCD_SPI_HOST);
        return err;
    }

    s_bus_up = true;
    return ESP_OK;
}

void lcd_bus_free(void)
{
    if (!s_bus_up) return;
    spi_bus_remove_device(s_dev);
    spi_bus_free(LCD_SPI_HOST);
    s_dev   = nullptr;
    s_bus_up = false;
}

void lcd_cmd(uint8_t cmd)
{
    spi_transaction_t t = {};
    t.flags      = SPI_TRANS_USE_TXDATA;
    t.length     = 8;
    t.tx_data[0] = cmd;
    t.user       = (void *)0;          // DC low = command
    spi_device_polling_transmit(s_dev, &t);
}

void lcd_data(const uint8_t *data, size_t len)
{
    if (len == 0) return;
    spi_transaction_t t = {};
    t.length    = len * 8;
    t.tx_buffer = data;
    t.user      = (void *)1;           // DC high = data
    spi_device_polling_transmit(s_dev, &t);
}

void lcd_cmd_p(uint8_t cmd, const uint8_t *params, size_t len)
{
    lcd_cmd(cmd);
    if (len) lcd_data(params, len);
}

void *lcd_dma_alloc(size_t bytes)
{
    return heap_caps_malloc(bytes, MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
}
