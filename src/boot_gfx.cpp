// Bringing the panel up, and the measurements that say it came up right.
//
// This is boot, not a test: gfx_bringup() is what gives the rest of the system
// a framebuffer. The self-test, the demo scene and the benchmark sit beside it
// because they run once, at the same moment, against the same panel - and
// because main.cpp should read as a boot sequence rather than a bring-up log.
//
// Moved out of main.cpp in R2d of the SmartOS restructuring.

#include "boot_gfx.h"
#include "system.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "board/devkitc_n16r8.h"
#include "gfx/fb.h"
#include "gfx/text.h"
#include "port/kport.h"
#include "board/input.h"
#include "board/lcd_bus.h"
#include "board/lcd_panel.h"
#include "board/log.h"
#include "os/app.h"
#include "os/boot.h"

static fb_t s_fb;

fb_t *system_fb(void) { return &s_fb; }

void alloc_framebuffer(void)
{
    if (!fb_init(&s_fb, LCD_WIDTH, LCD_HEIGHT)) return;
    logf("framebuffer: %ux%u RGB565 in PSRAM, %u bytes at %p\n",
         s_fb.w, s_fb.h, (unsigned)s_fb.bytes, (void *)s_fb.px);
    logf("             PSRAM left %lu bytes\n",
         (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

// --- stage 3.2: is PSRAM fast enough to draw into? -------------------------
//
// Moving the framebuffer off-chip only helps if filling it is cheaper than
// sending it. If a full-screen fill in PSRAM costs more than the 20 ms the SPI
// transfer takes, the bottleneck has simply moved and the compositor would be
// waiting on memory instead of on the panel.
void gfx_bringup(void)
{
    if (!s_fb.px) return;

    logln();
    logln("--- stage 3.2: framebuffer in PSRAM ---");

    int64_t t0 = kport_now_us();
    fb_clear(&s_fb, FB_BLACK);
    double memset_ms = (kport_now_us() - t0) / 1000.0;

    t0 = kport_now_us();
    fb_clear(&s_fb, FB_BLUE);
    double write_ms = (kport_now_us() - t0) / 1000.0;

    t0 = kport_now_us();
    fb_fill_rect(&s_fb, 20, 20, 200, 120, FB_RED);
    double rect_ms = (kport_now_us() - t0) / 1000.0;

    logf("  clear (memset path) : %6.2f ms  %5.1f MB/s\n",
         memset_ms, (s_fb.bytes / 1e6) / (memset_ms / 1000.0));
    logf("  clear (pixel loop)  : %6.2f ms  %5.1f MB/s   <- the real cost\n",
         write_ms, (s_fb.bytes / 1e6) / (write_ms / 1000.0));
    logf("  fill 200x120        : %6.2f ms\n", rect_ms);

    // Now the way out. Staged copies PSRAM into internal RAM first; direct asks
    // the DMA engine to read PSRAM itself, which is worth a great deal if it
    // works at all.
    t0 = kport_now_us();
    fb_present(&s_fb);
    double staged_ms = (kport_now_us() - t0) / 1000.0;

    t0 = kport_now_us();
    lcd_blit_wire_direct(0, 0, s_fb.w, s_fb.h, (const uint8_t *)s_fb.px,
                         (size_t)s_fb.w * 2);
    double direct_ms = (kport_now_us() - t0) / 1000.0;

    logf("  present, staged     : %6.2f ms  %5.1f FPS\n",
         staged_ms, 1000.0 / staged_ms);
    logf("  present, DMA from PSRAM: %6.2f ms  %5.1f FPS\n",
         direct_ms, 1000.0 / direct_ms);
    logln("  (if the screen is right after this line, DMA reads PSRAM fine)");
}


// A screen designed to be readable off a bad photograph.
//
// The demo scene could not settle what was wrong, because every candidate
// explanation - inversion, swapped colour order, a rotation problem - would
// change several things at once and the picture was too dim to tell them apart.
// This draws four unmistakable things instead, each of which isolates exactly
// one question.
void gfx_selftest(void)
{
    if (!s_fb.px) return;

    const int bw = 70, bh = 50;

    fb_clear(&s_fb, FB_BLACK);
    fb_fill_rect(&s_fb, 0,                0,                bw, bh, FB_RED);
    fb_fill_rect(&s_fb, s_fb.w - bw,      0,                bw, bh, FB_GREEN);
    fb_fill_rect(&s_fb, 0,                s_fb.h - bh,      bw, bh, FB_BLUE);
    fb_fill_rect(&s_fb, s_fb.w - bw,      s_fb.h - bh,      bw, bh, FB_WHITE);

    // A bar across the middle that is obviously not square, so a rotation or a
    // stride problem cannot hide as "it looks about right".
    fb_fill_rect(&s_fb, 0, s_fb.h / 2 - 8, s_fb.w, 16, FB_YELLOW);

    fb_present(&s_fb);

    logln();
    logln("--- gfx self-test: what is on the glass? ---");
    logln("  background        : BLACK");
    logln("  top-left  70x50   : RED");
    logln("  top-right 70x50   : GREEN");
    logln("  bottom-left       : BLUE");
    logln("  bottom-right      : WHITE");
    logln("  wide bar, middle  : YELLOW, full width, 16 px tall");
    logln("Anything different tells us which of these is wrong:");
    logln("  background white       -> inversion (drop 0x21 from init_st7789)");
    logln("  red and blue swapped   -> colour order (s_bgr)");
    logln("  corners in wrong spots -> rotation / MADCTL");
    logln("  bar is tall and narrow -> rotation, width and height transposed");
}

// Draws something with every primitive, so the clipping and the colour order
// can be judged by eye rather than asserted.
void gfx_demo_scene(void)
{
    if (!s_fb.px) return;

    fb_clear(&s_fb, fb_rgb(16, 20, 32));

    // The same bar status_draw() paints, so the demo shows what the system
    // really looks like: APP_AREA_Y - 1, not 24. The separator is the LAST row
    // before the app area, and at 24 it sat on the app area's first row.
    fb_fill_rect(&s_fb, 0, 0, s_fb.w, APP_AREA_Y - 1, fb_rgb(40, 48, 72));
    fb_hline(&s_fb, 0, APP_AREA_Y - 1, s_fb.w, FB_CYAN);

    const uint16_t swatch[] = { FB_RED, FB_GREEN, FB_BLUE, FB_YELLOW,
                                FB_CYAN, FB_MAGENTA, FB_WHITE, FB_GREY };
    for (int i = 0; i < 8; ++i) {
        fb_fill_rect(&s_fb, 10 + i * 38, 40, 30, 40, swatch[i]);
        fb_rect(&s_fb, 10 + i * 38, 40, 30, 40, FB_WHITE);
    }

    // Deliberately off the edges: if clipping is wrong this corrupts memory
    // rather than drawing a partial box, so it is worth having in the scene.
    fb_fill_rect(&s_fb, -40, 100, 100, 50, fb_rgb(200, 80, 0));
    fb_fill_rect(&s_fb, s_fb.w - 60, 100, 100, 50, fb_rgb(0, 160, 120));
    fb_fill_rect(&s_fb, 120, s_fb.h - 20, 80, 100, fb_rgb(120, 0, 160));

    for (int i = 0; i < 16; ++i) {
        fb_vline(&s_fb, 20 + i * 6, 170, 10 + i * 3, FB_WHITE);
    }

    fb_rect(&s_fb, 0, 0, s_fb.w, s_fb.h, FB_RED);   // 1px border, both extremes

    fb_present(&s_fb);
    logln("  scene drawn: 8 swatches, 3 clipped rects, 16 bars, 1px border");
}

// Counts while the display is being written to.
//
// Wall-clock frame time alone cannot tell polling from queued - both push the
// same bytes at the same clock. What changes is whether the core is available
// during those milliseconds, and the only way to see that is to give it
// something else to do and count how much of it got done.
//
// It sits below the benchmark in priority, so it advances only when the
// benchmark is genuinely blocked rather than merely sharing.
static volatile DRAM_ATTR uint32_t s_cpu_probe;
static volatile DRAM_ATTR bool     s_cpu_probe_run = true;

static void cpu_probe_task(void *arg)
{
    (void)arg;
    while (s_cpu_probe_run) s_cpu_probe = s_cpu_probe + 1;
    vTaskDelete(NULL);
}

static void benchmark_mode(const char *label, lcd_xfer_mode_t mode)
{
    static const uint16_t colors[] = {
        COLOR_RED, COLOR_GREEN, COLOR_BLUE, COLOR_WHITE,
        COLOR_YELLOW, COLOR_CYAN, COLOR_MAGENTA, COLOR_BLACK
    };
    const int frames = sizeof(colors) / sizeof(colors[0]);

    lcd_bus_set_mode(mode);

    uint32_t spin0 = s_cpu_probe;
    int64_t  t0    = kport_now_us();
    for (int i = 0; i < frames; ++i) lcd_fill_screen(colors[i]);
    int64_t  dt    = kport_now_us() - t0;
    uint32_t spins = s_cpu_probe - spin0;

    double per_frame_ms = (dt / 1000.0) / frames;
    size_t frame_bytes  = (size_t)lcd_width() * lcd_height() * lcd_bytes_per_pixel();
    double mbps         = (frame_bytes / (per_frame_ms / 1000.0)) / 1e6;

    logf("  %-8s %6.2f ms/frame  %5.1f FPS  %4.2f MB/s   other work done: %lu\n",
         label, per_frame_ms, 1000.0 / per_frame_ms, mbps,
         (unsigned long)spins);
}

void benchmark(void)
{
    logln();
    logln("--- full-screen fill: polling vs queued ---");
    logf("frame is %ux%u at %u bytes/pixel = %u bytes on the wire\n",
         lcd_width(), lcd_height(), lcd_bytes_per_pixel(),
         (unsigned)((size_t)lcd_width() * lcd_height() * lcd_bytes_per_pixel()));

    // A task to soak up whatever core 0 is not using, and a priority above it
    // so it only runs when we are actually blocked.
    s_cpu_probe_run = true;
    xTaskCreatePinnedToCore(cpu_probe_task, "cpuprobe", 2048, nullptr, 1, nullptr, 0);
    UBaseType_t was = uxTaskPriorityGet(NULL);
    vTaskPrioritySet(NULL, 3);

    benchmark_mode("polling", LCD_XFER_POLLING);
    benchmark_mode("queued",  LCD_XFER_QUEUED);

    vTaskPrioritySet(NULL, was);
    s_cpu_probe_run = false;
    kport_yield_ms(5);            // let the probe task retire

    logln("'other work done' is the point: the bytes go out at the same rate");
    logln("either way, but polling spends the whole transfer watching a FIFO.");
    logln("Leaving the bus in queued mode from here.");
    lcd_bus_set_mode(LCD_XFER_QUEUED);
}

