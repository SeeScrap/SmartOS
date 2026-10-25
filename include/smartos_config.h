#pragma once
// ===========================================================================
// SmartOS - every compile-time setting, in one file.
// SmartOS - ค่าตั้งค่าตอนคอมไพล์ทั้งหมดของระบบ รวมอยู่ในไฟล์เดียว
// ===========================================================================
//
// This is the file to open when you want to change how the system is built.
// Nothing outside it defines a tunable number; if you find one, that is a bug
// in this file, not a style preference.
//
// ไฟล์นี้คือที่เดียวที่ต้องเปิดเวลาอยากเปลี่ยนค่าของระบบ ถ้าเจอค่าที่ปรับได้
// อยู่ที่อื่น ถือว่าไฟล์นี้บกพร่อง ไม่ใช่เรื่องรสนิยมการจัดโค้ด
//
// HOW TO CHANGE SOMETHING / วิธีแก้ค่า
//   Edit the number here, or override it from the build without touching this
//   file at all - every entry is wrapped in #ifndef, so
//       build_flags = -D K_MAX_TASKS=48
//   in platformio.ini wins over what is written below.
//   แก้เลขในไฟล์นี้ได้ตรงๆ หรือจะ override จาก build โดยไม่แตะไฟล์นี้เลยก็ได้
//   เพราะทุกค่าห่อด้วย #ifndef ไว้แล้ว
//
// SOME NUMBERS HERE WERE MEASURED, NOT CHOSEN / บางค่ามาจากการวัด ไม่ใช่การเดา
//   Where a comment explains a number, that comment is the reason the number
//   is what it is - usually something that went wrong on the board once. Read
//   it before changing the value.
//   ค่าไหนมีคอมเมนต์อธิบาย คอมเมนต์นั้นคือเหตุผลที่มันเป็นเลขนี้ ส่วนใหญ่มาจาก
//   สิ่งที่เคยพังบนบอร์ดจริง อ่านก่อนแก้
//
// Settings the user changes while the system is running (backlight, screen
// timeout, wallpaper) are not here - they live in flash, see os/settings.h.
// ค่าที่ผู้ใช้เปลี่ยนได้ตอนระบบทำงาน (ความสว่าง เวลาปิดจอ ภาพพื้นหลัง) ไม่ได้
// อยู่ที่นี่ มันเก็บในแฟลช ดู os/settings.h
//
// ---------------------------------------------------------------------------
// CONTENTS / สารบัญ
//   1. Tasks and the scheduler / งานและตัวจัดคิว
//   2. Memory / หน่วยความจำ
//   3. The cross-core mailbox / กล่องข้อความข้ามคอร์
//   4. Locks / ล็อก
//   5. Parking core 1 for flash / การพักคอร์ 1 ตอนเขียนแฟลช
//   6. Apps / แอป
//   7. The event bus / บัสเหตุการณ์
//   8. Files / ไฟล์
//   9. The system log / บันทึกระบบ
//  10. Saved settings / ค่าที่จำไว้ในแฟลช
//  11. The shell / เชลล์
//  12. Pins, ADC and sampling / ขา ADC และการสุ่มสัญญาณ
//  13. The chip / ตัวชิป
//  14. The screen / หน้าจอ
//  15. The buttons / ปุ่ม
//
// The board's pin numbers are NOT here. They are one file per board, in
// src/board/, because a second board adds a file rather than editing this one.
// ขาของบอร์ดไม่ได้อยู่ที่นี่ แต่แยกเป็นไฟล์ละบอร์ดใน src/board/ เพื่อให้บอร์ด
// ตัวที่สองแค่เพิ่มไฟล์ ไม่ต้องมาแก้ไฟล์นี้
// ---------------------------------------------------------------------------


// ===========================================================================
// 1. TASKS AND THE SCHEDULER / งานและตัวจัดคิว            (kernel/ktask.h)
// ===========================================================================

// 32 is the ceiling, not a guess: the wait sets in ksync.h are one uint32_t
// per object, one bit per task (a static_assert there holds the two together).
// It was 16 until stage 4.2, when the stage 2 demo plus the input sampler had
// filled every slot and the event bus's timer service had nowhere to go.
#ifndef K_MAX_TASKS
#define K_MAX_TASKS   32
#endif

// Two, and the kernel means it: core 0 runs ESP-IDF, core 1 is ours. Several
// places name core 1 by number, so this is a statement of fact about the
// chip, not a dial. See kernel/kcore1.h.
#ifndef K_MAX_CPUS
#define K_MAX_CPUS    2
#endif

