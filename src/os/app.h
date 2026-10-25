#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "smartos_config.h"

#include "gfx/fb.h"
#include "os/evbus.h"

// ---------------------------------------------------------------------------
// Stage 4.3b - apps.
//
// An app is four callbacks and a name. Each running app is a kernel task of
// its own, pinned to core 1 - preemptible, so one that never yields slows the
// others down but cannot stop them, and cannot touch core 0's USB, flash or
// display work at all. Its code and data may live in flash and PSRAM like
// anything else's; stage 4.3a is what makes that safe on core 1.
//
// THE SCREEN
//
// Rows 0-23 are the status bar, drawn by core 0. Rows 24-239 are the app area.
// An app draws into a view of that area only (fb_view), so the status bar is
// out of its reach by construction, and its coordinates start at (0, 0).
//
// ONLY THE FOCUSED APP DRAWS
//
// Every app gets events; only the focused one gets input and only it may draw.
// An app asks to be drawn with app_invalidate(); the runtime calls draw() when
// the app has focus and the screen is free. `full` means the area has just been
// cleared - after gaining focus, say - and everything must be painted; when it
// is false, damage tracking sends only what draw() touched.
//
// FRAMES CHANGE HANDS
//
// The app area is one piece of memory with two users: the app draws into it on
// core 1, and core 0 flushes it to the panel. One word, changed only by
// compare-and-swap, says whose turn it is:
//
//     IDLE -> DRAWING (the focused app) -> READY -> FLUSHING (core 0) -> IDLE
//
// So a flush never reads a half-drawn frame, draw() never writes pixels that
// are on their way out, and an app cannot outrun the display: it gets
// SYS_FRAME_DONE when its frame is out, and draws again then if it still wants
// to.
//
// APPS RUN ONLY WHEN THE USER STARTS THEM
//
// Nothing starts an app on its own. At boot only Home runs - the launcher,
// which is part of the system (APP_FLAG_SYSTEM), not an app - and the user
// opens an app from its list. Holding BACK (its first REPEAT, 400 ms in)
// CLOSES the focused app: its task ends, its memory, timers and sensor
// streams go back, and Home has focus again. A closed app is not running.
//
// Until stage 4.3d every registered app was started at boot and BACK-hold
// cycled between them, all of them running whether anyone looked at them or
// not. That was the wrong rule, and it is gone.
//
// INPUT AND FOCUS
//
// The app manager - a kernel task - is the only subscriber to EV_INPUT, and
// forwards each event to the focused app alone. All focus changes go through
// the manager, so there is exactly one writer.
//
// BACKGROUND APPS KEEP RUNNING
//
// An app that loses focus still gets its timers and its sensor readings; it
// only stops drawing. That is right for a logger or a scope, and wrong for a
// game: check c->focused in the timer handler and do nothing without it, or
// the game plays on unseen. (Stage 4.3c's snake did exactly that on its first
// run, and had crashed before anyone switched to it.)
//
// WHAT AN APP MAY NOT DO
//
// Call libc's printf family (use k_snprintf - see kfmt.h), or any FreeRTOS or
// IDF driver function. Both assume a FreeRTOS task, and core 1 has none.
//
// --- ภาษาไทย ---------------------------------------------------------------
//
// stage 4.3b - แอป
//
// แอปหนึ่งตัวคือฟังก์ชันสี่ตัวกับชื่อหนึ่งชื่อ แอปที่กำลังทำงานจะเป็น task ของเคอร์เนล
// ตัวหนึ่งที่ตรึงไว้บน core 1 และถูกแย่งซีพียูได้ แอปที่ไม่ยอมปล่อยคอร์จึงทำให้
// ตัวอื่นช้าลงได้ แต่หยุดมันไม่ได้ และแตะงานฝั่ง USB แฟลช หรือจอของ core 0
// ไม่ได้เลย โค้ดและข้อมูลของแอปอยู่ในแฟลชและ PSRAM ได้เหมือนของอย่างอื่น
// สิ่งที่ทำให้ปลอดภัยบน core 1 คือกลไกใน stage 4.3a
//
// เรื่องหน้าจอ
//
// แถว 0-23 เป็นแถบสถานะซึ่ง core 0 วาด แถว 24-239 เป็นพื้นที่ของแอป แอปวาดลงใน
// มุมมองเฉพาะพื้นที่ส่วนนั้น (fb_view) แถบสถานะจึงอยู่นอกระยะเอื้อมโดยโครงสร้าง
// และพิกัดของแอปเริ่มที่ (0, 0)
//
// มีเฉพาะแอปที่อยู่หน้าสุดที่วาดได้
//
// ทุกแอปได้รับ event แต่มีเฉพาะแอปที่อยู่หน้าสุดที่ได้รับการกดปุ่มและวาดได้
// แอปขอให้วาดด้วย app_invalidate() แล้วระบบจะเรียก draw() เมื่อแอปอยู่หน้าสุด
// และจอว่าง ค่า `full` เป็นจริงแปลว่าพื้นที่เพิ่งถูกล้าง เช่นหลังเพิ่งได้หน้าจอมา
// จึงต้องวาดใหม่ทั้งหมด ถ้าเป็นเท็จ ระบบติดตามการเปลี่ยนแปลงจะส่งเฉพาะส่วนที่
// draw() แตะเท่านั้น
//
// เฟรมถูกส่งต่อระหว่างสองคอร์
//
// พื้นที่ของแอปคือหน่วยความจำก้อนเดียวที่มีผู้ใช้สองฝั่ง: แอปวาดลงไปบน core 1
// และ core 0 ส่งมันออกจอ มีตัวแปรคำเดียวที่เปลี่ยนค่าด้วย compare-and-swap
// เท่านั้น ทำหน้าที่บอกว่าตอนนี้เป็นตาของใคร:
//
//     IDLE -> DRAWING (แอปหน้าสุด) -> READY -> FLUSHING (core 0) -> IDLE
//
// การส่งออกจอจึงไม่มีทางอ่านเฟรมที่วาดค้างอยู่ draw() ไม่มีทางเขียนทับพิกเซลที่
// กำลังถูกส่งออก และแอปวาดเร็วกว่าจอไม่ได้ - มันจะได้รับ SYS_FRAME_DONE
// เมื่อเฟรมออกไปแล้ว และค่อยวาดใหม่ตอนนั้นถ้ายังอยากวาด
//
// แอปทำงานเฉพาะตอนที่ผู้ใช้เปิดเท่านั้น
//
// ไม่มีอะไรเปิดแอปขึ้นมาเอง ตอนบูตมีแต่ Home ที่ทำงาน ซึ่งเป็นส่วนหนึ่งของระบบ
// (APP_FLAG_SYSTEM) ไม่ใช่แอป แล้วผู้ใช้เลือกเปิดแอปจากรายการของมัน
// การกด BACK ค้าง (นับจาก REPEAT ครั้งแรกที่ 400 มิลลิวินาที) คือการปิดแอปที่อยู่
// หน้าสุด: task ของมันจบ หน่วยความจำ ตัวจับเวลา และสายข้อมูลเซนเซอร์ถูกคืน
// แล้ว Home กลับมาอยู่หน้าสุด แอปที่ปิดแล้วคือแอปที่ไม่ได้ทำงาน
//
// ก่อน stage 4.3d แอปที่ลงทะเบียนไว้ทุกตัวถูกเปิดตั้งแต่บูต และการกด BACK ค้าง
// คือการวนไปมาระหว่างแอป ทุกตัวทำงานอยู่ไม่ว่าจะมีคนดูหรือไม่ นั่นเป็นกฎที่ผิด
// และมันถูกเอาออกไปแล้ว
//
// การกดปุ่มและการอยู่หน้าสุด
//
// ตัวจัดการแอป ซึ่งเป็น task ของเคอร์เนล เป็นผู้รับ EV_INPUT เพียงรายเดียว
// และส่งต่อแต่ละ event ให้เฉพาะแอปที่อยู่หน้าสุด การเปลี่ยนว่าใครอยู่หน้าสุด
// ผ่านตัวจัดการนี้ทั้งหมด จึงมีผู้เขียนค่าเพียงรายเดียวเสมอ
//
// แอปที่ถูกย่อยังทำงานต่อ
//
// แอปที่หลุดจากหน้าสุดยังได้รับตัวจับเวลาและค่าจากเซนเซอร์ของตัวเอง เพียงแต่
// หยุดวาดจอ ซึ่งถูกต้องสำหรับตัวบันทึกข้อมูลหรือออสซิลโลสโคป แต่ผิดสำหรับเกม
// ให้ตรวจ c->focused ในตัวจัดการตัวจับเวลาแล้วไม่ทำอะไรถ้าไม่ได้อยู่หน้าสุด
// ไม่อย่างนั้นเกมจะเล่นต่อไปโดยไม่มีใครเห็น (เกมงูใน stage 4.3c ทำแบบนั้นพอดี
// ในการรันครั้งแรก และมันแพ้ไปแล้วก่อนที่ใครจะสลับกลับมาดู)
//
// สิ่งที่แอปทำไม่ได้
//
// เรียกตระกูล printf ของ libc (ให้ใช้ k_snprintf - ดู kfmt.h) หรือเรียกฟังก์ชัน
// ของ FreeRTOS หรือไดรเวอร์ของ IDF ทั้งสองอย่างถือว่าตัวเองอยู่ใน task ของ
// FreeRTOS ซึ่ง core 1 ไม่มี
//
// แอปคือฟังก์ชันเรียกกลับสี่ตัวกับชื่อหนึ่งชื่อ แอปที่กำลังรันแต่ละตัวเป็น task
// ของเคอร์เนลของตัวเอง ตรึงไว้ที่คอร์ 1 และถูกแย่งซีพียูได้ แอปที่ไม่ยอมคืนคอร์
// จึงทำให้ตัวอื่นช้าลงได้ แต่หยุดมันไม่ได้ และแตะงาน USB แฟลช หรือจอของคอร์ 0
// ไม่ได้เลย
//
// แถวที่ 0-23 เป็นแถบสถานะซึ่งคอร์ 0 เป็นคนวาด แถวที่ 24-239 เป็นพื้นที่ของแอป
// แอปวาดลงใน view ของพื้นที่นั้นเท่านั้น แถบสถานะจึงอยู่นอกมือของมันโดยโครงสร้าง
// และมีแค่แอปที่ได้โฟกัสเท่านั้นที่วาด
// ---------------------------------------------------------------------------

