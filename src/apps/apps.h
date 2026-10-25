#pragma once
#include <stdarg.h>
#include <stdint.h>

#include "os/app.h"

#include "smartos_config.h"

// ---------------------------------------------------------------------------
// The apps that ship with the system. Each is an app_t and nothing else - no
// task, no queue, no drawing surface of its own; the runtime in os/app.cpp
// provides all three. Each file registers itself with APP_REGISTER, so this
// header is only needed by code that refers to a particular app by name - the
// test script, here. A new app does not have to be added to it.
//
// The peek functions exist for the stage 4.3b test script, which drives these
// apps with injected button events and needs to see what they made of them.
// ---------------------------------------------------------------------------

extern const app_t app_input;     // stage 3.6's button screen, now an app
extern const app_t app_counter;   // a number, UP/DOWN/OK, and a blinking cursor
extern const app_t app_bounce;    // a box bouncing at 50 Hz - frame pacing
extern const app_t app_scope;     // a signal reader: an ADC pin, plotted at 100 Hz
extern const app_t app_snake;     // a small game, and a template for others
extern const app_t app_hog;       // test only, not registered: never returns from init
extern const app_t app_fstest;    // test only, not registered: the stage 4.4 file checks
extern const app_t app_logspam;   // test only, not registered: logs 200 lines at once
extern volatile bool g_logspam_done;
// Stage 5.3: the task manager, and what the tests read back from it.
extern const app_t app_tasks;
int         app_tasks_pm(const app_ctx_t *c, const char *task_name);   // per mille, -1 if absent
void        app_tasks_core_pm(const app_ctx_t *c, int *busy, int *park, int *idle);
const char *app_tasks_message(const app_ctx_t *c);
int         app_tasks_selected(const app_ctx_t *c);            // app id, -1 if none
int         app_tasks_page(const app_ctx_t *c);                // 0 processes, 1 performance
int         app_tasks_history(const app_ctx_t *c, int r, int *samples);  // newest, per mille
// Test only, not registered (stage 5.2): apps that must be killed - see app_killtest.cpp.
extern const app_t app_kt_spin, app_kt_drawspin, app_kt_sleeper, app_kt_mboxer, app_kt_bigbuf, app_kt_bigger;
// Stage 5.4d - the limits: a 1 KB stack, more timers than there are, and a
// state block the kernel pool cannot hold.
extern const app_t app_kt_tiny, app_kt_timers, app_kt_hungry;
int      app_kt_tiny_depth(void);
int      app_kt_timers_got(void);
int      app_kt_timers_refused(void);
uint32_t app_kt_timer_ticks(void);

// Stage 5.4d, -D SMARTOS_TEST_FSFULL only: writes until the filesystem is full.
#ifdef SMARTOS_TEST_FSFULL
extern const app_t app_kt_filler;
int      app_kt_fill_rc(void);        // what the write that found the end returned
int      app_kt_fill_small_rc(void);  // one byte, with no room left
int      app_kt_fill_files(void);
uint32_t app_kt_fill_kb(void);
uint32_t app_kt_fill_used(void);      // KB used after deleting it all
uint32_t app_kt_fill_total(void);
uint32_t app_kt_fill_ms(void);        // fill, log, delete: the lot
bool     app_kt_fill_done(void);      // set last, when it really has finished
#endif
int app_kt_big_ok(void);        // 1: 1 MB written and read back, -1 not yet
int app_kt_big_refused(void);   // 1: 3.5 MB more was refused
uint32_t app_kt_spins(void);
uint32_t app_kt_draw_spins(void);
uint32_t app_kt_sleeps(void);
uint32_t app_kt_reads(void);
extern const app_t app_settings;  // the system's settings, stage 4.5
const char *app_settings_message(const app_ctx_t *c);   // its one-line feedback
extern const app_t app_logview;   // the system log, stage 4.5
int         app_home_selected(const app_ctx_t *c);   // Home's highlighted row
const char *app_home_selected_name(const app_ctx_t *c);  // the app on that row
const char *app_home_message(const app_ctx_t *c);       // its one-line feedback
int         app_logview_lines(const app_ctx_t *c);
const char *app_logview_line(const app_ctx_t *c, int from_end);   // 0 = newest

// Stage 6.4a: the notes app, and what it is holding - for the test script.
extern const app_t app_notes;
const char *app_notes_text(const app_ctx_t *c);
const char *app_notes_message(const app_ctx_t *c);

// FSCHECK_MAX is in smartos_config.h, section 18.
typedef struct {
    const char *what;
    bool        ok;
    char        detail[80];
} fscheck_t;
extern fscheck_t     g_fscheck[FSCHECK_MAX];
extern volatile int  g_fscheck_n;
extern volatile bool g_fscheck_done;

uint32_t app_input_inputs(const app_ctx_t *c);    // input events it received
int32_t  app_counter_value(const app_ctx_t *c);
uint32_t app_counter_inputs(const app_ctx_t *c);
uint32_t app_bounce_inputs(const app_ctx_t *c);
uint32_t app_hog_spins(void);
uint32_t app_scope_mv(const app_ctx_t *c);
uint32_t app_scope_samples(const app_ctx_t *c);
int      app_scope_pin(const app_ctx_t *c);
uint32_t app_scope_rec_lines(const app_ctx_t *c);
bool     app_snake_over(const app_ctx_t *c);
uint32_t app_snake_moves(const app_ctx_t *c);
int      app_snake_head_row(const app_ctx_t *c);
