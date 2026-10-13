#pragma once
#include <stdbool.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// Stage 5.4 - the screen switches itself off.
//
// After the "screenoff" setting's seconds with no button pressed, core 0 takes
// the backlight down to nothing. The apps keep running: a scope goes on
// recording in the dark. The next press wakes it and is swallowed, the way a
// phone does it - waking should not also do something.
//
// Core 0 owns the backlight (LEDC) and watches the clock; core 1 sees the
// buttons first. So core 0 decides when to switch off, and the app manager on
// core 1 asks it to wake through the mailbox.
//
// 0 seconds means never.
// ---------------------------------------------------------------------------

void     screen_init(void);       // core 0, after settings_load
void     screen_poll(void);       // core 0's main loop
bool     screen_is_off(void);     // either core
void     screen_wake_k(void);     // core 1: wake it now (mailbox, no timeout)
uint32_t screen_offs(void);       // times it has switched off, for tests