// APP_MAX, APP_CHORD_MS (the DOWN-then-BACK chord), APP_Q_LEN, APP_AREA_Y,
// APP_REGISTRY_MAX, APP_MEM_RESERVE, APP_KERNEL_MAX and APP_STOP_GRACE_MS are
// set in smartos_config.h, section 6.
#define APP_AREA_H     (LCD_HEIGHT - APP_AREA_Y)

// EV_SYS codes, in ev.a.
enum {
    SYS_FOCUS_GAINED  = 1,   // to an app
    SYS_FOCUS_LOST    = 2,   // to an app
    SYS_STOP          = 3,   // to an app: finish and exit
    SYS_FRAME_DONE    = 4,   // to the focused app: its last frame is on the panel
    SYS_FOCUS_CHANGED = 5,   // published: b = app id now focused, or 0xFF for none
    SYS_FOCUS_REQ     = 6,   // to the manager: b = app id, or 0xFE for "next"
    SYS_APP_EXITED    = 7,   // to the manager: b = app id
    SYS_FOCUS_CLAIM   = 8,   // to the manager: focus b, but only if nobody has it
    SYS_SETTING_CHANGED = 9, // published: a system setting changed (os/settings.h)
    SYS_APPS_CHANGED   = 10, // published: an app opened or ended (stage 5.5)
    SYS_REDRAW         = 11, // to the manager: the app in front should draw again
};
#define APP_FOCUS_NEXT 0xFE
#define APP_NONE       0xFF

