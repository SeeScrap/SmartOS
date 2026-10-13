#pragma once
#include <stdbool.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// Stage 3.6 - buttons as an event stream.
//
// Buttons are wired GPIO -> switch -> GND and read with the internal pull-up,
// so a pressed key reads LOW.
//
// WHY EVENTS AND NOT A BITMASK
//
// The previous API was input_poll() plus held/pressed/released masks, sampled
// from core 0's main loop. It had two faults a UI would hit. A tap shorter than
// the gap between polls vanished, because a mask only says what is true NOW.
// And every consumer had to poll at the right rate or miss edges. An event
// stream fixes both: every accepted edge becomes a record, in order, with the
// time it happened, and it waits in a queue until someone takes it.
//
// WHO RUNS WHAT
//
// The sampler is a kernel task pinned to core 1, so it samples on the
// kernel's own tick at a fixed rate whatever core 0 is doing. That also means
// everything it touches lives in IRAM/DRAM - core 0 turns the flash cache off
// whenever it writes flash - which is why it reads GPIO_IN_REG directly rather
// than calling gpio_get_level(), which lives in flash.
//
// The consumers are whoever subscribes to EV_INPUT on the event bus. Today
// that is core 0's main loop, because drawing needs the SPI driver and the SPI
// driver is FreeRTOS code.
//
// SINCE STAGE 4.2 THE EVENTS GO ON THE BUS
//
// The driver used to own a queue with exactly one reader. It now publishes
// EV_INPUT on the system event bus (os/evbus.h) - a = input_kind_t, b = the
// button index, t_ms = when the edge was accepted - and anyone who wants
// buttons subscribes. Dropping, sequence numbers and gap detection moved to
// the bus, per subscriber, so a slow reader loses only its own events.
// ---------------------------------------------------------------------------

enum {
    BTN_UP    = 1u << 0,
    BTN_DOWN  = 1u << 1,
    BTN_LEFT  = 1u << 2,
    BTN_RIGHT = 1u << 3,
    BTN_OK    = 1u << 4,
    BTN_BACK  = 1u << 5,
};
#define BTN_COUNT 6

typedef enum {
    INPUT_PRESS   = 1,
    INPUT_RELEASE = 2,
    INPUT_REPEAT  = 3,      // held: first after INPUT_REPEAT_DELAY_MS, then every INPUT_REPEAT_EVERY_MS
} input_kind_t;

#define INPUT_SAMPLE_MS        2
#define INPUT_DEBOUNCE_MS     12    // a level must hold this long to count
#define INPUT_REPEAT_DELAY_MS 400
#define INPUT_REPEAT_EVERY_MS 120

// Core 0, once, before core 1 is started and after evbus_init(): the pull-ups.
void        input_init(void);

// The sampler. Create it as a kernel task pinned to core 1. Never returns.
// A button already held when it starts becomes the baseline, not a press.
void        input_task(void *arg);

uint32_t    input_held(void);       // debounced state, as a BTN_* mask
uint32_t    input_raw(void);        // undebounced, read now - for wiring checks

// Passes the sampler has made. The liveness measure for it - NOT its task
// switch count, which stops moving once it is the only task on core 1: a task
// that wakes from a sleep with nothing else to run resumes where it is, and no
// context switch is counted. A heartbeat built on switches read that as a hang.
uint32_t    input_samples(void);

const char *input_name(unsigned button);
const char *input_kind_name(uint8_t kind);

// --- for tests --------------------------------------------------------------
// input_step() is exactly one sample of the sampler's loop, with the time
// passed in rather than read, so a script can feed it bounces and holds in
// microseconds of real time. input_reset() forgets the debouncer's state.
// Neither may be called while input_task is running.
void        input_step(uint32_t down_mask, uint32_t now_ms);
void        input_reset(void);