// 2 KB per task. Measured high-water across the whole demo was 704 bytes, so
// this is still 3x headroom, and it keeps twelve tasks inside a 48 KB heap.
#ifndef K_STACK_SIZE
#define K_STACK_SIZE  2048
#endif

// Stage 5.4d - a task may ask for another size. Apps declare theirs in
// app_t.stack: a screen that keeps a line buffer on the stack needs more than
// 2 KB, and an app that does almost nothing can have less and leave the pool
// for something else. The floor is what a context switch alone needs below the
// stack pointer (see K_STACK_GUARD) with room to do anything at all; the
// ceiling only keeps a typo from swallowing the pool.
#ifndef K_STACK_MIN
#define K_STACK_MIN   1024
#endif
#ifndef K_STACK_MAX
#define K_STACK_MAX   16384
#endif

// Dead space allocated below every stack.
//
// A canary at the bottom of a stack only detects an overrun at the next switch
// out, by which time the overrunning task has already written over whatever was
// underneath - which, since stacks come from one heap, is another task's stack.
// The first deliberate overrun test proved that the hard way: an interrupt
// watchdog timeout on core 0 and a boot loop, with the guard never getting a
// chance to speak. Giving each stack a run-off area means a modest overrun
// lands in memory nobody else owns, and the canary lives to report it.
//
// 256 is a floor, not a preference. A task that has overrun still has to be
// switched out, and switching writes roughly 200 bytes BELOW the stack pointer:
// a base save area per live window, k_yield's own frames, and the 112-byte tick
// interrupt frame if the tick happens to land there. A run-off area smaller
// than that just moves the corruption into the next heap block's header.
#ifndef K_STACK_GUARD
#define K_STACK_GUARD 256
#endif

// How many priority levels the ready table has. One round-robin ring per
// level, so this is a table width, not just a range check.
#ifndef K_PRIO_LEVELS
#define K_PRIO_LEVELS  32
#endif


// ===========================================================================
// 2. MEMORY / หน่วยความจำ                                 (kernel/kheap.h)
// ===========================================================================

// The internal pool: task stacks, app state, anything the kernel itself
// touches - and anything that must work with the flash cache off, or be the
// target of an atomic (S32C1I does not work on PSRAM).
#ifndef KHEAP_INTERNAL_SIZE
#define KHEAP_INTERNAL_SIZE  (128 * 1024)
#endif

// What the pool settles for when something else has taken the internal heap
// first - the radio, since stage 6.3 - and how much is left to ESP-IDF, which
// still has to run the filesystem, the USB console and Wi-Fi itself.
#ifndef KHEAP_INTERNAL_MIN
#define KHEAP_INTERNAL_MIN   (48 * 1024)
#endif
#ifndef KHEAP_IDF_RESERVE
#define KHEAP_IDF_RESERVE    (40 * 1024)
#endif

// PSRAM, for apps' large data: sample buffers, maps, images (app_alloc in
// os/app.h). Never for stacks: core 1 parks on the interrupted task's stack.
#ifndef KHEAP_PSRAM_SIZE
#define KHEAP_PSRAM_SIZE     (4 * 1024 * 1024)
#endif

#ifndef KHEAP_ALIGN
#define KHEAP_ALIGN          16          // Xtensa stacks want 16; so does DMA
#endif


// ===========================================================================
// 3. THE CROSS-CORE MAILBOX / กล่องข้อความข้ามคอร์         (kernel/kmbox.h)
// ===========================================================================

// Slots live in internal SRAM: S32C1I, which k_cas uses, does not work on PSRAM.
#ifndef KMBOX_SLOTS
#define KMBOX_SLOTS 8
#endif

// How long kmbox_call waits for an answer. Jobs that are slow on purpose -
// a flash erase, later - should use kmbox_call_timeout with their own figure.
#ifndef KMBOX_TIMEOUT_MS
#define KMBOX_TIMEOUT_MS 1000
#endif

// Ops from KMBOX_OP_EXT up are served by whoever registers for them, so a
// service - the device service in os/dev.cpp, first - can hand work to core 0
// without the mailbox knowing anything about it.
#ifndef KMBOX_OP_EXT
#define KMBOX_OP_EXT  16
#endif
#ifndef KMBOX_OPS
#define KMBOX_OPS     48
#endif


// ===========================================================================
// 4. LOCKS / ล็อก                                         (kernel/kspin.h)
// ===========================================================================

