#include "os/app.h"

#include <string.h>

#include "esp_attr.h"
#include "kernel/kheap.h"
#include "kernel/kmbox.h"
#include "kernel/kspin.h"
#include "kernel/ktask.h"
#include "kernel/ktime.h"
#include "os/dev.h"
#include "os/screen.h"
#include "os/syslog.h"
#include "s3hal/input.h"

enum { FRAME_IDLE = 0, FRAME_DRAWING, FRAME_READY, FRAME_FLUSHING };

#define BTN_INDEX_BACK 5     // input.h: BTN_BACK = 1u << 5
#define BTN_INDEX_DOWN 1     // input.h: BTN_DOWN = 1u << 1

// Every word below is shared between the cores, so all of it is internal
// DRAM: s_frame and s_used change hands through k_cas, and S32C1I does not
// work on PSRAM.
static DRAM_ATTR app_ctx_t         s_ctx[APP_MAX];

// A slot is FREE, STARTING while app_start fills its context in - on either
// core - or LIVE once it is whole. app_poll on core 0 only ever looks at LIVE
// ones: a context half written by app_start is not something to judge.
enum { SLOT_FREE = 0, SLOT_STARTING, SLOT_LIVE };
static volatile DRAM_ATTR uint32_t s_used[APP_MAX];
static DRAM_ATTR ev_t              s_queues[APP_MAX][APP_Q_LEN];

static DRAM_ATTR fb_t              s_view;          // the app area
static volatile DRAM_ATTR uint32_t s_frame = FRAME_IDLE;
static volatile DRAM_ATTR int      s_frame_owner = APP_NONE;   // who has it DRAWING
static DRAM_ATTR uint32_t          s_kills, s_rescued;
static kmbox_forgot_t              s_last_forgot;                // core 0
static volatile DRAM_ATTR int      s_focus = APP_NONE;
static volatile DRAM_ATTR int      s_home  = APP_NONE;    // the system app
static volatile DRAM_ATTR uint32_t s_flushed;

static DRAM_ATTR ev_t              s_mgr_q[32];
static DRAM_ATTR int               s_mgr_sub = -1;

static uint16_t                    s_app_bg;

static inline void barrier(void)
{
    __asm__ __volatile__("memw" ::: "memory");
}

// --- used by apps --------------------------------------------------------------

void app_invalidate(app_ctx_t *c)
{
    c->dirty = true;
}

// Stage 5.4d: there are EVBUS_MAX_TIMERS of these for the whole system, so an
// app can be told no. It used to get a bare -1, which an app that did not
// check simply never noticed - it just never ticked again. Now the refusal is
// in the system log with the app's name on it, and counted, so a screen that
// has stopped refreshing can be explained without a debugger.
static uint32_t s_timers_denied;

int app_timer(app_ctx_t *c, uint32_t period_ms, uint32_t tag)
{
    const int t = evbus_timer_start(c->sub, period_ms, tag);
    if (t < 0) {
        s_timers_denied++;
        slog_k("app", "%s asked for a timer and there are none free (%d)",
               c->app->name, EVBUS_MAX_TIMERS);
    }
    return t;
}

uint32_t app_timers_denied(void) { return s_timers_denied; }

uint16_t app_bg(void) { return s_app_bg; }

// An app's memory is tagged with its slot + 1 (0 is the kernel's own), so
// app_end can sweep the lot, whether the app closed or was killed.
static inline uint16_t owner_of(int id) { return (uint16_t)(id + 1); }

void *app_alloc(app_ctx_t *c, size_t bytes)
{
    if (!c) return nullptr;
    // Its own budget first: an app cannot take more than it declared, so what
    // app_can_start promised the others stays there (stage 5.5).
    if (kheap_owner_bytes(KHEAP_PSRAM, owner_of(c->id)) + bytes > c->app->psram_max) return nullptr;
    void *p = kmalloc_in(KHEAP_PSRAM, bytes, owner_of(c->id));
    if (p) memset(p, 0, bytes);
    return p;
}

void app_free(app_ctx_t *c, void *p)
{
    (void)c;
    kfree(p);
}

