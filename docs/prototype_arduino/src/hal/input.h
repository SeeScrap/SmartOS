#pragma once
#include <stdint.h>

// Buttons are wired GPIO -> switch -> GND and read with the internal pull-up,
// so a pressed key reads LOW. Everything above this layer sees the bitmask.
enum {
    BTN_UP    = 1u << 0,
    BTN_DOWN  = 1u << 1,
    BTN_LEFT  = 1u << 2,
    BTN_RIGHT = 1u << 3,
    BTN_OK    = 1u << 4,
    BTN_BACK  = 1u << 5,
};
#define BTN_COUNT 6

void        input_init(void);

// Call often. Debounces, then updates the held/pressed/released views.
void        input_poll(void);

uint32_t    input_held(void);        // currently down
uint32_t    input_pressed(void);     // went down since the last poll
uint32_t    input_released(void);    // came up since the last poll

const char *input_name(uint32_t single_bit);
