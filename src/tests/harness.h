#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "gfx/fb.h"
#include "os/app.h"

// ---------------------------------------------------------------------------
// Stage 5.4b - the test suites' shared tools.
//
// Everything this firmware claims is checked on the board at boot, and until
// this stage all of it lived in main.cpp, which had grown to 3,900 lines of
// boot sequence and test script mixed together. The suites are in src/tests/
// now, one file per area, and this is what they share.
//
// The default build carries only what runs on every boot - the case printers
// and the pump. The suites that open apps are inside `#ifdef S3_TEST_APPS` in
// their own files, so they cost the default firmware nothing.
//
// BUTTONS, WITHOUT HANDS
//
// press() and tap() publish the same EV_INPUT events the button sampler does,
// so the app manager and the apps cannot tell them from a finger - and the
// timing is exact, which a finger never is.
// ---------------------------------------------------------------------------

// One line of result: what was checked, what was seen, OK or FAIL.
void sdk_case(const char *what, bool ok, const char *detail);
void app_case(const char *what, bool ok, const char *detail);

// Runs core 0's loop for this long, so apps get their frames and the mailbox
// gets served while a script waits on them.
void pump(uint32_t ms);

// One button event, and a press-and-release pair. `button` is the index, not
// the mask: 0 UP, 1 DOWN, 2 LEFT, 3 RIGHT, 4 OK, 5 BACK.
void press(uint32_t button, uint8_t kind);
void tap(uint32_t button);

// Pumps until app `id`'s slot is free. Milliseconds it took, or -1 if it never
// was.
int  wait_gone(int id, uint32_t max_ms);

// Pixels of exactly this colour in fb - how a test sees what is on the panel.
int  count_colour(const fb_t *fb, uint16_t colour);

// Is this text in that file? file_has flushes the system log first, so a line
// still in RAM counts; log_has looks in both log files, since a rotation may
// have moved the line to the older one.
bool file_has_raw(const char *path, const char *needle);
bool file_has(const char *path, const char *needle);
bool log_has(const char *needle);
