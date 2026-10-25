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

#include <stdint.h>

#include "esp_attr.h"

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
