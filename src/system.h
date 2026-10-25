#pragma once

#include "gfx/fb.h"

// ---------------------------------------------------------------------------
// Stage 5.4b - what core 0's system loop offers the rest of the firmware.
//
// main.cpp is the boot sequence: bring the hardware up, start core 1, hand the
// screen to the apps, then run this loop. The test suites live in src/tests/
// and need exactly two things from it - one pass of the loop, and the screen
// they check pixels in.
// ---------------------------------------------------------------------------

// One pass of core 0's loop: serve core 1's mailbox, reap apps, flush a frame,
// save settings, flush the log, take the system's readings, watch the screen
// timeout. Safe to call as often as you like.
void  system_pass(void);

// The whole panel, including the status bar. Apps get a view of the area below
// it (see app.h), not this.
fb_t *system_fb(void);

// Core 0's loop, from app_main. Never returns.
void  system_loop(void);
