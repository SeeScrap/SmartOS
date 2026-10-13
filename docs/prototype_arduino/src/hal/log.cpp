#include "hal/log.h"

#include <stdarg.h>
#include <string.h>

#define LOG_BAUD 115200

void log_init(void)
{
    Serial.begin(LOG_BAUD);
#if ARDUINO_USB_CDC_ON_BOOT
    // With CDC on boot, `Serial` is the native USB port and `Serial0` is UART0.
    Serial0.begin(LOG_BAUD);
#endif
}

static void log_write(const char *s, size_t n)
{
    Serial.write((const uint8_t *)s, n);
#if ARDUINO_USB_CDC_ON_BOOT
    Serial0.write((const uint8_t *)s, n);
#endif
}

void logf(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    if (n >= (int)sizeof(buf)) n = sizeof(buf) - 1;
    log_write(buf, (size_t)n);
}

void logln(const char *s)
{
    if (s && *s) log_write(s, strlen(s));
    log_write("\r\n", 2);
}
