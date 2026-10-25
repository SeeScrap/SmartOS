#pragma once

// ===========================================================================
// SmartOS - everything an application may use.
// SmartOS - ทุกอย่างที่แอปพลิเคชันใช้ได้
// ===========================================================================
//
// One include. An app written against this file and nothing else has the
// kernel, the screen, the event bus, the filesystem, saved settings, the
// system log, the on-screen controls and the buttons.
//
// เขียนแอปโดย include ไฟล์นี้ไฟล์เดียว แล้วจะได้เคอร์เนล หน้าจอ บัสเหตุการณ์
// ระบบไฟล์ ค่าที่จำไว้ บันทึกระบบ ส่วนควบคุมบนจอ และปุ่ม
//
// THIS LIST IS A PROMISE, NOT A CONVENIENCE
//
// Stage 7 lets a user install an app that was built separately, as a file,
// without rebuilding the operating system. Such an app is linked against
// whatever is reachable from this header, so everything here has to keep
// working, with the same name and the same meaning, for as long as apps built
// today are expected to run.
//
// That makes adding something here a decision, not a tidying-up. The question
// to ask of anything proposed for this file is not "would an app find it
// useful" but "are we prepared to keep it working". Exposing too little is
// easy to fix later; exposing too much is not fixable at all.
//
// รายการนี้คือคำสัญญา ไม่ใช่ความสะดวก การเพิ่มของเข้ามาจึงเป็นการตัดสินใจ
// คำถามไม่ใช่ "แอปจะได้ใช้ไหม" แต่คือ "เราพร้อมจะค้ำมันไปตลอดหรือเปล่า"
// เปิดน้อยไปแก้ทีหลังได้ เปิดมากไปแก้ไม่ได้เลย
//
// WHAT IS NOT HERE, AND WHY
//
// The kernel's internals (kernel/internal/), the spinlocks, the cross-core
// mailbox and the port layer. An app that needs any of those is not an app -
// it is part of the system, and it can include what it needs directly.
// ---------------------------------------------------------------------------

// The kernel: tasks, locks, the clock, the heap, printing.
#include "smartos.h"

// --- the app itself ---------------------------------------------------------
// app_t, APP_REGISTER, the context an app is handed, app_alloc, app_timer,
// app_invalidate.
#include "os/app.h"

// Events: input, timers, and messages between apps.
#include "os/evbus.h"

// --- what an app can keep ---------------------------------------------------
#include "os/fs.h"          // files under /fs, through core 0
#include "os/settings.h"    // named values that survive a reboot
#include "os/syslog.h"      // the system log, rate-limited per app

// --- what an app can look at ------------------------------------------------
#include "os/sysinfo.h"     // memory and storage figures
#include "os/krand.h"       // random numbers
#include "os/dev.h"         // pins and the ADC, through core 0

// --- drawing ----------------------------------------------------------------
#include "gfx/fb.h"         // the framebuffer, colours, rectangles, damage
#include "gfx/text.h"       // the 5x8 font
#include "gfx/image.h"      // pictures held in flash

// --- the toolkit ------------------------------------------------------------
#include "ui/ui.h"          // lists, rows, bars, confirmations
#include "ui/keyboard.h"    // typing with six buttons

// --- the buttons ------------------------------------------------------------
#include "board/input.h"    // BTN_UP and friends, press/release/repeat
