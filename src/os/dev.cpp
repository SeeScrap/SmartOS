#include "os/dev.h"

#include <string.h>

#include "driver/gpio.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "driver/gptimer.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_private/adc_private.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "soc/gpio_reg.h"
#include "soc/soc.h"

#include "board/devkitc_n16r8.h"
#include "kernel/kmbox.h"
#include "kernel/ktime.h"
#include "board/log.h"

enum {
    DEV_JOB_GPIO_MODE = KMBOX_OP_EXT,   // a0 = pin, a1 = dev_mode_t
    DEV_JOB_ADC_READ,                   // a0 = pin -> result = mV
    DEV_JOB_ADC_STREAM,                 // a0 = pin, a1 = period ms (0 = stop), a2 = sub
    DEV_JOB_PULL,                       // a0 = pin, a1 = dev_pull_t
};

// --- shared with core 1 --------------------------------------------------------
// Which pins the device service has made outputs. Two words, not one 64-bit
// value: a 64-bit read is two loads on this core, and could see half an update.
static volatile DRAM_ATTR uint32_t s_out_lo, s_out_hi;
static volatile DRAM_ATTR uint32_t s_samples, s_lost;

// --- core 0 only ------------------------------------------------------------------
//
// HOW A STREAM IS SAMPLED
//
// Until this change a FreeRTOS task took the readings, and it lost them
// whenever core 0 wrote flash: a write turns the cache off and stops every
// task, and one sector erase is ~30 ms - three readings of a 100 Hz stream.
// Measured on the scope test: 0 to 20 of 200 readings lost in 2 s, in step
// with how long flash was busy (23 ms busy: 0 lost; 162 ms busy: 20 lost),
// and 0 of them lost in the app's queue.
//
// Now a hardware timer interrupts every millisecond. Its handler and the ADC
// read it calls are in IRAM, and everything they touch is in internal RAM, so
// it keeps running while the cache is off. It takes the raw reading on time
// and stamps it with its own millisecond count; the stream task later turns
// the raw value into millivolts (calibration is flash code) and sends it on
// with the time it was TAKEN, not the time it was sent.
typedef struct {
    bool                      active;
    int                       pin;
    int                       sub;
    uint32_t                  period_ms;
    uint32_t                  left_ms;   // until the next reading
    uint32_t                  gen;       // bumped on every (re)start
    adc_oneshot_unit_handle_t unit;
    adc_channel_t             ch;
} stream_t;

// One reading, from the interrupt to the stream task.
typedef struct {
    uint8_t  slot;
    uint8_t  pin;
    uint16_t raw;
    uint32_t gen;
    uint32_t t_ms;                        // the timer's count when it was taken
} sample_t;

#define SAMPLE_RING 256                   // 2.5 s of one 100 Hz stream

static DRAM_ATTR stream_t          s_streams[DEV_STREAMS];
static DRAM_ATTR portMUX_TYPE      s_streams_mux = portMUX_INITIALIZER_UNLOCKED;
static DRAM_ATTR sample_t          s_ring[SAMPLE_RING];
static volatile DRAM_ATTR uint32_t s_head, s_tail;   // the ISR writes head, the task tail
static volatile DRAM_ATTR uint32_t s_timer_ms;       // counted by the ISR
static uint32_t                    s_ms_offset;      // k_millis() - s_timer_ms, set per stream start
static SemaphoreHandle_t          s_adc_lock;
static adc_oneshot_unit_handle_t  s_unit[2];
static adc_cali_handle_t          s_cali[PIN_MAX + 1];
static bool                       s_adc_ready[PIN_MAX + 1];

bool dev_pin_free(int pin)
{
    return pin >= 0 && pin <= PIN_MAX && !((PINS_RESERVED >> pin) & 1ULL);
}

