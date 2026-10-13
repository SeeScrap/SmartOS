#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "kernel/ksync.h"

// ---------------------------------------------------------------------------
// Stage 4.2 - the system event bus.
//
// Stage 3.6 gave the buttons a queue of their own, with exactly one reader.
// That does not survive a second reader: two apps cannot both drain one queue.
// So events now go through a bus. Anyone publishes; each subscriber owns a
// queue and a mask of the event types it wants, and a publish is copied into
// every matching queue.
//
// WHAT AN EVENT CARRIES
//
//   id   bus-wide, +1 per publish or send. Every copy of one event has the
//        same id, so two subscribers can tell they saw the same thing.
//   seq  per subscriber, +1 per event OFFERED to that subscriber - delivered
//        or dropped. A gap in seq is exactly that subscriber's losses, and
//        nobody else's traffic shows up in it.
//
// A FULL SUBSCRIBER DROPS; IT NEVER BLOCKS ANYONE
//
// Delivery is try-send only. A subscriber that stops reading loses its own
// events and counts them, and every other subscriber carries on as if it did
// not exist. The alternative - a publisher that waits for room - lets one
// stalled app freeze the input sampler, and with it the whole interface.
//
// WHO MAY CALL WHAT
//
// Publishing and try-receiving are safe from any context on either core.
// evbus_recv() blocks, so it is for kernel tasks only. Everything here is IRAM
// and DRAM, because core 1 calls it.
//
// LOCKING
//
// One spinlock guards the subscriber table and the timers, taken with
// interrupts masked like k_lock(). Delivery happens under it - that is what
// makes unsubscribe safe, since no publisher can be halfway into a queue that
// is going away. Lock order is bus, then scheduler (k_queue_try_send takes the
// scheduler lock inside); nothing may take them the other way round, and
// nothing may klog() while holding the bus lock.
//
// --- ภาษาไทย ---------------------------------------------------------------
//
// stage 4.2 - บัสเหตุการณ์ของระบบ
//
// stage 3.6 ให้ปุ่มมีคิวของตัวเอง โดยมีผู้อ่านเพียงรายเดียว ซึ่งใช้ไม่ได้ทันทีที่มี
// ผู้อ่านรายที่สอง เพราะสองแอปดึงจากคิวเดียวกันไม่ได้ เหตุการณ์จึงเปลี่ยนมาวิ่งผ่าน
// บัสแทน ใครจะส่งก็ได้ ผู้รับแต่ละรายมีคิวของตัวเองและมี bitmask บอกว่าสนใจ
// เหตุการณ์ชนิดไหน การส่งหนึ่งครั้งคือการคัดลอกลงทุกคิวที่ตรงเงื่อนไข
//
// เหตุการณ์หนึ่งพกอะไรมาบ้าง
//
//   id   เป็นเลขของทั้งบัส เพิ่มทีละหนึ่งทุกการส่ง สำเนาทุกชุดของเหตุการณ์เดียวกัน
//        มี id เดียวกัน ผู้รับสองรายจึงบอกได้ว่าเห็นเหตุการณ์เดียวกัน
//   seq  เป็นเลขของผู้รับแต่ละราย เพิ่มทีละหนึ่งทุกครั้งที่ "เสนอ" ให้ผู้รับรายนั้น
//        ไม่ว่าจะส่งถึงหรือถูกทิ้ง ช่องว่างใน seq จึงคือจำนวนที่ผู้รับรายนั้นเสียไป
//        พอดี และไม่ปนกับปริมาณของผู้รับรายอื่น
//
// คิวที่เต็มจะทิ้งเหตุการณ์ ไม่ใช่หยุดใครไว้
//
// การส่งเป็นแบบ try-send เท่านั้น ผู้รับที่เลิกอ่านจะเสียเหตุการณ์ของตัวเองและถูก
// นับไว้ ส่วนผู้รับรายอื่นทำงานต่อราวกับไม่มีรายนั้นอยู่ ทางเลือกอีกทาง - ให้ผู้ส่ง
// รอจนมีที่ว่าง - จะทำให้แอปที่ค้างหนึ่งตัวแช่แข็งตัวอ่านปุ่ม และแช่แข็งทั้งส่วนติดต่อ
// ผู้ใช้ไปด้วย
//
// ใครเรียกอะไรได้บ้าง
//
// การส่งและการรับแบบไม่รอ เรียกได้อย่างปลอดภัยจากทุกบริบทบนทั้งสองคอร์
// ส่วน evbus_recv() เป็นแบบรอ จึงใช้ได้เฉพาะใน task ของเคอร์เนล ทุกอย่างในนี้
// อยู่ใน IRAM และ DRAM เพราะ core 1 เป็นผู้เรียก
//
// การล็อก
//
// มี spinlock ตัวเดียวคุ้มครองตารางผู้รับและตัวจับเวลา โดยปิดอินเทอร์รัปต์ตอนถือ
// เหมือน k_lock() การส่งเกิดขึ้นใต้ล็อกนี้ ซึ่งเป็นสิ่งที่ทำให้การยกเลิกการสมัครรับ
// ปลอดภัย เพราะไม่มีผู้ส่งรายใดค้างอยู่กลางคิวที่กำลังจะหายไป ลำดับการถือล็อกคือ
// บัสก่อนแล้วค่อยตัวจัดคิว (k_queue_try_send ถือล็อกตัวจัดคิวอยู่ข้างใน)
// ห้ามถือสลับลำดับกัน และห้ามเรียก klog() ขณะถือล็อกของบัส
// ---------------------------------------------------------------------------

