#pragma once
#include <stdint.h>

#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// Time and delay, kept behind our own names so that stage 2.4 can swap the
// implementation for the kernel's own tick without touching any caller.
//
// hal_delay_ms still goes through FreeRTOS today, which is exactly what we
// want while our code runs on core 0: it hands the CPU back instead of
// spinning. hal_delay_us busy-waits, because sub-tick delays cannot yield.

static inline void     hal_delay_ms(uint32_t ms) { vTaskDelay(pdMS_TO_TICKS(ms)); }
static inline void     hal_delay_us(uint32_t us) { esp_rom_delay_us(us); }
static inline int64_t  hal_micros(void)          { return esp_timer_get_time(); }
static inline uint32_t hal_millis(void)          { return (uint32_t)(esp_timer_get_time() / 1000); }
