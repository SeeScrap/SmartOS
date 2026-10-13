// ---------------------------------------------------------------------------
// Project S3 - stage 1: hardware bring-up
//
// Everything here exists to answer four questions before a single line of
// kernel gets written:
//   1. is the N16R8 memory configuration actually right (8 MB PSRAM live)?
//   2. which controller is on the panel?
//   3. how fast can we really push a full frame? (this sets the OS render design)
//   4. are the buttons wired the way pins.h says?
// ---------------------------------------------------------------------------
#include <Arduino.h>

#include "board/pins.h"
#include "hal/input.h"
#include "hal/log.h"
#include "hal/lcd_bus.h"
#include "hal/lcd_panel.h"
#include "hal/lcd_probe.h"

static uint16_t *s_framebuffer = nullptr;

static void report_memory(void)
{
    logln();
    logln("=== Project S3 / bring-up ===");
    logf("chip      : %s rev %d, %d core(s) @ %lu MHz\n",
                  ESP.getChipModel(), ESP.getChipRevision(), ESP.getChipCores(),
                  (unsigned long)getCpuFrequencyMhz());
    logf("flash     : %lu bytes\n", (unsigned long)ESP.getFlashChipSize());
    logf("internal  : %lu bytes free\n", (unsigned long)ESP.getFreeHeap());

    size_t psram = ESP.getPsramSize();
    logf("PSRAM     : %lu bytes\n", (unsigned long)psram);
    if (psram < 4u * 1024 * 1024) {
        logln("  !! expected ~8 MB. Check board_build.arduino.memory_type = qio_opi");
        logln("     in platformio.ini - a wrong setting leaves PSRAM dead.");
    }
}

static void alloc_framebuffer(void)
{
    size_t bytes = (size_t)LCD_WIDTH * LCD_HEIGHT * 2;   // RGB565
    s_framebuffer = (uint16_t *)ps_malloc(bytes);
    logf("framebuffer: %u bytes RGB565 in PSRAM -> %s\n",
                  (unsigned)bytes, s_framebuffer ? "OK" : "FAILED");
    if (s_framebuffer) {
        logf("             at %p, PSRAM left %lu bytes\n",
                      s_framebuffer, (unsigned long)ESP.getFreePsram());
    }
}

static void benchmark(void)
{
    const uint16_t colors[] = {
        COLOR_RED, COLOR_GREEN, COLOR_BLUE, COLOR_WHITE,
        COLOR_YELLOW, COLOR_CYAN, COLOR_MAGENTA, COLOR_BLACK
    };
    const int frames = sizeof(colors) / sizeof(colors[0]);

    uint32_t t0 = micros();
    for (int i = 0; i < frames; ++i) lcd_fill_screen(colors[i]);
    uint32_t dt = micros() - t0;

    double per_frame_ms = (dt / 1000.0) / frames;
    size_t frame_bytes  = (size_t)lcd_width() * lcd_height() * lcd_bytes_per_pixel();
    double mbps         = (frame_bytes / (per_frame_ms / 1000.0)) / 1e6;

    logln();
    logln("--- full-screen fill benchmark ---");
    logf("wire format : %u bytes/pixel\n", lcd_bytes_per_pixel());
    logf("frame size  : %u bytes on the wire\n", (unsigned)frame_bytes);
    logf("per frame   : %.2f ms  ->  %.1f FPS ceiling\n",
                  per_frame_ms, 1000.0 / per_frame_ms);
    logf("throughput  : %.2f MB/s at %d MHz SPI\n",
                  mbps, LCD_SPI_HZ / 1000000);
    logln("This is the number the OS renderer has to live inside:");
    logln("redrawing the whole screen every frame is not affordable,");
    logln("so stage 3 gets a dirty-rectangle compositor.");
}

static void draw_test_pattern(void)
{
    lcd_fill_screen(COLOR_BLACK);

    const uint16_t bars[] = {
        COLOR_RED, COLOR_GREEN, COLOR_BLUE, COLOR_WHITE,
        COLOR_YELLOW, COLOR_CYAN, COLOR_MAGENTA
    };
    const int n = sizeof(bars) / sizeof(bars[0]);
    uint16_t bw = lcd_width() / n;
    for (int i = 0; i < n; ++i) {
        lcd_fill_rect(i * bw, 0, bw, 60, bars[i]);
    }

    // Corner markers - if these land in the right corners, MADCTL/rotation is right.
    lcd_fill_rect(0, 0, 16, 16, COLOR_WHITE);
    lcd_fill_rect(lcd_width() - 16, lcd_height() - 16, 16, 16, COLOR_WHITE);
}

// Lights up a box for each button so the wiring can be checked on the panel
// itself, without watching the serial log.
static void draw_button_state(uint32_t held)
{
    struct { uint32_t bit; uint16_t x, y; } slots[] = {
        { BTN_UP,     140, 140 }, { BTN_DOWN,  140, 220 },
        { BTN_LEFT,    80, 180 }, { BTN_RIGHT, 200, 180 },
        { BTN_OK,     320, 180 }, { BTN_BACK,  390, 180 },
    };
    for (auto &s : slots) {
        lcd_fill_rect(s.x, s.y, 56, 56, (held & s.bit) ? COLOR_GREEN : RGB565(40, 40, 40));
    }
}

void setup()
{
    log_init();
    delay(1500);                       // let USB CDC enumerate before we talk

    report_memory();
    alloc_framebuffer();

    logln();
    lcd_ctrl_t ctrl = lcd_probe_run();
#ifdef LCD_FORCE_CTRL
    ctrl = LCD_FORCE_CTRL;   // set in platformio.ini when the panel cannot be read
#endif
    logf("[lcd] controller: %s\n", lcd_ctrl_name(ctrl));

    esp_err_t err = lcd_bus_init(LCD_SPI_HZ);
    if (err != ESP_OK) {
        logf("[lcd] SPI bus init failed: %d\n", (int)err);
        return;
    }
    if (!lcd_panel_init(ctrl)) return;

    lcd_set_rotation(1);               // landscape 480x320
    lcd_backlight(true);
    logf("[lcd] up: %ux%u, %u bytes/pixel\n",
                  lcd_width(), lcd_height(), lcd_bytes_per_pixel());

    benchmark();
    draw_test_pattern();

    input_init();
    draw_button_state(0);

    logln();
    logln("--- button test: press each button, watch here and the panel ---");
}

void loop()
{
    input_poll();

    uint32_t pressed  = input_pressed();
    uint32_t released = input_released();

    if (pressed || released) {
        for (int i = 0; i < BTN_COUNT; ++i) {
            uint32_t bit = 1u << i;
            if (pressed  & bit) logf("[btn] %-5s down\n", input_name(bit));
            if (released & bit) logf("[btn] %-5s up\n",   input_name(bit));
        }
        draw_button_state(input_held());
    }

    delay(5);
}