typedef struct app_ctx app_ctx_t;

// Written with designated initializers - .name = ..., .draw = ... - and every
// field has a default, so an app names only what it uses (see app_hello.cpp).
// Only .name and .draw are required.
typedef struct {
    const char *name      = nullptr;
    const char *about     = nullptr;   // one line, shown under Home's list (5.4)
    uint32_t    mask      = 0;         // broadcast types it also wants (EV_APP, say);
                                       // input, timers and system notices arrive anyway
    size_t      user_size = 0;         // bytes of zeroed state the runtime allocates
    void (*init)(app_ctx_t *c)                         = nullptr;
    void (*on_event)(app_ctx_t *c, const ev_t *ev)     = nullptr;
    void (*draw)(app_ctx_t *c, fb_t *fb, bool full)    = nullptr;
    void (*stop)(app_ctx_t *c)                         = nullptr;
    uint32_t    flags     = 0;         // APP_FLAG_*

    // The most PSRAM this app may hold through app_alloc (stage 5.5). Asking
    // for more is refused, and an app cannot even start unless this much is
    // free - so what is open can always keep what it was promised.
    uint32_t    psram_max = 0;

    // Its task's stack, in bytes (stage 5.4d). 0 is K_STACK_SIZE, which is
    // what every app used until now; the kernel holds it to
    // K_STACK_MIN..K_STACK_MAX. An app that keeps a big buffer on the stack
    // says so here, and one that barely uses it can give the pool the rest -
    // the task manager's "stack" column shows what each is really using.
    uint32_t    stack = 0;
} app_t;

