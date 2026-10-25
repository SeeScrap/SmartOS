#pragma once

// ===========================================================================
// The port layer: everything the kernel needs from the chip underneath it.
// ชั้น port: ทุกอย่างที่เคอร์เนลต้องการจากชิปที่อยู่ข้างใต้
// ===========================================================================
//
// WHAT THIS FILE IS FOR
//
// src/kernel/ is the scheduler, the locks, the heap, the clock, the mailbox
// and the fault reporter. None of that is specific to an ESP32-S3 - but it
// does need a handful of things only the chip can give it: a way to place
// code in RAM that works with the flash cache off, a console it can write to
// without taking anybody's lock, a microsecond clock, an inter-processor
// interrupt, and somewhere to carve its heap from at boot.
//
// Before R4 the kernel reached for those directly, which is how
// src/kernel/kmbox.cpp came to include s3hal/hal_time.h and pull FreeRTOS in
// behind it: the kernel depended on the HAL, which depended on the operating
// system the kernel was supposed to be replacing. This file is the seam that
// reverses that. The kernel calls kport_*; the implementation for this chip
// is src/port/kport_esp32s3.cpp.
//
// WHY THERE IS NO STRUCT OF FUNCTION POINTERS
//
// A port is usually written as a vtable. Not here. This kernel's context
// switch and spinlocks are measured in instructions, and anything on the
// switch, tick or lock path has to resolve at compile time. So the seam is
// inline functions and macros where it is hot, and plain extern "C" symbols
// resolved at link time where it is not. Porting means writing another
// kport_*.cpp, not filling in a table at run time.
//
// เหตุผลที่ไม่ใช้ตารางฟังก์ชันพอยน์เตอร์: การสลับคอนเท็กซ์และสปินล็อกของ
// เคอร์เนลนี้วัดกันเป็นจำนวนคำสั่ง ของที่อยู่บนเส้นทางนั้นต้องตัดสินตอนคอมไพล์
// ---------------------------------------------------------------------------

#include <stdarg.h>
#include <stdint.h>

#include "esp_attr.h"
#include "esp_rom_sys.h"

#include "smartos_config.h"

// --- where code and data live ---------------------------------------------
// Core 1 must not depend on the flash cache: core 0 can switch it off at any
// moment to write flash, and anything core 1 is running from flash at that
// instant would fault. Everything on core 1's path is therefore placed in
// internal RAM, and the data it touches with the cache off in internal DRAM.
//
// These are NOT an optimisation. Code that falls back to flash crashes with
// the cache off, which is why they are not conditional on anything.
#define K_IRAM  IRAM_ATTR
#define K_DRAM  DRAM_ATTR


// --- the console ----------------------------------------------------------
// Two ways to print, because the two cores cannot use the same one.
//
// CORE 1 uses kport_rom_printf, through the klog() macro in kernel/klog.h.
// printf() and the ESP_LOG macros take newlib and FreeRTOS locks that belong
// to the scheduler on core 0, and core 1 is not a FreeRTOS core at all - it has
// no task control block, so blocking on one of those would hang the core with
// nothing printed. The ROM routine pushes characters straight at the console
// peripheral with no locking, which is why IDF's own ESP_EARLY_LOGx uses it
// during start-up and on the second core. A macro, not a wrapper: a wrapper
// would need a va_list variant of a ROM routine, and klog is called from the
// fault path where fewer moving parts is the whole point.
//
// Its format set is the small ROM one: %c %d %u %x %s %p with width and zero
// padding, and no %f.
#define kport_rom_printf  esp_rom_printf

// CORE 0 uses these: full newlib formatting, under the same console lock, so a
// line from core 0 and a line from core 1 cannot interleave. s3hal/log.h's
// logf() and logln() are these two - it keeps the old names for the hundreds
// of call sites that use them.
extern "C" void kport_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
extern "C" void kport_vprintf(const char *fmt, va_list ap);
extern "C" void kport_println(const char *s);


// --- waiting ---------------------------------------------------------------
// kport_delay_us spins. It is used where there is nothing to yield to - core 1
// waking up, and the mailbox stress build holding a race window open - so it
// is inline and goes nowhere near a scheduler.
static inline void kport_delay_us(uint32_t us) { esp_rom_delay_us(us); }

// kport_yield_ms gives the core up for a while, and is CORE 0 ONLY: it goes
// into FreeRTOS, which core 1 has no task control block for. The mailbox's
// K_JOB_SLEEP handler is the caller - core 1 asks core 0 to wait, precisely
// because core 1 cannot wait this way itself.
extern "C" void kport_yield_ms(uint32_t ms);
