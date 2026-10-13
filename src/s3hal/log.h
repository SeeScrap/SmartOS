#pragma once

// Console output. Under ESP-IDF this is just stdio - the console peripheral is
// picked in sdkconfig (USB Serial/JTAG for this board), so no port juggling is
// needed the way it was on the Arduino core.
void log_init(void);
void logf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void logln(const char *s = "");