// Generous on purpose. The console lock is held across printf, and printf
// blocks until the USB FIFO drains - which, if the host is not reading right
// then, is far longer than a lock is normally held for. Too short a timeout
// here does not prevent a hang, it just produces interleaved output from two
// cores, which was exactly what happened at 10 ms.
#ifndef K_SPIN_TIMEOUT
#define K_SPIN_TIMEOUT 40000000u  // order of a quarter second, then give up
#endif


// ===========================================================================
// 5. PARKING CORE 1 FOR FLASH / การพักคอร์ 1 ตอนเขียนแฟลช  (kernel/kpark.h)
// ===========================================================================

// Core 1's CPU interrupt for the park request. Level 1, level-triggered.
// Core 1's interrupt allocation is entirely ours - IDF never configures it.
#ifndef KPARK_CPU_INT
#define KPARK_CPU_INT        2
#endif

// How long core 0 waits for core 1 to acknowledge a park.
#ifndef KPARK_TIMEOUT_US
#define KPARK_TIMEOUT_US  5000
#endif
#ifndef KPARK_FLASH_TIMEOUT_US
#define KPARK_FLASH_TIMEOUT_US 50000
#endif


// ===========================================================================
// 6. APPS / แอป                                              (os/app.h)
// ===========================================================================

// How many apps may be open at once.
#ifndef APP_MAX
#define APP_MAX        8
#endif

// Stage 5.5 - DOWN held, then BACK: the app in front is minimised. It keeps
// running; Home lists it under "Running" and opening it again brings it back.
//
// A DOWN press is held back this long to see whether BACK follows. A tap is
// not delayed - its release flushes the held press at once - so only a DOWN
// that is held down waits, and only by this much.
#ifndef APP_CHORD_MS
#define APP_CHORD_MS   150
#endif

// Events waiting for one app.
#ifndef APP_Q_LEN
#define APP_Q_LEN      16
#endif

// The top of the app's area: the status bar sits above it.
#ifndef APP_AREA_Y
#define APP_AREA_Y     24
#endif

// How many apps may be built into the firmware and listed in Home.
#ifndef APP_REGISTRY_MAX
#define APP_REGISTRY_MAX 16
#endif

#ifndef APP_MEM_RESERVE
#define APP_MEM_RESERVE 8192        // kernel pool kept free for the kernel itself
#endif

// No single app may take more than this of the kernel pool, stack and state
// together. Without a ceiling, "does it fit" depended on what else was open:
// the same app started on a quiet board and was refused on a busy one, and
// one careless app could leave the pool with nothing for the next. A quarter
// of the pool is a 16 KB stack (K_STACK_MAX) and still 16 KB of state, far
// past anything here: the largest app today is the task manager at about 3 KB.
#ifndef APP_KERNEL_MAX
#define APP_KERNEL_MAX (KHEAP_INTERNAL_SIZE / 4)
#endif

// An app asked to finish that has not ended this long afterwards - one stuck
// in a loop never reads the request - is killed (stage 5.2).
#ifndef APP_STOP_GRACE_MS
#define APP_STOP_GRACE_MS 2000
#endif


// ===========================================================================
// 7. THE EVENT BUS / บัสเหตุการณ์                           (os/evbus.h)
// ===========================================================================

// One slot per open app, plus the three the system always has - the app
// manager, the status bar and the screen timeout - and one spare. The
// static_assert in evbus.h holds it to at least that.
#ifndef EVBUS_MAX_SUBS
#define EVBUS_MAX_SUBS   (APP_MAX + 4)
#endif

#ifndef EVBUS_MAX_TIMERS
#define EVBUS_MAX_TIMERS  8
#endif

// The timer service sleeps until the earliest deadline, but never longer than
// this, which bounds how late a newly started timer can be for its first
// period.
#ifndef EVBUS_TIMER_MAX_SLEEP_MS
#define EVBUS_TIMER_MAX_SLEEP_MS 10
#endif


// ===========================================================================
// 8. FILES / ไฟล์                                              (os/fs.h)
// ===========================================================================

#ifndef FS_NAME_MAX
#define FS_NAME_MAX   32
#endif


// ===========================================================================
// 9. THE SYSTEM LOG / บันทึกระบบ                            (os/syslog.h)
// ===========================================================================