bool dev_adc_capable(int pin)
{
    adc_unit_t unit;
    adc_channel_t ch;
    return dev_pin_free(pin) && adc_oneshot_io_to_channel(pin, &unit, &ch) == ESP_OK;
}

// Brings up the unit and the channel on first use. ADC lock held.
static bool adc_prepare(int pin, adc_unit_t *unit_out, adc_channel_t *ch_out)
{
    adc_unit_t unit;
    adc_channel_t ch;
    if (adc_oneshot_io_to_channel(pin, &unit, &ch) != ESP_OK) return false;

    if (!s_unit[unit]) {
        adc_oneshot_unit_init_cfg_t ucfg = {};
        ucfg.unit_id = unit;
        if (adc_oneshot_new_unit(&ucfg, &s_unit[unit]) != ESP_OK) return false;
    }
    if (!s_adc_ready[pin]) {
        adc_oneshot_chan_cfg_t ccfg = {};
        ccfg.atten    = ADC_ATTEN_DB_12;         // the full 0-3.1 V range
        ccfg.bitwidth = ADC_BITWIDTH_DEFAULT;
        if (adc_oneshot_config_channel(s_unit[unit], ch, &ccfg) != ESP_OK) return false;

        // Calibration is per channel on this chip, and optional: without it
        // the reading is scaled from raw, which is a few percent out.
        adc_cali_curve_fitting_config_t cal = {};
        cal.unit_id  = unit;
        cal.chan     = ch;
        cal.atten    = ADC_ATTEN_DB_12;
        cal.bitwidth = ADC_BITWIDTH_DEFAULT;
        if (adc_cali_create_scheme_curve_fitting(&cal, &s_cali[pin]) != ESP_OK) {
            s_cali[pin] = nullptr;
        }
        s_adc_ready[pin] = true;
    }
    *unit_out = unit;
    *ch_out   = ch;
    return true;
}

// Calibration is flash code: task context only, never the interrupt.
static uint32_t raw_to_mv(int pin, int raw)
{
    int v = 0;
    if (s_cali[pin] && adc_cali_raw_to_voltage(s_cali[pin], raw, &v) == ESP_OK) return (uint32_t)v;
    return (uint32_t)raw * 3100u / 4095u;
}

static int adc_read_mv(int pin, uint32_t *mv)
{
    int rc = DEV_ERR_IO;
    xSemaphoreTake(s_adc_lock, portMAX_DELAY);
    adc_unit_t unit;
    adc_channel_t ch;
    int raw = 0;
    if (adc_prepare(pin, &unit, &ch) &&
        adc_oneshot_read(s_unit[unit], ch, &raw) == ESP_OK) {
        *mv = raw_to_mv(pin, raw);
        rc = DEV_OK;
    }
    xSemaphoreGive(s_adc_lock);
    return rc;
}

// --- mailbox handlers: core 0, FreeRTOS context -----------------------------------

static uint32_t h_gpio_mode(const k_job_t *j, uint32_t *status)
{
    const int pin = (int)j->a0;
    if (!dev_pin_free(pin)) { *status = 1; return (uint32_t)DEV_ERR_PIN; }

    gpio_config_t c = {};
    c.pin_bit_mask = 1ULL << pin;
    c.mode         = (j->a1 == DEV_OUT) ? GPIO_MODE_INPUT_OUTPUT : GPIO_MODE_INPUT;
    c.pull_up_en   = (j->a1 == DEV_IN_PULLUP)   ? GPIO_PULLUP_ENABLE   : GPIO_PULLUP_DISABLE;
    c.pull_down_en = (j->a1 == DEV_IN_PULLDOWN) ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE;
    if (gpio_config(&c) != ESP_OK) { *status = 1; return (uint32_t)DEV_ERR_IO; }

    // A pin that is a GPIO now is no longer an ADC input - and the interrupt
    // must stop reading it as one.
    s_adc_ready[pin] = false;
    portENTER_CRITICAL(&s_streams_mux);
    for (int i = 0; i < DEV_STREAMS; ++i) {
        if (s_streams[i].active && s_streams[i].pin == pin) s_streams[i].active = false;
    }
    portEXIT_CRITICAL(&s_streams_mux);

    const uint32_t bit = 1u << (pin & 31);
    volatile uint32_t *mask = (pin < 32) ? &s_out_lo : &s_out_hi;
    *mask = (j->a1 == DEV_OUT) ? (*mask | bit) : (*mask & ~bit);
    return DEV_OK;
}

