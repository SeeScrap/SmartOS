#include "tests/harness.h"

#include <stdio.h>
#include <string.h>

#include "esp_timer.h"

#include "os/evbus.h"
#include "os/syslog.h"
#include "port/kport.h"
#include "board/input.h"
#include "board/log.h"
#include "system.h"

void sdk_case(const char *what, bool ok, const char *detail)
{
    logf("  %-28s %-46s %s\n", what, detail, ok ? "OK" : "FAIL");
}

void app_case(const char *what, bool ok, const char *detail)
{
    logf("  %-30s %-44s %s\n", what, detail, ok ? "OK" : "FAIL");
}

void pump(uint32_t ms)
{
    const int64_t end = esp_timer_get_time() + (int64_t)ms * 1000;
    while (esp_timer_get_time() < end) {
        system_pass();
        kport_yield_ms(1);
    }
}

void press(uint32_t button, uint8_t kind)
{
    evbus_publish(EV_INPUT, EV_SRC_TEST, kind, button);
}

void tap(uint32_t button)
{
    press(button, INPUT_PRESS);
    press(button, INPUT_RELEASE);
}

int wait_gone(int id, uint32_t max_ms)
{
    const int64_t t0 = esp_timer_get_time();
    while (app_get(id)) {
        if (esp_timer_get_time() - t0 > (int64_t)max_ms * 1000) return -1;
        system_pass();
        kport_yield_ms(1);
    }
    return (int)((esp_timer_get_time() - t0) / 1000);
}

int wait_for(bool (*cond)(void *), void *ctx, uint32_t max_ms)
{
    const int64_t t0 = esp_timer_get_time();
    while (!cond(ctx)) {
        if (esp_timer_get_time() - t0 > (int64_t)max_ms * 1000) return -1;
        system_pass();
        kport_yield_ms(1);
    }
    return (int)((esp_timer_get_time() - t0) / 1000);
}

int count_colour(const fb_t *fb, uint16_t colour)
{
    int n = 0;
    for (size_t i = 0; i < (size_t)fb->w * fb->h; ++i) if (fb->px[i] == colour) n++;
    return n;
}

bool file_has_raw(const char *path, const char *needle)
{
    FILE *f = fopen(path, "r");
    if (!f) return false;
    char ln[SYSLOG_LINE + 40];
    bool found = false;
    while (!found && fgets(ln, sizeof(ln), f)) found = strstr(ln, needle) != nullptr;
    fclose(f);
    return found;
}

bool file_has(const char *path, const char *needle)
{
    slog_flush();                   // lines still in RAM count too
    return file_has_raw(path, needle);
}

// Anywhere in the system log. A line can be in log.1.txt by the time it is
// looked for: a run that starts with the log just under SYSLOG_MAX rotates
// within its first few lines (seen: two runs of six failed "apps in the log",
// both starting at 31930 bytes).
bool log_has(const char *needle)
{
    return file_has("/fs/sys/log.txt", needle) || file_has_raw("/fs/sys/log.1.txt", needle);
}