// Lines from kernel tasks and apps share a budget: SYSLOG_BURST at once,
// refilled at SYSLOG_RATE a second. Past it they are dropped and counted, and
// the next line that gets through says how many went missing. System code on
// core 0 is trusted and not limited.
#ifndef SYSLOG_MAX
#define SYSLOG_MAX     (32 * 1024)
#endif
#ifndef SYSLOG_LINE
#define SYSLOG_LINE    120
#endif
#ifndef SYSLOG_RATE
#define SYSLOG_RATE    20              // lines a second, sustained
#endif
#ifndef SYSLOG_BURST
#define SYSLOG_BURST   40
#endif
#ifndef SYSLOG_BUF
#define SYSLOG_BUF     2048            // lines wait in RAM, at most this many bytes
#endif
#ifndef SYSLOG_FLUSH_MS
#define SYSLOG_FLUSH_MS 2000           // ...for at most this long (see syslog.cpp)
#endif


// ===========================================================================
// 10. SAVED SETTINGS / ค่าที่จำไว้ในแฟลช                   (os/settings.h)
// ===========================================================================
// How many settings the table holds - not their values. The three the system
// ships with (backlight, screenoff, wallpaper) are registered in main.cpp.

#ifndef SETTINGS_MAX
#define SETTINGS_MAX      16
#endif
#ifndef SETTINGS_KEY_MAX
#define SETTINGS_KEY_MAX  15
#endif
#ifndef SETTINGS_SAVE_MS
#define SETTINGS_SAVE_MS  1000
#endif


// ===========================================================================
// 11. THE SHELL / เชลล์                                     (os/shell.h)
// ===========================================================================

#ifndef SHELL_LINE
#define SHELL_LINE 96            // longest command line, including arguments
#endif


// ===========================================================================
// 12. PINS, ADC AND SAMPLING / ขา ADC และการสุ่มสัญญาณ        (os/dev.h)
// ===========================================================================

#ifndef DEV_STREAMS
#define DEV_STREAMS        4
#endif
#ifndef DEV_STREAM_MIN_MS
#define DEV_STREAM_MIN_MS  2    // 500 samples a second per stream at most
#endif


// ===========================================================================
// 13. THE CHIP / ตัวชิป
// ===========================================================================

// The CPU clock, in MHz. CCOUNT advances at this rate, so anything that turns
// cycles into time divides by it.
//
// ESP-IDF owns the real setting (CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ in
// sdkconfig.defaults). This is SmartOS's copy of it, and a static_assert in
// kernel/ktime.cpp fails the build if the two ever disagree - which is the
// whole point: three places used to divide by a bare 240.
#ifndef K_CPU_MHZ
#define K_CPU_MHZ  240
#endif

// Ticks a second. 1000 gives the 1 ms tick the scheduler has always used.
#ifndef K_TICK_HZ
#define K_TICK_HZ  1000
#endif


// ===========================================================================
// 14. THE SCREEN / หน้าจอ
// ===========================================================================
// The panel is 240x320 native and used in landscape, so these are the
// rotated, logical size - what every app and the framebuffer see.

#ifndef LCD_WIDTH
#define LCD_WIDTH     320
#endif
#ifndef LCD_HEIGHT
#define LCD_HEIGHT    240
#endif

// 80 MHz. The four signal pins are the FSPI IOMUX pads, which is what makes
// this rate reachable at all; on any other pins the GPIO matrix caps it lower.
#ifndef LCD_SPI_HZ
#define LCD_SPI_HZ  (80 * 1000 * 1000)
#endif

// How long the bring-up demo scene stays on screen after the boot screen,
// before the launcher takes over. It exists to be judged by eye, so it needs
// long enough to be looked at; 0 skips it.
#ifndef GFX_DEMO_HOLD_MS
#define GFX_DEMO_HOLD_MS  1500
#endif

// The backlight is a 10-bit LEDC channel, so full brightness is this duty.
// Four places used to write 1023 by hand.
#ifndef LCD_BACKLIGHT_MAX_DUTY
#define LCD_BACKLIGHT_MAX_DUTY  1023u
#endif


// ===========================================================================
// 15. THE BUTTONS / ปุ่ม
// ===========================================================================
// Which pins they are on is in the board file; how they are read is here.

#ifndef BTN_COUNT
#define BTN_COUNT 6
#endif

#ifndef INPUT_SAMPLE_MS
#define INPUT_SAMPLE_MS        2
#endif
#ifndef INPUT_DEBOUNCE_MS
#define INPUT_DEBOUNCE_MS     12    // a level must hold this long to count
#endif
#ifndef INPUT_REPEAT_DELAY_MS
#define INPUT_REPEAT_DELAY_MS 400
#endif
#ifndef INPUT_REPEAT_EVERY_MS
#define INPUT_REPEAT_EVERY_MS 120
#endif