size_t app_psram_bytes(const app_ctx_t *c)
{
    return c ? kheap_owner_bytes(KHEAP_PSRAM, owner_of(c->id)) : 0;
}

// --- the runtime, one per app, on core 1 -------------------------------------

// Draws if this app is focused, wants to, and the screen is free. If the
// screen is busy - a frame still on its way out - the app stays dirty, and the
// SYS_FRAME_DONE that follows the flush brings it straight back here.
static void maybe_draw(app_ctx_t *c)
{
    if (!c->dirty || !c->focused) return;
    // The claim and the note of who made it go together: a kill takes effect
    // only at a switch-out, and with preemption held there is none between
    // them. The owner is how app_end hands the screen back from a killed app.
    k_preempt_hold();
    const bool claimed = k_cas(&s_frame, FRAME_IDLE, FRAME_DRAWING);
    if (claimed) s_frame_owner = c->id;
    k_preempt_release();
    if (!claimed) return;

    // Focus may have moved between our last event and the claim. The screen
    // is not ours to draw on then: hand it straight back.
    if (s_focus != c->id) {
        barrier();
        s_frame = FRAME_IDLE;
        return;
    }

    const bool full = c->full;
    c->dirty = false;
    c->full  = false;

    if (full) fb_clear(&s_view, s_app_bg);
    c->app->draw(c, &s_view, full);
    c->frames++;

    barrier();                 // every pixel lands before core 0 may read them
    s_frame = FRAME_READY;
    barrier();
}

static void app_task(void *arg)
{
    app_ctx_t *c = (app_ctx_t *)arg;

    slog_k("app", "%s opened (id %d)", c->app->name, c->id);
    if (c->app->init) c->app->init(c);

    // The first frame is always a complete one - whenever focus arrives.
    c->dirty = true;
    c->full  = true;

    while (!c->stopping) {
        maybe_draw(c);

        ev_t ev;
        evbus_recv(c->sub, &ev);
        c->events++;
        if (ev.type == EV_INPUT) c->inputs++;

        if (ev.type == EV_SYS) {
            switch (ev.a) {
                case SYS_FOCUS_GAINED:
                    c->focused = true;
                    c->dirty   = true;
                    c->full    = true;      // the area holds another app's pixels
                    break;
                case SYS_FOCUS_LOST:
                    c->focused = false;
                    break;
                case SYS_STOP:
                    c->stopping = true;
                    break;
                case SYS_FRAME_DONE:
                    continue;               // only a chance to draw; loop round
                default:
                    break;
            }
        }
        if (c->app->on_event) c->app->on_event(c, &ev);
    }

    if (c->app->stop) c->app->stop(c);
    // Returning ends the task: k_task_launch calls k_exit for us. Everything
    // else - streams, subscription, memory, the slot - is app_poll's, on core
    // 0, once the task is DEAD. Until stage 5.2 it happened here, which a
    // killed app never reaches.
}

// --- the registry -------------------------------------------------------------------
// Plain zero-initialised storage, so it is valid before any constructor runs -
// C++ makes no promise about the order files are initialised in, and the
// APP_REGISTER lines live in many files.
static const app_t *s_reg[APP_REGISTRY_MAX];
static int          s_reg_order[APP_REGISTRY_MAX];
static int          s_reg_n;

bool app_register(const app_t *app, int order)
{
    if (!app || s_reg_n >= APP_REGISTRY_MAX) return false;
    // Kept sorted as it fills: by order, then by name, so two apps given the
    // same number still land the same way every build.
    int i = s_reg_n++;
    while (i > 0 && (s_reg_order[i - 1] > order ||
                     (s_reg_order[i - 1] == order && strcmp(s_reg[i - 1]->name, app->name) > 0))) {
        s_reg[i]       = s_reg[i - 1];
        s_reg_order[i] = s_reg_order[i - 1];
        --i;
    }
    s_reg[i]       = app;
    s_reg_order[i] = order;
    return true;
}

int app_registry_count(void) { return s_reg_n; }

const app_t *app_registry_at(int i)
{
    return (i >= 0 && i < s_reg_n) ? s_reg[i] : nullptr;
}

const app_t *app_find(const char *name)
{
    for (int i = 0; i < s_reg_n; ++i) if (strcmp(s_reg[i]->name, name) == 0) return s_reg[i];
    return nullptr;
}

