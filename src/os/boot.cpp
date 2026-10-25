#include "os/boot.h"

#include "esp_timer.h"

#include "gfx/text.h"
#include "os/app.h"
#include "os/settings.h"
#include "port/kport.h"
#include "board/lcd_panel.h"

// The look. Near-black rather than black: on this panel a true black next to
// the dark slate of the mark shows the panel's own backlight unevenness, and
// a few levels up hides it.
#define BG        fb_rgb(5, 7, 10)
#define CHIP      fb_rgb(22, 30, 42)
#define EDGE      fb_rgb(17, 125, 187)     // the same blue the CPU graph uses
#define PIN       fb_rgb(90, 100, 115)
#define NAME      fb_rgb(220, 228, 236)
#define UNDER     fb_rgb(110, 120, 135)

#define DOTS      8                        // the spinner, one ring
#define DOT_R     3
#define RING_R    16
#define SPIN_MS   90                       // a dot's worth of turn

// The spinner turns on its own, from an esp_timer task on core 0, because
// boot is not a loop: it is a sequence of long synchronous steps, and a
// spinner that only moved when one finished turned nine times in two seconds
// and looked broken. Now it turns every SPIN_MS whatever core 0 is doing -
// except during a flash write, where the cache is off and nothing on this core
// runs at all. That pause is honest: it is the filesystem, and it is short.
static esp_timer_handle_t s_tick;

static fb_t     *s_fb;
static uint32_t  s_frame;                  // spinner position, and frames drawn
static uint32_t  s_steps;
static int64_t   s_begin_us, s_end_us, s_last_us;
static const char *s_what = "";
static bool      s_shown;
static uint32_t  s_mark_px;                // of the mark, still on the panel at the end
static int64_t   s_preview_until_us;       // the shell is showing it; hands off the panel

// Where things sit. The mark is above the middle, the way a boot screen puts
// it, with the name under it and the spinner below that.
static int s_cx, s_logo_y, s_spin_y, s_text_y;

// --- the mark ----------------------------------------------------------------
// A chip: a body with a notch, eight pins a side, "S3" across the middle.
static void draw_mark(fb_t *fb)
{
    const int w = 92, h = 64;
    const int x = s_cx - w / 2, y = s_logo_y;

    for (int i = 0; i < 8; ++i) {          // the pins first, so the body laps them
        const int py = y + 6 + i * ((h - 12) / 7);
        fb_fill_rect(fb, x - 7, py, 8, 3, PIN);
        fb_fill_rect(fb, x + w - 1, py, 8, 3, PIN);
    }
    fb_fill_rect(fb, x, y, w, h, CHIP);
    fb_rect(fb, x, y, w, h, EDGE);
    fb_rect(fb, x + 1, y + 1, w - 2, h - 2, EDGE);
    fb_fill_rect(fb, x + 6, y + 5, 5, 5, EDGE);        // pin-1 notch

    const int tw = fb_text_width("S3", 5);
    fb_text(fb, s_cx - tw / 2, y + (h - 5 * TEXT_CELL_H) / 2 + 2, "S3", NAME, 5);
}

// --- the spinner --------------------------------------------------------------
// Eight dots on a ring; the leading one is brightest and three behind it fade.
// Windows turns a group of dots around a circle - this is the same idea with
// what a 5x8 framebuffer and no anti-aliasing can do.
static void draw_spinner(fb_t *fb)
{
    // Sixteenths of a turn, so no trigonometry: the eight positions of a ring
    // of radius RING_R, worked out once.
    static const int8_t dx[DOTS] = {  0,  11,  16,  11,   0, -11, -16, -11 };
    static const int8_t dy[DOTS] = { -16, -11,  0,  11,  16,  11,   0, -11 };

    for (int i = 0; i < DOTS; ++i) {
        const int behind = (int)((s_frame - (uint32_t)i) % DOTS);
        uint16_t c;
        switch (behind) {
            case 0:  c = fb_rgb(120, 200, 255); break;
            case 1:  c = fb_rgb(60, 130, 190);  break;
            case 2:  c = fb_rgb(35, 75, 110);   break;
            case 3:  c = fb_rgb(22, 40, 60);    break;
            default: c = fb_rgb(14, 20, 28);    break;
        }
        fb_fill_circle(fb, s_cx + dx[i], s_spin_y + dy[i], DOT_R, c);
    }
}

// The line under the spinner, cleared to its full width each time so a shorter
// word cannot leave the tail of a longer one behind.
static void draw_what(fb_t *fb)
{
    fb_fill_rect(fb, 0, s_text_y, fb->w, TEXT_CELL_H, BG);
    const int tw = fb_text_width(s_what, 1);
    fb_text(fb, s_cx - tw / 2, s_text_y, s_what, UNDER, 1);
}

