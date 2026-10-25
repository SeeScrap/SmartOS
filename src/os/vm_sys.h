#pragma once

#include "gfx/fb.h"
#include "os/app.h"
#include "os/vm.h"

// ===========================================================================
// What an installed app can ask the system to do.
// สิ่งที่แอปซึ่งติดตั้งทีหลังขอให้ระบบทำได้
// ===========================================================================
//
// THIS TABLE IS THE ABI. Not include/smartos_app.h - that is the interface
// for an app compiled into the firmware. An installed app has no symbols and
// no addresses; it has these numbers, and nothing else reaches the system.
//
// So the rules are the strict ones:
//   - a number, once used, keeps its meaning for ever
//   - new calls are appended, never inserted
//   - a call that has to change gets a new number, and the old one stays
//   - SMARTOS_ABI_VERSION goes up only when one of those is broken anyway
//
// ตารางนี้คือ ABI ตัวจริง ไม่ใช่ smartos_app.h ซึ่งเป็นอินเทอร์เฟซของแอปที่
// คอมไพล์เข้าเฟิร์มแวร์ แอปที่ติดตั้งไม่มีสัญลักษณ์และไม่มีแอดเดรส มีแต่
// หมายเลขเหล่านี้ กฎจึงเข้มงวด: หมายเลขที่ใช้แล้วความหมายห้ามเปลี่ยน เพิ่ม
// ได้เฉพาะต่อท้าย และห้ามแทรกกลาง
//
// STRINGS, since the machine has no pointers
//
// A string is an int32 handle. Zero or above is an offset into the app's own
// string blob, which was loaded with it and never changes. Below zero is one
// of the scratch buffers that sys_fmt and sys_num write into - -1 is the
// first, -2 the second. Both are bounds-checked on every use; a handle that
// is neither stops the app.
// ---------------------------------------------------------------------------

#define VM_SCRATCH_SLOTS 2
#define VM_SCRATCH_BYTES 64

// What the system calls are given, through vm_syscalls_t::user. One of these
// exists per running installed app.
typedef struct {
    const vm_program_t *prog;     // for the string blob
    app_ctx_t          *app;      // for app_invalidate and the app's identity
    fb_t               *fb;       // only while draw() is running; null otherwise
    char                scratch[VM_SCRATCH_SLOTS][VM_SCRATCH_BYTES];
    uint8_t             next_scratch;
} vm_app_t;

// The table, and how many entries it has. os/vm_sys.cpp owns both.
extern const vm_syscall_fn *vm_sys_table(void);
extern uint16_t             vm_sys_count(void);

// The numbers. An app refers to these and nothing else.
enum {
    VMSYS_INVALIDATE   = 0,   // ()                      ask for a redraw
    VMSYS_MILLIS       = 1,   // ()                      milliseconds since boot
    VMSYS_RAND         = 2,   // (n)                     0 .. n-1
    VMSYS_RGB          = 3,   // (r, g, b)               a colour
    VMSYS_BG           = 4,   // ()                      the app area's background
    VMSYS_CLEAR        = 5,   // (colour)
    VMSYS_FILL_RECT    = 6,   // (x, y, w, h, colour)
    VMSYS_RECT         = 7,   // (x, y, w, h, colour)
    VMSYS_LINE         = 8,   // (x0, y0, x1, y1, colour)
    VMSYS_TEXT         = 9,   // (x, y, str, colour, scale)      -> x just past
    VMSYS_TEXT_BG      = 10,  // (x, y, str, colour, bg, scale)  -> x just past
    VMSYS_TEXT_WIDTH   = 11,  // (str, scale)                    -> pixels
    VMSYS_FMT          = 12,  // (str, a, b, c)   %d three times -> a handle
    VMSYS_NUM          = 13,  // (value)                         -> a handle
    VMSYS_SETTING_GET  = 14,  // (str)                           -> value
    VMSYS_SETTING_SET  = 15,  // (str, value)                    -> 1 if it took
    VMSYS_LOG          = 16,  // (str)
    VMSYS_WIDTH        = 17,  // ()                      the app area's width
    VMSYS_HEIGHT       = 18,  // ()                      the app area's height
    VMSYS_COUNT_,             // not a call: how many there are
};
