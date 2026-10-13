#include "hal/input.h"

#include <Arduino.h>

#include "board/pins.h"

#define DEBOUNCE_MS 12

static const uint8_t s_pins[BTN_COUNT] = {
    PIN_BTN_UP, PIN_BTN_DOWN, PIN_BTN_LEFT, PIN_BTN_RIGHT, PIN_BTN_OK, PIN_BTN_BACK
};
static const char *s_names[BTN_COUNT] = {
    "UP", "DOWN", "LEFT", "RIGHT", "OK", "BACK"
};

static uint32_t s_held = 0, s_pressed = 0, s_released = 0;
static uint32_t s_raw_last = 0;
static uint32_t s_raw_since = 0;

void input_init(void)
{
    for (int i = 0; i < BTN_COUNT; ++i) pinMode(s_pins[i], INPUT_PULLUP);
    s_held = s_pressed = s_released = 0;
    s_raw_last  = 0;
    s_raw_since = millis();
}

void input_poll(void)
{
    uint32_t raw = 0;
    for (int i = 0; i < BTN_COUNT; ++i) {
        if (digitalRead(s_pins[i]) == LOW) raw |= (1u << i);
    }

    uint32_t now = millis();
    if (raw != s_raw_last) {
        // Still bouncing - restart the settle timer and report no change yet.
        s_raw_last  = raw;
        s_raw_since = now;
        s_pressed = s_released = 0;
        return;
    }

    if (now - s_raw_since < DEBOUNCE_MS || raw == s_held) {
        s_pressed = s_released = 0;
        return;
    }

    s_pressed  = raw & ~s_held;
    s_released = s_held & ~raw;
    s_held     = raw;
}

uint32_t input_held(void)     { return s_held; }
uint32_t input_pressed(void)  { return s_pressed; }
uint32_t input_released(void) { return s_released; }

const char *input_name(uint32_t single_bit)
{
    for (int i = 0; i < BTN_COUNT; ++i) {
        if (single_bit == (1u << i)) return s_names[i];
    }
    return "?";
}
