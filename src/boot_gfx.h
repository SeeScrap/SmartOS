#pragma once

// ---------------------------------------------------------------------------
// Bringing the panel up: what main.cpp's boot sequence calls, in order.
//
// src/boot_gfx.cpp owns the framebuffer the whole system draws into; it is
// handed out by system_fb() in system.h. Everything here runs once, at boot,
// before the apps exist.
// ---------------------------------------------------------------------------

// Takes the framebuffer out of PSRAM. Must be first.
void alloc_framebuffer(void);

// Measures what a full frame costs through each transfer mode, with a second
// task counting how much of core 0 is left over. Runs before the kernel so
// core 0 is still ours alone.
void benchmark(void);

// The panel itself: reset, the controller's registers, the first pixels.
void gfx_bringup(void);

// Four coloured corners and one wide bar - the smallest picture that tells
// inversion, colour order and rotation apart. -D SMARTOS_GFX_SELFTEST.
void gfx_selftest(void);

// Every drawing primitive at once, for judging clipping and colour by eye.
void gfx_demo_scene(void);