// Part of the system rather than an app: started at boot, never listed in
// Home, and not closed by BACK. Home itself is the only one.
#define APP_FLAG_SYSTEM  (1u << 0)

struct app_ctx {
    const app_t *app;
    void        *user;      // user_size bytes, zeroed, the app's own state
    int          id;
    int          sub;       // its event bus subscription
    bool         focused;

    // The runtime's; apps read them, never write them.
    bool         dirty;
    bool         full;
    bool         stopping;
    uint32_t     events;
    uint32_t     inputs;    // of those, EV_INPUT (stage 5.1: the shell's ps)
    uint32_t     frames;

    // Core 0's, for ending it (stage 5.2).
    int          task;      // its kernel task, -1 until it has one
    uint32_t     task_gen;
    uint32_t     stop_ms;   // k_millis() when app_stop asked, 0 if nobody has
    bool         killed;
    volatile bool kill_req; // app_kill_request: app_poll kills it (stage 5.3)
};

// Asks for draw() to be called, once the app has focus and the screen is free.
void app_invalidate(app_ctx_t *c);

// Large data - sample buffers, maps, images - from the kernel's PSRAM pool
// (4 MB, stage 5.3c). Zeroed. Everything an app took is given back when it
// ends, however it ends; app_free returns a block sooner. Null if there is no
// room. Not for anything core 1 touches with the flash cache off, and not a
// target for atomics (see kernel/kheap.h).
void *app_alloc(app_ctx_t *c, size_t bytes);
void  app_free(app_ctx_t *c, void *p);
size_t app_psram_bytes(const app_ctx_t *c);      // what it holds now

// A periodic EV_TIMER to this app, a = tag. Stops when the app exits.
// Negative if every timer in the system is taken (EVBUS_MAX_TIMERS) - the
// refusal goes in the system log with the app's name, and is counted here.
int  app_timer(app_ctx_t *c, uint32_t period_ms, uint32_t tag);
uint32_t app_timers_denied(void);

// The colour the app area is cleared to before a full draw.
uint16_t app_bg(void);

// --- the registry: how an app gets into the system (stage 4.3c) -------------
// Adding an app is one new file in src/apps/ that ends with
//
//     APP_REGISTER(app_mything, 60);
//
// and nothing else: CMake picks up every .cpp in src/apps/, the component is
// linked whole-archive so a file nobody calls is not dropped, and the line
// above puts the app in this list from a static constructor before app_main
// runs. The number is the app's place in Home's list. Registering an app does
// not start it: only the user does that, from Home.
bool         app_register(const app_t *app, int order);

// Takes one back out again (stage 7). Only an installed app is ever
// unregistered - a built-in one is in the firmware and cannot leave - so this
// is how uninstalling works. Refuses nothing: the caller has already checked
// that the app is closed.
bool         app_unregister(const app_t *app);

