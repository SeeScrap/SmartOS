#pragma once
#include <stdint.h>

#include "gfx/fb.h"

// ---------------------------------------------------------------------------
// Stage 5.1 - a shell on the serial console.
//
// The same USB cable that carries the log takes commands: type a line, press
// Enter. It is how the system is looked at and driven without its buttons -
// which, on this board, are not wired yet - and how the later stage 5 tests
// get in.
//
//     help                  the list below
//     ps                    kernel tasks, then apps
//     free                  internal RAM, PSRAM, the kernel heap
//     df                    the filesystem
//     log [n]               the last n lines of the system log (20)
//     ls [path] / cat path  files under /fs, and nowhere else
//     open app / close app  what Home and hold-BACK do
//     set [key value]       list the settings, or change one
//     key btn...            tap buttons: up down left right ok back
//     hold btn              press and hold one (hold back closes the app)
//     screenshot            the screen as hex, for tools/screenshot.py
//     reboot
//
// WHERE IT RUNS
//
// shell_poll() is called from core 0's main loop, so a command runs in the
// same context as the mailbox handlers and the rest of the system loop: it can
// call slog(), settings_set() and app_start() without new locking. It is only
// polled once the boot tests are done; a line typed earlier waits (USB flow
// control holds it on the host).
//
// Input is read straight from the USB Serial/JTAG receive FIFO. No driver is
// installed, so output - stdio from core 0, the ROM console from core 1 - is
// exactly as before.
// ---------------------------------------------------------------------------

#define SHELL_LINE 96            // longest command line, including arguments

void     shell_init(void);       // core 0, before the main loop: the first prompt
void     shell_poll(void);       // core 0's main loop: reads input, runs whole lines
uint32_t shell_commands(void);   // lines run, ever - for tests
void     shell_set_screen(const fb_t *fb);   // for the screenshot command
