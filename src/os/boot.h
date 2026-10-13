#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "gfx/fb.h"

// ---------------------------------------------------------------------------
// Stage 6.1 - the boot screen.
//
// Until now a boot looked like what it was: the panel came up, the graphics
// self-tests painted swatches, damage rectangles and a sheet of glyphs across
// it, and then Home appeared. Every one of those is a measurement this
// firmware wants to keep making on every boot - but nobody needs to watch it
// happen.
//
// So the backlight now stays down through bring-up. The tests still draw and
// still flush, over the same SPI at the same cost, in the dark; then this
// draws one still screen - a mark, the name, and a spinner that turns as each
// part of the system comes up - and fades the backlight in. What the board is
// doing is a line under the spinner, the way an operating system usually says
// it: short words, no numbers.
//
// The screen is held for BOOT_MIN_MS whatever happens, because a boot screen
// nobody sees is not a boot screen. It is the only place in this firmware
// where time is spent on purpose.
//
// THE MARK
//
// Drawn, not an image: a chip with its pins and "S3" across it, in the
// framebuffer's own primitives. Stage 6.2 brings real images in - when it
// does, this is the first thing that should be able to use one.
// ---------------------------------------------------------------------------

#define BOOT_MIN_MS 1500        // the least time the screen is up

// Core 0, once the panel is alive: the first frame, then the backlight fades
// in. Everything drawn before this was invisible, which is the point.
void boot_screen_begin(fb_t *fb);

// One milestone. The spinner advances and the line under it changes: short
// and lower case - "the filesystem", "the kernel on core 1".
void boot_step(const char *what);

// Keeps the screen up, spinner turning, until it has been up for BOOT_MIN_MS.
void boot_screen_hold(void);

// Hands the panel over: clears to the app background, so the launcher draws
// onto a clean screen rather than over the mark.
void boot_screen_end(void);

// Paints it again over whatever is on the panel, with no backlight change and
// no effect on the figures below: the shell's "bootscreen", for looking at it
// without rebooting. The focused app draws over it at its next frame.
void boot_screen_preview(fb_t *fb);

// True while a preview is up: the status bar leaves the panel alone until it
// is over, so a screenshot taken now is of the boot screen and nothing else.
#define BOOT_PREVIEW_MS 20000
bool boot_preview_active(void);

// For the test script.
bool     boot_screen_shown(void);   // it was drawn and faded in
uint32_t boot_screen_ms(void);      // how long it was up, end to begin
uint32_t boot_steps(void);          // milestones it was told about
uint32_t boot_frames(void);         // spinner frames drawn
uint32_t boot_mark_px(void);        // pixels of the mark still on the panel at the end