static uint32_t h_adc_read(const k_job_t *j, uint32_t *status)
{
    uint32_t mv = 0;
    const int pin = (int)j->a0;
    if (!dev_adc_capable(pin)) { *status = 1; return (uint32_t)DEV_ERR_NOADC; }
    if (adc_read_mv(pin, &mv) != DEV_OK) { *status = 1; return (uint32_t)DEV_ERR_IO; }
    return mv;
}

static uint32_t h_adc_stream(const k_job_t *j, uint32_t *status)
{
    const int      pin    = (int)j->a0;
    const uint32_t period = j->a1;
    const int      sub    = (int)j->a2;
    if (!dev_adc_capable(pin)) { *status = 1; return (uint32_t)DEV_ERR_NOADC; }

    // The interrupt cannot set a channel up, so it is brought up here, and a
    // first reading proves it works.
    uint32_t mv;
    adc_unit_t unit = ADC_UNIT_1;
    adc_channel_t ch = ADC_CHANNEL_0;
    if (period) {
        if (adc_read_mv(pin, &mv) != DEV_OK) { *status = 1; return (uint32_t)DEV_ERR_IO; }
        xSemaphoreTake(s_adc_lock, portMAX_DELAY);
        const bool ok = adc_prepare(pin, &unit, &ch);
        xSemaphoreGive(s_adc_lock);
        if (!ok) { *status = 1; return (uint32_t)DEV_ERR_IO; }
    }

    int rc = DEV_ERR_FULL;
    portENTER_CRITICAL(&s_streams_mux);
    // An existing stream for this pin and subscriber is changed in place.
    int slot = -1;
    for (int i = 0; i < DEV_STREAMS; ++i) {
        if (s_streams[i].active && s_streams[i].pin == pin && s_streams[i].sub == sub) {
            slot = i;
            break;
        }
    }
    if (period == 0) {
        if (slot >= 0) s_streams[slot].active = false;
        rc = DEV_OK;
    } else {
        if (slot < 0) {
            for (int i = 0; i < DEV_STREAMS; ++i) if (!s_streams[i].active) { slot = i; break; }
        }
        if (slot >= 0) {
            s_streams[slot].pin       = pin;
            s_streams[slot].sub       = sub;
            s_streams[slot].period_ms = period < DEV_STREAM_MIN_MS ? DEV_STREAM_MIN_MS : period;
            s_streams[slot].left_ms   = 1;                  // the first on the next tick
            s_streams[slot].gen      += 1;                  // readings still queued are stale
            s_streams[slot].unit      = s_unit[unit];
            s_streams[slot].ch        = ch;
            s_streams[slot].active    = true;
            rc = DEV_OK;
            // Readings are stamped by the timer; this puts them on the
            // kernel's clock, the one apps see. Set here, not in dev_init,
            // because the kernel clock only runs once core 1 does.
            s_ms_offset = k_millis() - s_timer_ms;
        }
    }
    portEXIT_CRITICAL(&s_streams_mux);

    if (rc != DEV_OK) *status = 1;
    return (uint32_t)rc;
}