// --- system side ------------------------------------------------------------------

void app_system_init(fb_t *screen)
{
    memset((void *)s_ctx, 0, sizeof(s_ctx));
    for (int i = 0; i < APP_MAX; ++i) s_used[i] = 0;
    s_focus   = APP_NONE;
    s_home    = APP_NONE;
    s_frame   = FRAME_IDLE;
    s_flushed = 0;
    s_app_bg  = fb_rgb(16, 20, 32);

    fb_view(&s_view, screen, APP_AREA_Y, APP_AREA_H);

    // The manager's queue exists before core 1 does, so no input published in
    // between is lost for want of somewhere to go.
    // EV_TIMER too, since 5.5: a held DOWN waits APP_CHORD_MS for a BACK.
    s_mgr_sub = evbus_subscribe("appmgr", EV_MASK(EV_INPUT) | EV_MASK(EV_TIMER), EV_Q(s_mgr_q));
}

app_can_t app_can_start(const app_t *app)
{
    if (!app) return APP_NO_SLOT;
    int free_slots = 0;
    for (int i = 0; i < APP_MAX; ++i) if (s_used[i] == SLOT_FREE) free_slots++;
    if (!free_slots) return APP_NO_SLOT;

    // Every app needs a queue on the bus. There are more slots than apps, but
    // the system's own subscribers share them, so it is worth saying which
    // limit was hit rather than failing with -1 (stage 5.4d).
    if (evbus_subs_free() < 1) return APP_NO_BUS;

    // Its own ceiling first, then whether what is left can hold it. The order
    // matters for what the user is told: an app that is simply too big says so
    // on an empty board as well as a full one (stage 5.4e).
    const size_t stack = K_STACK_GUARD + k_stack_bytes_for(app->stack);
    if (stack + app->user_size > APP_KERNEL_MAX) return APP_TOO_BIG;

    kheap_stats_t in;
    kheap_stats_pool(KHEAP_INTERNAL, &in);
    const size_t need = stack + app->user_size + APP_MEM_RESERVE;
    if (in.free_bytes < need || in.largest_free < stack) return APP_NO_MEMORY;

    if (app->psram_max) {
        kheap_stats_t ps;
        kheap_stats_pool(KHEAP_PSRAM, &ps);
        // What the open apps were promised and have not taken yet is not free:
        // an app that is running must be able to reach its own budget, even
        // with others open beside it (stage 5.5).
        size_t promised = 0;
        for (int i = 0; i < APP_MAX; ++i) {
            const app_ctx_t *c = app_get(i);
            if (!c) continue;
            const size_t held = kheap_owner_bytes(KHEAP_PSRAM, owner_of(i));
            if (c->app->psram_max > held) promised += c->app->psram_max - held;
        }
        if (app->psram_max + promised > ps.free_bytes) return APP_NO_PSRAM;
        if (app->psram_max > ps.largest_free) return APP_NO_PSRAM;
    }
    return APP_OK;
}

const char *app_can_why(app_can_t r)
{
    switch (r) {
        case APP_NO_SLOT:   return "too many apps open";
        case APP_NO_MEMORY: return "not enough memory";
        case APP_NO_PSRAM:  return "not enough PSRAM";
        case APP_NO_BUS:    return "no room on the event bus";
        case APP_TOO_BIG:   return "asks for too much kernel memory";
        default:            return "";
    }
}

