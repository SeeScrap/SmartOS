#pragma once

// The hog test runs inside the app build, so asking for it asks for that too.
// This has to live here, where every test file sees it - not in main.cpp.
#if defined(SMARTOS_TEST_APPHOG) && !defined(SMARTOS_TEST_APPS)
#define SMARTOS_TEST_APPS 1
#endif
#if defined(SMARTOS_TEST_FSFULL) && !defined(SMARTOS_TEST_APPS)
#define SMARTOS_TEST_APPS 1
#endif


// ---------------------------------------------------------------------------
// Stage 5.4b - the test suites, one file each in src/tests/.
//
// Every one of these runs on the board and prints its own lines. The ones that
// open apps are only built with -D SMARTOS_TEST_APPS - on a normal boot only the
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

// With apps running (-D SMARTOS_TEST_APPS).
int  app_model_test(int in, int ct, int bn);   // returns bounce's id
void launcher_test(void);
void close_user_apps(void);
void app_hog_test(int bn);        // -D SMARTOS_TEST_APPHOG
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
#ifdef SMARTOS_TEST_FSFULL
void sys54d_fsfull_test(void);    // the filesystem, filled (minutes)
#endif

// The stage 2 kernel demo (tests/test_kernel.cpp), started by core1_main.
// Three calls rather than one: task ids follow creation order, and core 1's
// own cpu0idle task is created between the demo and its SMP pair.
void kernel_demo_start(void);
#ifdef SMARTOS_ENABLE_SMP
void kernel_smp_demo_start(void);
#endif
#ifdef SMARTOS_PREEMPT_SOAK
void kernel_soak_start(void);
#endif

// The stress builds (tests/test_kernel_stress.cpp), started by core1_main.
#ifdef SMARTOS_TEST_SLEEP_CLAIM
void kernel_claim_start(void);
#endif
#ifdef SMARTOS_TEST_MBOX_STRESS
void kernel_mbox_stress_start(void);
#endif
#ifdef SMARTOS_TEST_EVBUS
void kernel_evbus_stress_start(void);
#endif

// Parking core 1 around flash (tests/test_park.cpp).
void park_smoke_test(void);       // 100 reads, every boot
#if defined(SMARTOS_TEST_PARK) || defined(SMARTOS_TEST_PARK_OFF)
void park_flash_test(void);       // 40 erase/write/verify rounds
void kernel_park_work_start(void);
void park_psram_alloc(void);      // core 0, before core 1 starts
#endif

// Stage 7a (tests/test_loader.cpp): can this board run code it was not built
// with? The decision this whole stage rests on.
#ifdef SMARTOS_TEST_LOADER
void loader_probe_test(void);
#endif

// Stage 7 (tests/test_vm.cpp, tests/test_appload.cpp). Both run on every
// boot: the machine installed apps run on with every guard that makes
// running them safe, and the install path itself, with the board installing
// an app on itself.
void vm_logic_test(void);
void appload_test(void);