static uint32_t h_pull(const k_job_t *j, uint32_t *status)
{
    const int pin = (int)j->a0;
    if (!dev_pin_free(pin)) { *status = 1; return (uint32_t)DEV_ERR_PIN; }

    // Setting up an ADC channel resets the pad's pulls, so the channel is
    // brought up first and the pull applied after - never the other way round.
    if (dev_adc_capable(pin)) {
        xSemaphoreTake(s_adc_lock, portMAX_DELAY);
        adc_unit_t u;
        adc_channel_t c;
        adc_prepare(pin, &u, &c);
        xSemaphoreGive(s_adc_lock);
    }
    gpio_pull_mode_t m = GPIO_FLOATING;
    if (j->a1 == DEV_PULL_UP)   m = GPIO_PULLUP_ONLY;
    if (j->a1 == DEV_PULL_DOWN) m = GPIO_PULLDOWN_ONLY;
    if (gpio_set_pull_mode((gpio_num_t)pin, m) != ESP_OK) { *status = 1; return (uint32_t)DEV_ERR_IO; }
    return DEV_OK;
}

// Core 0. Since stage 5.2 the app runtime's cleanup runs on core 0 - it has to,
// for an app that was killed - so this is a direct call, not a mailbox job.
void dev_app_release(int sub)
{
    portENTER_CRITICAL(&s_streams_mux);
    for (int i = 0; i < DEV_STREAMS; ++i) {
        if (s_streams[i].active && s_streams[i].sub == sub) s_streams[i].active = false;
    }
    portEXIT_CRITICAL(&s_streams_mux);
}

// --- the sampling interrupt: core 0, IRAM, runs with the cache off -----------------

static IRAM_ATTR bool on_timer(gptimer_handle_t, const gptimer_alarm_event_data_t *, void *)
{
    const uint32_t now = s_timer_ms + 1;
    s_timer_ms = now;

    portENTER_CRITICAL_ISR(&s_streams_mux);
    for (int i = 0; i < DEV_STREAMS; ++i) {
        stream_t *s = &s_streams[i];
        if (!s->active || --s->left_ms) continue;
        s->left_ms = s->period_ms;

        int raw = 0;
        const uint32_t next = (s_head + 1) % SAMPLE_RING;
        if (next == s_tail || adc_oneshot_read_isr(s->unit, s->ch, &raw) != ESP_OK) {
            s_lost = s_lost + 1;             // the task is that far behind, or the read failed
            continue;
        }
        sample_t *o = &s_ring[s_head];
        o->slot = (uint8_t)i;
        o->pin  = (uint8_t)s->pin;
        o->raw  = (uint16_t)raw;
        o->gen  = s->gen;
        o->t_ms = now;
        s_head  = next;
    }
    portEXIT_CRITICAL_ISR(&s_streams_mux);
    return false;
}

// --- the stream task: core 0 ------------------------------------------------------

static void stream_task(void *arg)
{
    (void)arg;
    for (;;) {
        while (s_tail != s_head) {
            const sample_t r = s_ring[s_tail];
            s_tail = (s_tail + 1) % SAMPLE_RING;

            // Still the stream that took it? A stream stopped or restarted
            // since then must not receive readings from before.
            portENTER_CRITICAL(&s_streams_mux);
            const stream_t s = s_streams[r.slot];
            portEXIT_CRITICAL(&s_streams_mux);
            if (!s.active || s.gen != r.gen) continue;

            evbus_send_at(s.sub, EV_SENSOR, EV_SRC_DEV, r.pin, raw_to_mv(r.pin, r.raw),
                          r.t_ms + s_ms_offset);
            s_samples = s_samples + 1;
        }
        vTaskDelay(1);
    }
}