int app_start(const app_t *app)
{
    if (!app || !app->draw) return -1;
    if (app_can_start(app) != APP_OK) return -1;

    int id = -1;
    for (int i = 0; i < APP_MAX; ++i) {
        if (k_cas(&s_used[i], SLOT_FREE, SLOT_STARTING)) { id = i; break; }
    }
    if (id < 0) return -1;

    app_ctx_t *c = &s_ctx[id];
    memset((void *)c, 0, sizeof(*c));
    c->app  = app;
    c->id   = id;
    c->task = -1;

    if (app->user_size) {
        c->user = kmalloc_in(KHEAP_INTERNAL, app->user_size, owner_of(id));
        if (!c->user) { s_used[id] = SLOT_FREE; return -1; }
        memset(c->user, 0, app->user_size);
    }

    c->sub = evbus_subscribe(app->name, app->mask, EV_Q(s_queues[id]));
    if (c->sub < 0) {
        if (c->user) kfree(c->user);
        s_used[id] = SLOT_FREE;
        return -1;
    }

    // Core 1, and NORMAL: apps share the core round-robin with each other,
    // while the input sampler, the timer service and the manager - all HIGH -
    // stay ahead of every one of them.
    uint32_t gen = 0;
    const int task = k_task_create_stack(app->name, app_task, c, 1, K_PRIO_NORMAL,
                                         app->stack, &gen);
    if (task < 0) {
        evbus_unsubscribe(c->sub);
        if (c->user) kfree(c->user);
        s_used[id] = SLOT_FREE;
        return -1;
    }
    c->task_gen = gen;
    c->task     = task;
    barrier();
    s_used[id] = SLOT_LIVE;         // whole now: app_poll may judge it
    barrier();
    evbus_publish(EV_SYS, EV_SRC_SYSTEM, SYS_APPS_CHANGED, (uint32_t)id);

    // Not "if nobody has focus, take it": requests are handled later, by the
    // manager, so three apps started back to back all saw nobody and all
    // asked - and the last one won. The manager decides instead, being the
    // one place focus changes.
    if ((app->flags & APP_FLAG_SYSTEM) && s_home == APP_NONE) s_home = id;
    evbus_send(s_mgr_sub, EV_SYS, EV_SRC_SYSTEM, SYS_FOCUS_CLAIM, (uint32_t)id);
    return id;
}

void app_stop(int id)
{
    if (id < 0 || id >= APP_MAX || !s_used[id]) return;
    // The first request starts the clock app_poll kills by; 0 means "not asked".
    if (!s_ctx[id].stop_ms) s_ctx[id].stop_ms = k_millis() | 1u;
    evbus_send(s_ctx[id].sub, EV_SYS, EV_SRC_SYSTEM, SYS_STOP, 0);
}

int app_kill(int id)
{
    if (id < 0 || id >= APP_MAX || s_used[id] != SLOT_LIVE) return -1;
    app_ctx_t *c = &s_ctx[id];
    if (c->app->flags & APP_FLAG_SYSTEM) return -2;
    const k_kill_t r = k_task_kill((uint8_t)c->task, c->task_gen);
    if (r == K_KILL_DONE || r == K_KILL_PENDING) {
        c->killed = true;
        s_kills++;
    }
    return (int)r;
}

// Everything an app holds, handed back. Core 0, and only once its task will
// never run again - finished or killed - so nothing here races the app.
static void app_end(app_ctx_t *c, const kmbox_forgot_t *f)
{
    const int id = c->id;

    // Killed in the middle of draw(): the screen was claimed and would stay
    // claimed forever. The half-drawn frame is simply never shown.
    if (s_frame == FRAME_DRAWING && s_frame_owner == id) {
        s_frame_owner = APP_NONE;
        barrier();
        s_frame = FRAME_IDLE;
        barrier();
        s_rescued++;
    }

    dev_app_release(c->sub);           // its sensor streams stop first
    evbus_unsubscribe(c->sub);         // its timers go with it
    if (c->user) kfree(c->user);
    c->user = nullptr;
    const size_t psram = kheap_free_owner(KHEAP_PSRAM, owner_of(id));   // app_alloc's

    if (!c->killed) {
        if (psram) slog("app", "%s closed (id %d); %lu bytes of PSRAM returned",
                        c->app->name, id, (unsigned long)psram);
        else       slog("app", "%s closed (id %d)", c->app->name, id);
    } else if (f && (f->claimed | f->posted | f->done)) {
        slog("app", "%s killed (id %d); mailbox slots it left: %lu claimed, "
             "%lu posted, %lu answered", c->app->name, id, (unsigned long)f->claimed,
             (unsigned long)f->posted, (unsigned long)f->done);
    } else {
        slog("app", "%s killed (id %d)", c->app->name, id);
    }

    // The slot - and with it this context - may be handed to a new app the
    // moment it is marked free, so it is the last thing that touches c.
    barrier();
    s_used[id] = SLOT_FREE;
    barrier();

    evbus_send(s_mgr_sub, EV_SYS, EV_SRC_SYSTEM, SYS_APP_EXITED, (uint32_t)id);
    evbus_publish(EV_SYS, EV_SRC_SYSTEM, SYS_APPS_CHANGED, (uint32_t)id);
}

