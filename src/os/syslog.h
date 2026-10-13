#pragma once
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include "os/app.h"

// ---------------------------------------------------------------------------
// Stage 4.5 - the system log, kept on flash.
//
// The serial console forgets everything the moment nobody is watching it, and
// a crash overnight leaves nothing behind. So the events that matter - each
// boot and why the last one ended, core 1 faults, apps opened and closed - are
// also written to /fs/sys/log.txt, one line each:
//
//     #13    0.512 sys   boot, last reset: panic
//     #13   41.207 app   snake opened (id 1)
//
// the boot number, seconds since boot, who wrote it, and what happened.
//
// ROTATION
//
// At SYSLOG_MAX bytes the file becomes log.1.txt - replacing the previous one -
// and a new log.txt starts. The log never holds more than about twice that.
//
// TWO WAYS IN
//
// slog() is for system code on core 0, in a FreeRTOS task: it takes the line directly.
// Anything else - a kernel task, an app, either core - uses slog_k() or
// app_log(), which hand the line to core 0 through the mailbox, because the
// filesystem is only reachable from there.
//
// A CEILING FOR APPS
//
// Lines are buffered and written in batches (see syslog.cpp), but an app
// logging in a loop would still wear the flash and fill the log with noise, so
// lines from kernel tasks and apps share a budget: SYSLOG_BURST at once,
// refilled at SYSLOG_RATE a second. Past it they are dropped and counted, and
// the next line that gets through says how many went missing. System code on core 0 is trusted and not limited.
// ---------------------------------------------------------------------------

#define SYSLOG_MAX     (32 * 1024)
#define SYSLOG_LINE    120
#define SYSLOG_RATE    20              // lines a second, sustained
#define SYSLOG_BURST   40
#define SYSLOG_BUF     2048            // lines wait in RAM, at most this many bytes
#define SYSLOG_FLUSH_MS 2000           // ...for at most this long (see syslog.cpp)

// --- core 0, FreeRTOS context ------------------------------------------------------
void     slog_init(uint32_t boot);     // after fs_init; boot is this boot's number
void     slog(const char *tag, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
uint32_t slog_dropped(void);           // lines refused by the rate limit, ever
void     slog_flush(void);             // everything buffered, to flash, now
uint32_t slog_writes(void);           // batches written to flash, ever
// Batches the filesystem would not take - a full disk, or no mount - and the
// bytes in them (stage 5.4d). Both stay at zero on a healthy board.
uint32_t slog_lost_batches(void);
uint32_t slog_lost_bytes(void);
void     slog_poll(void);              // core 0's loop: flushes lines that are due
size_t   slog_size(void);              // bytes in log.txt, waiting lines included

// --- kernel tasks and apps, either core ------------------------------------------------
void slog_k(const char *tag, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void app_log(app_ctx_t *c, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

// The last `max` bytes of the log, starting at a line boundary, NUL-terminated.
// Returns the byte count. Kernel tasks and apps only - it goes through the
// mailbox like the writes do.
size_t slog_tail(char *buf, size_t max);
size_t slog_tail_local(char *buf, size_t max);   // the same, for core 0's own code

// Empties the log (both files). Returns 0 on success.
int    slog_clear(void);