#define EVBUS_MAX_SUBS   12      // apps (up to APP_MAX) plus the app manager and the status bar
#define EVBUS_MAX_TIMERS  8

// Event types. A subscriber's mask is a set of (1u << type).
enum {
    EV_INPUT = 0,       // a = input_kind_t, b = button index
    EV_TIMER = 1,       // a = the caller's tag, b = periods this event stands for
    EV_APP   = 2,       // a, b = whatever the two apps agree on
    EV_SYS   = 3,       // system notices - see SYS_* in os/app.h
    EV_SENSOR = 4,      // a = GPIO it was read from, b = value (mV for the ADC)
};
#define EV_MASK(type)  (1u << (type))
#define EV_MASK_ALL    0xFFFFFFFFu

// Where an event came from.
enum {
    EV_SRC_SYSTEM = 0,
    EV_SRC_INPUT  = 1,
    EV_SRC_TIMER  = 2,
    EV_SRC_TEST   = 3,
    EV_SRC_DEV    = 4,      // the device service, os/dev.h
    EV_SRC_SHELL  = 5,      // typed at the serial shell, os/shell.h (stage 5.1)
    EV_SRC_APP0   = 16,     // apps are EV_SRC_APP0 + their id (stage 4.3)
};

typedef struct {
    uint32_t t_ms;      // when it happened, in k_millis() time
    uint32_t a, b;
    uint16_t id;        // bus-wide
    uint16_t seq;       // this subscriber's
    uint8_t  type;
    uint8_t  src;
    uint16_t _pad;
} ev_t;                 // 20 bytes

typedef struct {
    uint32_t delivered;
    uint32_t dropped;
} evbus_stats_t;

void evbus_init(void);

// Registers a subscriber. storage must hold capacity ev_t and outlive the
// subscription. Returns the subscriber id, or -1 if the table is full.
int  evbus_subscribe(const char *name, uint32_t mask, ev_t *storage,
                     uint32_t capacity);

// Subscriber slots nobody holds. An app needs one to start, so what is left
// says whether another can open at all (stage 5.4d).
int  evbus_subs_free(void);

// After this returns, nothing will be delivered into that storage again.
// A kernel task blocked in evbus_recv() on it is NOT woken - stop it first.
void evbus_unsubscribe(int sub);

// To every subscriber whose mask has the type. Returns how many took it.
int  evbus_publish(uint8_t type, uint8_t src, uint32_t a, uint32_t b);
// The same, with the time supplied - for a source that knows better than
// "now" when its event happened, like a debouncer that accepted an edge.
int  evbus_publish_at(uint8_t type, uint8_t src, uint32_t a, uint32_t b,
                      uint32_t t_ms);

// To one subscriber, whatever its mask says. True if it was delivered.
bool evbus_send(int sub, uint8_t type, uint8_t src, uint32_t a, uint32_t b);
// The same, with the time supplied - like evbus_publish_at.
bool evbus_send_at(int sub, uint8_t type, uint8_t src, uint32_t a, uint32_t b, uint32_t t_ms);

// Passes an event on to one subscriber unchanged - same id, time, source and
// payload - so the receiver sees the event that happened, not a copy made
// later by whoever relayed it. The app manager routes input this way.
bool evbus_forward(int sub, const ev_t *ev);

void evbus_recv(int sub, ev_t *out);          // kernel tasks only - blocks
bool evbus_try_recv(int sub, ev_t *out);      // any context

evbus_stats_t evbus_stats(int sub);
int           evbus_pending(int sub);         // queued, not yet received

// --- timers -------------------------------------------------------------------
// A periodic EV_TIMER to one subscriber, a = tag. If the service falls behind,
// it sends one event with b = the number of periods that passed, rather than a
// burst of catch-up events. Returns the timer id, or -1 if none are free.
int  evbus_timer_start(int sub, uint32_t period_ms, uint32_t tag);
void evbus_timer_stop(int timer);

// The service that fires them. Create as a kernel task on core 1. It sleeps
// until the earliest deadline, but never longer than EVBUS_TIMER_MAX_SLEEP_MS,
// which bounds how late a newly started timer can be for its first period.
#define EVBUS_TIMER_MAX_SLEEP_MS 10
void evbus_timer_task(void *arg);