bool app_kill_request(int id)
{
    if (id < 0 || id >= APP_MAX || s_used[id] != SLOT_LIVE) return false;
    if (s_ctx[id].app->flags & APP_FLAG_SYSTEM) return false;
    s_ctx[id].kill_req = true;
    return true;
}

void app_poll(void)
{
    for (int id = 0; id < APP_MAX; ++id) {
        if (s_used[id] != SLOT_LIVE) continue;
        app_ctx_t *c = &s_ctx[id];

        if (c->kill_req && !c->killed) {
            slog("app", "%s killed on request", c->app->name);
            app_kill(id);
        }

        // Signed: stop_ms is "k_millis() | 1", which can be 1 ms ahead of the
        // clock. Unsigned, that read as 4 billion ms late, and the first run
        // killed every app the instant it was asked to close.
        if (c->stop_ms && !c->killed &&
            (int32_t)(k_millis() - c->stop_ms) >= APP_STOP_GRACE_MS) {
            slog("app", "%s did not close within %u ms of being asked - killing it",
                 c->app->name, (unsigned)APP_STOP_GRACE_MS);
            app_kill(id);
        }

        switch (k_task_status((uint8_t)c->task, c->task_gen)) {
            case K_TASK_ALIVE:
            case K_TASK_DYING:              // still on some core's stack: next pass
                break;
            case K_TASK_ENDED:
                app_end(c, nullptr);
                break;
            case K_TASK_ZOMBIE: {
                // Its mailbox slots first: until they are clear, core 0 could
                // still write an answer into its stack, so the stack must not
                // go back to the heap - which reaping allows.
#ifndef SMARTOS_TEST_KILL_NOFORGET
                const kmbox_forgot_t f = kmbox_forget((uint8_t)c->task, c->task_gen);
#else
                const kmbox_forgot_t f = {};    // test only: what skipping it costs
#endif
                if (f.running) break;       // mid-job; this cannot happen from the
                                            // main loop, but never reap under one
                k_task_reap((uint8_t)c->task, c->task_gen);
                s_last_forgot = f;
                app_end(c, &f);
                break;
            }
        }
    }
}

uint32_t app_kills(void)          { return s_kills; }
uint32_t app_frames_rescued(void) { return s_rescued; }

void app_last_forgot(uint32_t *claimed, uint32_t *posted, uint32_t *done)
{
    if (claimed) *claimed = s_last_forgot.claimed;
    if (posted)  *posted  = s_last_forgot.posted;
    if (done)    *done    = s_last_forgot.done;
}

void app_focus(int id)
{
    evbus_send(s_mgr_sub, EV_SYS, EV_SRC_SYSTEM, SYS_FOCUS_REQ, (uint32_t)id);
}

// Sent, not published: the manager's mask is EV_INPUT and EV_TIMER only, so a
// published EV_SYS would never reach it. Every request to the manager goes
// this way - see app_focus above.
void app_redraw_focused(void)
{
    evbus_send(s_mgr_sub, EV_SYS, EV_SRC_SYSTEM, SYS_REDRAW, 0);
}

int app_focused(void) { return s_focus; }

const app_ctx_t *app_get(int id)
{
    if (id < 0 || id >= APP_MAX || !s_used[id]) return nullptr;
    return &s_ctx[id];
}

int app_count(void)
{
    int n = 0;
    for (int i = 0; i < APP_MAX; ++i) if (s_used[i]) n++;
    return n;
}

int app_user_count(void)
{
    int n = 0;
    for (int i = 0; i < APP_MAX; ++i) {
        if (s_used[i] && !(s_ctx[i].app->flags & APP_FLAG_SYSTEM)) n++;
    }
    return n;
}

int app_home_id(void) { return s_home; }

int app_running(const app_t *app)
{
    for (int i = 0; i < APP_MAX; ++i) if (s_used[i] && s_ctx[i].app == app) return i;
    return -1;
}

