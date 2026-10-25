#include "s3hal/log.h"

#include <stdarg.h>
#include <stdio.h>

#include "kernel/kspin.h"

// The one console, shared by both cores.
extern "C" {
    k_spin_t          k_console_lock = 0;
    k_spin_t          k_sched_lock   = 0;
    volatile uint32_t k_spin_stuck   = 0;
}

void log_init(void)
{
    // Unbuffered: when the kernel crashes we want the last line to have
    // actually left the chip, not to be sitting in a stdio buffer.
    setvbuf(stdout, NULL, _IONBF, 0);
}

void logf(const char *fmt, ...)
{
    bool locked = k_spin_lock(&k_console_lock);
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    if (locked) k_spin_unlock(&k_console_lock);
}

void logln(const char *s)
{
    if (s && *s) fputs(s, stdout);
    fputs("\n", stdout);
}
