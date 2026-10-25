#include "board/log.h"

#include <stdarg.h>
#include <stdio.h>

#include "port/kport.h"

void log_init(void)
{
    // Unbuffered: when the kernel crashes we want the last line to have
    // actually left the chip, not to be sitting in a stdio buffer.
    setvbuf(stdout, NULL, _IONBF, 0);
}

// logf and logln are kport_printf and kport_println. The names and the
// header stay because a few hundred call sites use them; the bodies moved to
// port/kport.cpp in R4b, together with the console lock they take.
void logf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    kport_vprintf(fmt, ap);
    va_end(ap);
}

void logln(const char *s) { kport_println(s); }