// --- the compositor, core 0 -----------------------------------------------------

// Stage 5.4e - a frame drawn in the dark is not sent.
//
// The backlight goes off after the screenoff seconds, and the apps keep
// running: a scope records, a clock ticks, and each of them goes on asking to
// be drawn. Every one of those frames used to be pushed down the SPI bus to a
// panel nobody can see - a whole one costs 15.4 ms of wire time, a damaged
// region less, but never nothing. Now the frame is taken and
// acknowledged - so the app never stalls waiting for SYS_FRAME_DONE - and
// nothing is sent. What was drawn while dark is still in the framebuffer, and
// the damage tracker has kept it: the first flush after the screen comes back
// sends the whole area, so the panel cannot be left showing a stale frame.
static volatile uint32_t s_dark_frames;     // frames not sent, ever
static volatile uint32_t s_dark_now;        // the panel is behind the buffer
static volatile uint32_t s_sent_px;         // pixels that did go down the wire

bool app_compositor_poll(void)
{
    if (!k_cas(&s_frame, FRAME_READY, FRAME_FLUSHING)) {
        return false;
    }

    if (screen_is_off()) {
        s_dark_frames = s_dark_frames + 1;
        s_dark_now    = 1;
    } else {
        if (s_dark_now) {
            s_dark_now = 0;
            fb_damage_all(&s_view);     // everything drawn in the dark, at once
        }
        s_sent_px = s_sent_px + (uint32_t)fb_damage_pixels(&s_view);
        fb_flush(&s_view);
        s_flushed = s_flushed + 1;
    }

    barrier();
    s_frame = FRAME_IDLE;
    barrier();

    const int f = s_focus;
    if (f != APP_NONE) {
        evbus_send(s_ctx[f].sub, EV_SYS, EV_SRC_SYSTEM, SYS_FRAME_DONE, 0);
    }
    return true;
}

uint32_t app_frames_flushed(void) { return s_flushed; }
uint32_t app_frames_dark(void)    { return s_dark_frames; }
uint32_t app_pixels_sent(void)    { return s_sent_px; }

// --- the manager, core 1 ----------------------------------------------------------

// The next running app after `from`, other than `skip`. APP_NONE if none.
static int next_app(int from, int skip)
{
    for (int k = 1; k <= APP_MAX; ++k) {
        const int i = (from + k + APP_MAX) % APP_MAX;
        if (i != skip && s_used[i]) return i;
    }
    return APP_NONE;
}

// The one place focus changes. The old app hears it has lost focus before
// s_focus moves, so it stops asking to draw; the new app hears it has gained
// it after, so its first draw finds the screen already its own.
static void set_focus(int id)
{
    if (id != APP_NONE && (id < 0 || id >= APP_MAX || !s_used[id])) return;
    const int old = s_focus;
    if (old == id) return;

    if (old != APP_NONE) {
        evbus_send(s_ctx[old].sub, EV_SYS, EV_SRC_SYSTEM, SYS_FOCUS_LOST, 0);
    }
    barrier();
    s_focus = id;
    barrier();
    if (id != APP_NONE) {
        evbus_send(s_ctx[id].sub, EV_SYS, EV_SRC_SYSTEM, SYS_FOCUS_GAINED, 0);
    }
    evbus_publish(EV_SYS, EV_SRC_SYSTEM, SYS_FOCUS_CHANGED, (uint32_t)id);
}