void dev_init(void)
{
    s_adc_lock = xSemaphoreCreateMutex();
    kmbox_register(DEV_JOB_GPIO_MODE,  h_gpio_mode);
    kmbox_register(DEV_JOB_ADC_READ,   h_adc_read);
    kmbox_register(DEV_JOB_ADC_STREAM, h_adc_stream);
    kmbox_register(DEV_JOB_PULL,       h_pull);
    xTaskCreatePinnedToCore(stream_task, "devsvc", 4096, nullptr, 5, nullptr, 0);

    // 1 MHz count, an alarm every 1000: one interrupt a millisecond. Allocated
    // from core 0, so the interrupt is core 0's.
    gptimer_handle_t t = nullptr;
    gptimer_config_t tc = {};
    tc.clk_src       = GPTIMER_CLK_SRC_DEFAULT;
    tc.direction     = GPTIMER_COUNT_UP;
    tc.resolution_hz = 1000000;
    gptimer_alarm_config_t ac = {};
    ac.alarm_count                = 1000;
    ac.reload_count               = 0;
    ac.flags.auto_reload_on_alarm = true;
    gptimer_event_callbacks_t cb = {};
    cb.on_alarm = on_timer;
    if (gptimer_new_timer(&tc, &t) != ESP_OK ||
        gptimer_register_event_callbacks(t, &cb, nullptr) != ESP_OK ||
        gptimer_set_alarm_action(t, &ac) != ESP_OK ||
        gptimer_enable(t) != ESP_OK || gptimer_start(t) != ESP_OK) {
        logln("[dev] !! sampling timer failed to start - ADC streams will be silent");
    }
}

uint32_t dev_samples_sent(void) { return s_samples; }
uint32_t dev_samples_lost(void) { return s_lost; }

// --- the app side: core 1 ---------------------------------------------------------

static int call(uint32_t op, uint32_t a0, uint32_t a1, uint32_t a2)
{
    uint32_t r = 0;
    if (!kmbox_call(op, a0, a1, a2, &r)) {
        // A refusal comes back as a negative code in the result; a timeout or
        // a full mailbox leaves it 0.
        return ((int32_t)r < 0) ? (int32_t)r : DEV_ERR_IO;
    }
    return (int32_t)r;
}

int dev_gpio_mode(int pin, dev_mode_t mode)
{
    if (!dev_pin_free(pin)) return DEV_ERR_PIN;
    return call(DEV_JOB_GPIO_MODE, (uint32_t)pin, (uint32_t)mode, 0);
}

int dev_gpio_read(int pin)
{
    if (!dev_pin_free(pin)) return DEV_ERR_PIN;
    const uint32_t in = REG_READ(pin < 32 ? GPIO_IN_REG : GPIO_IN1_REG);
    return (int)((in >> (pin & 31)) & 1u);
}

int dev_gpio_write(int pin, int level)
{
    if (!dev_pin_free(pin)) return DEV_ERR_PIN;
    const uint32_t bit  = 1u << (pin & 31);
    const uint32_t outs = (pin < 32) ? s_out_lo : s_out_hi;
    if (!(outs & bit)) return DEV_ERR_MODE;
    if (pin < 32) REG_WRITE(level ? GPIO_OUT_W1TS_REG  : GPIO_OUT_W1TC_REG,  bit);
    else          REG_WRITE(level ? GPIO_OUT1_W1TS_REG : GPIO_OUT1_W1TC_REG, bit);
    return DEV_OK;
}

int dev_adc_read(int pin, uint32_t *mv)
{
    if (!dev_pin_free(pin)) return DEV_ERR_PIN;
    const int r = call(DEV_JOB_ADC_READ, (uint32_t)pin, 0, 0);
    if (r < 0) return r;
    if (mv) *mv = (uint32_t)r;
    return DEV_OK;
}

int dev_adc_stream(app_ctx_t *c, int pin, uint32_t period_ms)
{
    if (!c || !dev_pin_free(pin)) return DEV_ERR_PIN;
    return call(DEV_JOB_ADC_STREAM, (uint32_t)pin, period_ms, (uint32_t)c->sub);
}

int dev_pin_pull(int pin, dev_pull_t pull)
{
    if (!dev_pin_free(pin)) return DEV_ERR_PIN;
    return call(DEV_JOB_PULL, (uint32_t)pin, (uint32_t)pull, 0);
}

