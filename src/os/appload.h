#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "smartos_config.h"

// ===========================================================================
// Apps the user installed, loaded from the filesystem.
// แอปที่ผู้ใช้ติดตั้งเอง โหลดมาจากระบบไฟล์
// ===========================================================================
//
// An installed app is a file at /fs/apps/<name>.app. At boot this reads each
// one, checks it, gives it memory from the kernel pool and registers it -
// after which Home lists it beside the apps that were built into the
// firmware, and the user opens it the same way.
//
// NOTHING IN HOME KNOWS ABOUT ANY OF THIS. app_register() has always been a
// run-time call rather than a link-time one, so an app that arrives as a file
// enters the registry by the same door as one compiled in. That was true
// before stage 7 started and is the reason the stage is as small as it is.
//
// Home ไม่รู้เรื่องนี้เลยแม้แต่น้อย เพราะ app_register() เป็นการเรียกตอนรันไทม์
// มาตั้งแต่ต้น แอปที่มาเป็นไฟล์จึงเข้าทะเบียนทางประตูเดียวกับแอปที่คอมไพล์เข้าไป
//
// THE FILE
//
// A 56-byte header, then three blocks laid end to end: the constant pool, the
// code, and the strings. Everything is little-endian, and every length in the
// header is checked against the file's actual size before a byte of it is
// trusted. See tools/mkapp.py, which writes them.
//
//   0   4   "SAPP"
//   4   2   ABI version - must equal SMARTOS_ABI_VERSION
//   6   2   flags, reserved, must be zero
//   8  16   name, NUL-padded. What Home shows.
//  24   2   where it sits in Home's list
//  26   2   globals: how many 32-bit words of state it keeps
//  28   2   stack: how deep its operand stack may go
//  30   2   how many constants
//  32   4   bytes of code
//  36   4   bytes of strings
//  40   4   entry point for init,  or 0xFFFFFFFF for none
//  44   4   entry point for an event
//  48   4   entry point for draw
//  52   4   entry point for stop
// ---------------------------------------------------------------------------

#define APPLOAD_MAGIC    0x50504153u   // "SAPP", little-endian
#define APPLOAD_HDR      56
#define APPLOAD_NAME_MAX 16
#define APPLOAD_NONE     0xFFFFFFFFu

// How many installed apps the board will hold at once. They share
// APP_REGISTRY_MAX with the built-in ones, so this is also a promise that
// installing apps cannot push a built-in one out of Home.
#ifndef APPLOAD_MAX
#define APPLOAD_MAX 6
#endif

// Where they live, and what they are called.
#define APPLOAD_DIR ".app"

// Core 0, at boot, after the kernel is up: read every app in /fs/apps and
// register the ones that are sound. Returns how many were registered.
int appload_scan(void);

// Loads one file by name, as "counter" rather than "counter.app". Returns
// true if it is now in the registry. Used by the shell after an install.
bool appload_one(const char *name);

// Forgets an installed app: it leaves the registry and gives its memory back.
// Refuses while the app is open - the caller closes it first.
bool appload_forget(const char *name);

// For the shell and the tests.
int         appload_count(void);
const char *appload_name_at(int i);
const char *appload_last_error(void);   // why the last load was refused
uint32_t    appload_bytes(void);        // kernel pool held by installed apps