void app_manager_task(void *arg)
{
    (void)arg;
    bool swallow_back = false;

    // Stage 5.5, the minimise chord. A DOWN press is not passed on at once:
    // it waits up to APP_CHORD_MS to see whether BACK follows, which means
    // "put this app in the background". Anything else that arrives first -
    // the DOWN release of a tap, another button - sends the held press on
    // immediately, so only a DOWN that is really held waits at all. A timer
    // runs only while one is held, to end the wait when nothing else comes.
    bool     pend = false, swallow_down = false;
    ev_t     pend_ev = {};
    uint32_t pend_ms = 0;
    int      chord_timer = -1;

    for (;;) {
        ev_t ev;
        evbus_recv(s_mgr_sub, &ev);

        if (ev.type == EV_TIMER) {
            if (pend && k_millis() - pend_ms >= APP_CHORD_MS) {
                pend = false;
                evbus_timer_stop(chord_timer);
                chord_timer = -1;
                const int f = s_focus;
                if (f != APP_NONE) evbus_forward(s_ctx[f].sub, &pend_ev);
            }
            continue;
        }

        if (ev.type == EV_INPUT) {
            // Stage 5.4: with the screen off, the first press only wakes it.
            if (screen_is_off()) {
                screen_wake_k();
                if (ev.a != INPUT_RELEASE) continue;
            }
            const bool back = (ev.b == BTN_INDEX_BACK);
            const bool down = (ev.b == BTN_INDEX_DOWN);

            // The rest of a chord's DOWN - its repeats and its release -
            // belongs to the gesture, not to whatever is focused next.
            if (down && swallow_down) {
                if (ev.a == INPUT_RELEASE) swallow_down = false;
                continue;
            }

            if (pend) {
                if (back && ev.a == INPUT_PRESS) {
                    // The chord. The app keeps running; Home comes to the
                    // front and lists it under "Running".
                    pend = false;
                    evbus_timer_stop(chord_timer);
                    chord_timer  = -1;
                    swallow_down = true;
                    swallow_back = true;          // and its release
                    const int f = s_focus;
                    if (f != APP_NONE && f != s_home && s_home != APP_NONE) {
                        slog_k("app", "%s minimised", s_ctx[f].app->name);
                        set_focus(s_home);
                    }
                    continue;
                }
                // Not the chord: the held press goes on its way first.
                pend = false;
                evbus_timer_stop(chord_timer);
                chord_timer = -1;
                const int f = s_focus;
                if (f != APP_NONE) evbus_forward(s_ctx[f].sub, &pend_ev);
            } else if (down && ev.a == INPUT_PRESS && s_focus != APP_NONE) {
                pend    = true;
                pend_ev = ev;
                pend_ms = k_millis();
                chord_timer = evbus_timer_start(s_mgr_sub, 30, 1);
                continue;
            }

            // BACK held closes the focused app. The first REPEAT is the
            // gesture; the rest of that hold - more repeats, and the release -
            // belongs to it, not to whichever app is focused afterwards. The
            // app exits at its next event, and SYS_APP_EXITED below hands
            // focus back to Home. Home itself is never closed this way.
            if (back && swallow_back) {
                if (ev.a == INPUT_RELEASE) swallow_back = false;
                continue;
            }
            if (back && ev.a == INPUT_REPEAT) {
                swallow_back = true;
                const int f = s_focus;
                if (f != APP_NONE && f != s_home) app_stop(f);
                continue;
            }

            const int f = s_focus;
            if (f != APP_NONE) evbus_forward(s_ctx[f].sub, &ev);
            continue;
        }

        if (ev.type == EV_SYS) {
            if (ev.a == SYS_FOCUS_REQ) {
                const int want = (ev.b == APP_FOCUS_NEXT) ? next_app(s_focus, APP_NONE)
                                                          : (int)ev.b;
                set_focus(want);
            } else if (ev.a == SYS_FOCUS_CLAIM) {
                if (s_focus == APP_NONE) set_focus((int)ev.b);
            } else if (ev.a == SYS_REDRAW) {
                // Stage 6.4d: the panel missed whatever was drawn while it was
                // dark - the compositor threw those frames away - and an app
                // that only draws when something happens has no reason to draw
                // again. Any frame will do: the first flush after the screen
                // comes back sends the whole area anyway. Handled here because
                // `dirty` is core 1's to write, and the screen wakes on core 0.
                if (s_focus != APP_NONE && s_used[s_focus] == SLOT_LIVE) {
                    s_ctx[s_focus].dirty = true;
                }
            } else if (ev.a == SYS_APP_EXITED && s_focus == (int)ev.b) {
                // Back to Home if there is one; otherwise whoever is left.
                const int h = s_home;
                set_focus((h != APP_NONE && h != (int)ev.b && s_used[h])
                              ? h : next_app((int)ev.b, (int)ev.b));
            }
        }
    }
}