// Is that app running right now? Uninstalling one that is would leave the
// runtime holding a pointer into memory about to be freed.
bool         app_is_open(const char *name);
int          app_registry_count(void);
const app_t *app_registry_at(int i);           // in order
const app_t *app_find(const char *name);

#define APP_REGISTER(app_var, order)                                   \
    __attribute__((used)) static const bool s_registered_##app_var =   \
        app_register(&(app_var), (order))

// --- system side ---------------------------------------------------------------
// Core 0, once, before any app starts: the screen the apps draw on.
void app_system_init(fb_t *screen);

// Starts an app on core 1. Returns its id, or -1 (no slot, no memory, no
// subscription). It takes focus only if nothing has it; Home opens an app
// with app_start() followed by app_focus().
int  app_start(const app_t *app);

// Stage 5.5. Could this app be opened right now? APP_OK, or why not. Home
// asks before opening, so it can say what is missing instead of just failing.
// Apps stay open in the background once minimised, so this is what stops the
// board being filled past what it has.
// PSRAM is judged against what the open apps were promised, not only against
// what they have taken: an app that is running keeps its budget.
typedef enum {
    APP_OK = 0,
    APP_NO_SLOT,          // all APP_MAX slots are taken
    APP_NO_MEMORY,        // the kernel pool cannot hold its stack and state
    APP_NO_PSRAM,         // less free than its psram_max
    APP_NO_BUS,           // no subscriber slot left on the event bus (5.4d)
    APP_TOO_BIG,          // over APP_KERNEL_MAX on its own (5.4e)
} app_can_t;
app_can_t app_can_start(const app_t *app);
const char *app_can_why(app_can_t r);

// Asks an app to finish. It exits at its next event; focus moves on if it had
// it. An app that has not ended APP_STOP_GRACE_MS later - one stuck in a loop
// never reads the request - is killed (stage 5.2).
void app_stop(int id);

// Stage 5.2. Ends an app now, whatever it is doing - k_task_kill underneath.
// Core 0 main loop only (the shell, the stop watchdog), never from inside a
// mailbox handler: see kmbox_forget. Returns the k_kill_t, or -1 for no such
// app or one still starting, -2 for Home.
int  app_kill(int id);

// The same, from anywhere - an app on core 1, the task manager first. Only
// marks the app; app_poll does the kill on core 0 at its next pass. False for
// no such app, or Home.
bool app_kill_request(int id);

// Core 0, from its main loop: ends apps that have finished or been killed -
// their streams, subscription, memory and slot - and kills any that ignored
// app_stop for APP_STOP_GRACE_MS. Every app's end goes through here, the
// ordinary kind too: one path, run after the task is known not to run again.
void     app_poll(void);
uint32_t app_kills(void);                    // apps killed, ever
uint32_t app_frames_rescued(void);           // screens freed from a killed app mid-draw
void     app_last_forgot(uint32_t *claimed, uint32_t *posted, uint32_t *done);   // last kill's

// Requests focus for an app, or APP_FOCUS_NEXT. Handled by the manager.
void app_focus(int id);

// Stage 6.4d: asks the app in front for one more frame. What the screen uses
// when the backlight comes back, because the frames drawn while the panel was
// dark were thrown away and an app that draws only on events has no reason to
// draw again. Safe from either core.
void app_redraw_focused(void);

int              app_focused(void);          // APP_NONE if nobody
const app_ctx_t *app_get(int id);            // null if that slot is empty
int              app_count(void);            // everything running, Home included
int              app_user_count(void);       // apps the user opened: Home excluded
int              app_home_id(void);          // Home's id, or APP_NONE
int              app_running(const app_t *app);   // its id if running, else -1

// Core 0, from its main loop: flushes the app area if a frame is waiting.
// Returns true if it took one - which it does even while the screen is off,
// when it takes the frame and sends nothing (stage 5.4e). The first flush
// after the screen comes back sends the whole area.
bool     app_compositor_poll(void);
uint32_t app_frames_flushed(void);
uint32_t app_frames_dark(void);         // frames taken but not sent
uint32_t app_pixels_sent(void);         // pixels that did reach the panel (2 bytes each)

// The manager task. Create on core 1 at HIGH priority, before any app.
void     app_manager_task(void *arg);
