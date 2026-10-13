#pragma once
#include "driver/gpio.h"

// ---------------------------------------------------------------------------
// The OS's own GPIO layer.
//
// This is not just an Arduino replacement. Once the kernel owns core 1 it will
// need to touch pins from its own interrupt handlers, where framework helpers
// are not safe to call - so the pin API has to be ours from here on.
// ---------------------------------------------------------------------------

static inline void gpio_out(int pin)
{
    gpio_config_t c = {};
    c.pin_bit_mask = 1ULL << pin;
    c.mode         = GPIO_MODE_OUTPUT;
    gpio_config(&c);
}

static inline void gpio_in(int pin)
{
    gpio_config_t c = {};
    c.pin_bit_mask = 1ULL << pin;
    c.mode         = GPIO_MODE_INPUT;
    gpio_config(&c);
}

static inline void gpio_in_pullup(int pin)
{
    gpio_config_t c = {};
    c.pin_bit_mask = 1ULL << pin;
    c.mode         = GPIO_MODE_INPUT;
    c.pull_up_en   = GPIO_PULLUP_ENABLE;
    gpio_config(&c);
}

static inline void gpio_hi(int pin)           { gpio_set_level((gpio_num_t)pin, 1); }
static inline void gpio_lo(int pin)           { gpio_set_level((gpio_num_t)pin, 0); }
static inline void gpio_set(int pin, int lvl) { gpio_set_level((gpio_num_t)pin, lvl ? 1 : 0); }
static inline int  gpio_rd(int pin)           { return gpio_get_level((gpio_num_t)pin); }
