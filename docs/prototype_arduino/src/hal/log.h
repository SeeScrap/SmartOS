#pragma once
#include <Arduino.h>

// The DevKitC-1 has two USB sockets and third-party S3 boards often have a
// USB-UART chip instead of the native port, so "which port does Serial come
// out of" depends on hardware we cannot see from here. Rather than guess,
// every log line goes to BOTH the native USB CDC and UART0 (GPIO43/44).
// Whichever cable is plugged in, the output shows up.
void log_init(void);
void logf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void logln(const char *s = "");
