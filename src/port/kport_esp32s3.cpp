// The ESP32-S3 side of the port seam. See port/kport.h for what it is for.

#include "port/kport.h"

#include <stdarg.h>
#include <stdio.h>

#include "kernel/kspin.h"

// The locks the kernel declares and the chip owns.
//
// k_console_lock is held by both cores - by kport_printf here and by klog() on
// core 1 - which is why it lives on this side of the seam rather than in any
// one core's code. The very first bring-up faulted inside the ROM printf with
// both cores in it, before this existed.
//
// k_sched_lock and k_spin_stuck are the scheduler's, and are defined beside it
// because all three were defined together in s3hal/log.cpp until R4b and there
// is no reason to scatter them now.
extern "C" {
    k_spin_t          k_console_lock = 0;
    k_spin_t          k_sched_lock   = 0;
    volatile uint32_t k_spin_stuck   = 0;
}

extern "C" void kport_vprintf(const char *fmt, va_list ap)
{
    const bool locked = k_spin_lock(&k_console_lock);
    vprintf(fmt, ap);
    if (locked) k_spin_unlock(&k_console_lock);
}

extern "C" void kport_printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    kport_vprintf(fmt, ap);
    va_end(ap);
}

extern "C" void kport_println(const char *s)
{
    if (s && *s) fputs(s, stdout);
    fputs("\n", stdout);
}
