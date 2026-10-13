#pragma once

// The hog test runs inside the app build, so asking for it asks for that too.
// This has to live here, where every test file sees it - not in main.cpp.
#if defined(S3_TEST_APPHOG) && !defined(S3_TEST_APPS)
#define S3_TEST_APPS 1
#endif
#if defined(S3_TEST_FSFULL) && !defined(S3_TEST_APPS)
#define S3_TEST_APPS 1
#endif

// ---------------------------------------------------------------------------
// Stage 5.4b - the test suites, one file each in src/tests/.
//
// Every one of these runs on the board and prints its own lines. The ones that
// open apps are only built with -D S3_TEST_APPS - on a normal boot only the
// user opens apps - so their files are empty without it, and main.cpp calls
// them from inside the same #ifdef.
// ---------------------------------------------------------------------------

// Logic, fed a script - no hardware beyond the framebuffer. Every boot.
void input_logic_test(void);      // the debouncer
void evbus_logic_test(void);      // the event bus
void sdk_logic_test(void);        // the app SDK's own rules
void ui_logic_test(void);         // the app toolkit: lists, bars, confirmation
void gfx_damage_demo(void);       // damage tracking, and what a flush costs
void gfx_chunk_demo(void);        // one transaction per rectangle
void gfx_text_demo(void);         // the font, and text as damage

// With apps running (-D S3_TEST_APPS).
int  app_model_test(int in, int ct, int bn);   // returns bounce's id
void launcher_test(void);
void close_user_apps(void);
void app_hog_test(int bn);        // -D S3_TEST_APPHOG
void sdk_live_test(void);
void fs_live_test(void);
void sys45_live_test(void);       // settings and the system log
void sys52_kill_test(void);       // killing apps
void sys53_tasks_test(void);      // CPU accounting and the task manager
void sys55_minimise_test(void);   // minimising
void sys54_polish_test(void);     // descriptions, confirmation, screen off
void sys54d_limits_test(void);    // stacks, timers, bus slots, the kernel pool
void sys54e_quiet_test(void);     // no frames in the dark, a ceiling per app
void sys62_wallpaper_test(void);  // Home's background picture
void sys63_radio_test(void);      // the radio: what it is, what it costs, one scan
void sys64_keyboard_test(void);   // typing on the board, and keeping it
#ifdef S3_TEST_FSFULL
void sys54d_fsfull_test(void);    // the filesystem, filled (minutes)
#endif