static void paint(fb_t *fb, bool all)
{
    if (all) {
        fb_clear(fb, BG);
        draw_mark(fb);
        const int tw = fb_text_width("SmartOS", 2);
        fb_text(fb, s_cx - tw / 2, s_logo_y + 76, "SmartOS", NAME, 2);
    }
    draw_spinner(fb);
    draw_what(fb);
    s_frame++;
    if (all) fb_present(fb);
    else     fb_flush(fb);
}

// True while the callback is inside paint(). esp_timer_stop() promises that no
// NEW callback will start; it says nothing about one already running - and this
// one blocks inside the SPI driver, which lets the main task through to the
// panel behind its back. Two tasks in lcd_bus.cpp then share its transaction
// slots, and the wait there is portMAX_DELAY: one lost transaction hangs the
// boot for good. So end() waits for the callback in flight before it touches
// the panel.
static volatile bool s_in_tick;

static void tick(void *)
{
    if (!s_fb) return;
    s_in_tick = true;
    paint(s_fb, false);
    s_in_tick = false;
}

void boot_screen_begin(fb_t *fb)
{
    if (!fb || !fb->px) return;
    s_fb      = fb;
    s_cx      = fb->w / 2;
    s_logo_y  = 40;
    s_spin_y  = 172;
    s_text_y  = 200;
    s_what    = "starting";
    s_begin_us = s_last_us = esp_timer_get_time();

    paint(fb, true);

    // Fade in rather than switch on: the panel is already showing the mark, so
    // there is nothing to see arriving.
    for (int pct = 0; pct <= 100; pct += 5) {
        lcd_backlight_pct((uint8_t)pct);
        kport_yield_ms(8);
    }
    s_shown = true;

    const esp_timer_create_args_t args = { tick, nullptr, ESP_TIMER_TASK, "bootspin", true };
    if (esp_timer_create(&args, &s_tick) == ESP_OK) {
        esp_timer_start_periodic(s_tick, SPIN_MS * 1000);
    }
}

void boot_step(const char *what)
{
    if (!s_shown || !s_fb) return;
    s_steps++;
    s_what = what ? what : "";      // the spinner's own timer paints it
    s_last_us = esp_timer_get_time();
}

void boot_screen_hold(void)
{
    if (!s_shown || !s_fb) return;
    s_what = "ready";
    while ((esp_timer_get_time() - s_begin_us) / 1000 < BOOT_MIN_MS) kport_yield_ms(10);
}

// Pixels of the mark's edge colour on the panel right now. Counted at the end
// of the boot screen, where it answers the question a screenshot cannot: was
// the mark still there, or had something drawn over it? (It had: the first
// version of system_loop cleared the screen and drew the status bar before
// holding the boot screen, and this read 0.)
static uint32_t edge_pixels(const fb_t *fb)
{
    uint32_t n = 0;
    for (size_t i = 0; i < (size_t)fb->w * fb->h; ++i) if (fb->px[i] == EDGE) n++;
    return n;
}

void boot_screen_end(void)
{
    if (!s_fb) return;
    if (s_tick) {                    // the spinner stops before the panel changes hands
        esp_timer_stop(s_tick);          // no new callback starts after this
        while (s_in_tick) kport_yield_ms(1);   // ...and the one in flight finishes
        esp_timer_delete(s_tick);
        s_tick = nullptr;
    }
    s_mark_px = edge_pixels(s_fb);
    s_end_us = esp_timer_get_time();
    fb_clear(s_fb, app_bg());
    fb_present(s_fb);
    // Whatever the brightness setting says, now that it has been loaded.
    lcd_backlight_pct((uint8_t)settings_get("backlight"));
}

// Stage 6.1: paint it again, over whatever is on the panel now, without
// touching the backlight or the timings. For looking at the boot screen
// without rebooting - the shell's "bootscreen", and how the screenshots in
// this README were taken. The focused app redraws over it at its next frame.
void boot_screen_preview(fb_t *fb)
{
    if (!fb || !fb->px) return;
    s_preview_until_us = esp_timer_get_time() + (int64_t)BOOT_PREVIEW_MS * 1000;
    fb_t *keep = s_fb;
    s_fb = fb;
    s_cx = fb->w / 2;
    s_logo_y = 40;
    s_spin_y = 172;
    s_text_y = 200;
    s_what = "ready";
    paint(fb, true);
    s_fb = keep;
}

bool boot_preview_active(void)
{
    return s_preview_until_us && esp_timer_get_time() < s_preview_until_us;
}

bool     boot_screen_shown(void) { return s_shown; }
uint32_t boot_steps(void)        { return s_steps; }
uint32_t boot_frames(void)       { return s_frame; }
uint32_t boot_mark_px(void)      { return s_mark_px; }

uint32_t boot_screen_ms(void)
{
    if (!s_shown) return 0;
    const int64_t end = s_end_us ? s_end_us : esp_timer_get_time();
    return (uint32_t)((end - s_begin_us) / 1000);
}
