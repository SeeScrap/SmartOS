#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "smartos_config.h"

#include "os/app.h"

// ---------------------------------------------------------------------------
// Stage 4.3c - hardware for apps.
//
// Apps run on core 1, and core 1 cannot call IDF's drivers: they are built on
// FreeRTOS mutexes and queues, and core 1 has no FreeRTOS task to own one. So
// hardware is a service. The drivers live on core 0, in the device service;
// apps ask for what they want through the mailbox, and readings that arrive on
// a schedule come back as EV_SENSOR events on the bus.
//
// Two things stay on core 1 because they are single register accesses and
// safe from anywhere: reading a GPIO, and driving one the app has configured
// as an output.
//
// PINS
//
// Everything the system itself uses - the LCD, touch, SD, buttons, flash and
// PSRAM, native USB, UART0, the strapping pins - is refused (PINS_RESERVED in
// board/pins.h). What is left for apps: 1, 2, 17, 18, 38-42, 48. The ADC is on
// 1 and 2 (ADC1) and 17 and 18 (ADC2).
//
// WHAT IS NOT HERE YET
//
// I2C and SPI sensors, and pulse counting. The shape is the same - a handler
// on core 0, a call on core 1 - and they slot in as further DEV_JOB_ ops.
// ---------------------------------------------------------------------------

typedef enum {
    DEV_IN = 0,
    DEV_IN_PULLUP,
    DEV_IN_PULLDOWN,
    DEV_OUT,
} dev_mode_t;

typedef enum {
    DEV_PULL_NONE = 0,
    DEV_PULL_UP,
    DEV_PULL_DOWN,
} dev_pull_t;

#define DEV_OK          0
#define DEV_ERR_PIN    -1   // reserved for the system, or no such pin
#define DEV_ERR_NOADC  -2   // the pin has no ADC channel
#define DEV_ERR_FULL   -3   // no free stream slot
#define DEV_ERR_IO     -4   // the driver failed, or core 0 did not answer
#define DEV_ERR_MODE   -5   // write to a pin that is not an output


// --- for apps: kernel tasks on core 1 -------------------------------------------
int  dev_gpio_mode(int pin, dev_mode_t mode);
int  dev_gpio_read(int pin);                     // 0 or 1, or DEV_ERR_PIN
int  dev_gpio_write(int pin, int level);         // the pin must be DEV_OUT
int  dev_adc_read(int pin, uint32_t *mv);        // one reading, calibrated to mV

// Readings every period_ms as EV_SENSOR to this app (a = pin, b = mV). A
// period of 0 stops that pin's stream. Streams end by themselves when the app
// exits. At the fastest rate, a reading every 2 ms, an app that stops reading
// its queue for 32 ms loses readings - the bus drops, it never blocks.
int  dev_adc_stream(app_ctx_t *c, int pin, uint32_t period_ms);

// Pull resistors on an ADC pin. With nothing wired to it, a pull-up reads near
// the top of the range and a pull-down near 0 V - enough to prove the whole
// path from the pin to an app without any hardware.
int  dev_pin_pull(int pin, dev_pull_t pull);

// --- anyone ------------------------------------------------------------------------
bool dev_pin_free(int pin);                      // exists and is not reserved
bool dev_adc_capable(int pin);

// --- system side ------------------------------------------------------------------
void dev_init(void);                             // core 0, before core 1 starts
void dev_app_release(int sub);                   // core 0: the app runtime, when an app ends
uint32_t dev_samples_sent(void);
uint32_t dev_samples_lost(void);       // readings the sampling interrupt could not keep
