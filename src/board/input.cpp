#include "board/input.h"

#include "esp_attr.h"
#include "soc/gpio_reg.h"
#include "soc/soc.h"

#include "board/devkitc_n16r8.h"
#include "os/evbus.h"
#include "kernel/ktime.h"
#include "board/gpio.h"

// Everything below that the sampler touches is DRAM or IRAM - see input.h.
// Note the const table: without DRAM_ATTR it would be .rodata, which is flash
// behind the same cache, and just as unreachable with the cache off.
static DRAM_ATTR const uint8_t s_pins[BTN_COUNT] = {
    PIN_BTN_UP, PIN_BTN_DOWN, PIN_BTN_LEFT, PIN_BTN_RIGHT, PIN_BTN_OK, PIN_BTN_BACK
};

static const char *s_names[BTN_COUNT] = {        // core 0 only
    "UP", "DOWN", "LEFT", "RIGHT", "OK", "BACK"
};

static volatile DRAM_ATTR uint32_t s_held;       // debounced, BTN_* mask
static volatile DRAM_ATTR uint32_t s_samples;

// Per button, not global. The first version of this driver ran one settle
// timer for all six, so a single bouncing contact held back every other
// button's edges until it had settled.
static DRAM_ATTR bool     s_diff[BTN_COUNT];         // raw disagrees with s_held
static DRAM_ATTR uint32_t s_diff_since[BTN_COUNT];   // ...since this time
static DRAM_ATTR uint32_t s_next_repeat[BTN_COUNT];

// Pins 4-7, 15 and 16 are all below 32, so one register holds every button.
// Pull-ups: a pressed button reads 0.
static IRAM_ATTR uint32_t read_down_mask(void)
{
    uint32_t in = REG_READ(GPIO_IN_REG);
    uint32_t down = 0;
    for (int b = 0; b < BTN_COUNT; ++b) {
        if (!(in & (1u << s_pins[b]))) down |= (1u << b);
    }
    return down;
}

// Stamped with the time the edge was accepted, not the time of publishing -
// they are the same today, but only one of them is the truth.
static IRAM_ATTR void emit(uint8_t kind, uint8_t button, uint32_t now)
{
    evbus_publish_at(EV_INPUT, EV_SRC_INPUT, kind, button, now);
}

IRAM_ATTR void input_step(uint32_t down_mask, uint32_t now)
{
    uint32_t held = s_held;

    for (int b = 0; b < BTN_COUNT; ++b) {
        const uint32_t bit  = 1u << b;
        const bool     down = (down_mask & bit) != 0;
        const bool     was  = (held & bit) != 0;

        if (down != was) {
            // Disagreeing with the accepted state. Only a disagreement that
            // lasts INPUT_DEBOUNCE_MS without a break becomes an edge; any
            // sample that agrees again (below) starts the wait over.
            if (!s_diff[b]) {
                s_diff[b]       = true;
                s_diff_since[b] = now;
            } else if (now - s_diff_since[b] >= INPUT_DEBOUNCE_MS) {
                s_diff[b] = false;
                held ^= bit;
                emit(down ? INPUT_PRESS : INPUT_RELEASE, (uint8_t)b, now);
                if (down) s_next_repeat[b] = now + INPUT_REPEAT_DELAY_MS;
            }
        } else {
            s_diff[b] = false;
            // Scheduled from now rather than from the last repeat: if the task
            // was held off for a while, it resumes the rhythm instead of
            // firing a burst to catch up.
            if (down && (int32_t)(now - s_next_repeat[b]) >= 0) {
                emit(INPUT_REPEAT, (uint8_t)b, now);
                s_next_repeat[b] = now + INPUT_REPEAT_EVERY_MS;
            }
        }
    }

    s_held = held;
}

IRAM_ATTR void input_task(void *arg)
{
    (void)arg;

    // Whatever is held at start is where we begin, not something that just
    // happened. Reporting it as a press would invent an edge nobody made.
    s_held = read_down_mask();

    for (;;) {
        input_step(read_down_mask(), k_millis());
        s_samples = s_samples + 1;
        k_sleep_ms(INPUT_SAMPLE_MS);
    }
}

void input_reset(void)
{
    s_held = 0;
    for (int b = 0; b < BTN_COUNT; ++b) {
        s_diff[b]        = false;
        s_diff_since[b]  = 0;
        s_next_repeat[b] = 0;
    }
}

void input_init(void)
{
    for (int i = 0; i < BTN_COUNT; ++i) gpio_in_pullup(s_pins[i]);
    input_reset();
}

uint32_t input_held(void)              { return s_held; }
uint32_t input_raw(void)               { return read_down_mask(); }
uint32_t input_samples(void)           { return s_samples; }

const char *input_name(unsigned button)
{
    return button < BTN_COUNT ? s_names[button] : "?";
}

const char *input_kind_name(uint8_t kind)
{
    switch (kind) {
        case INPUT_PRESS:   return "PRESS";
        case INPUT_RELEASE: return "RELEASE";
        case INPUT_REPEAT:  return "REPEAT";
        default:            return "?";
    }
}
