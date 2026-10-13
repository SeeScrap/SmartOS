#include "os/syslog.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_timer.h"

#include "kernel/kmbox.h"
#include "os/fs.h"
#include "os/kfmt.h"
#include "s3hal/log.h"

#define LOG_PATH   "/fs/sys/log.txt"
#define LOG_OLD    "/fs/sys/log.1.txt"

enum { SLOG_JOB = KMBOX_OP_EXT + 12 };   // fs has +8
enum { OP_LINE = 1, OP_TAIL, OP_CLEAR };

// On the caller's stack for one call, like fs_req_t - and for the same reason
// the call has no timeout.
typedef struct {
    uint32_t op;
    char     tag[8];
    char     text[SYSLOG_LINE];
    char    *buf;
    uint32_t len;
    uint32_t out;
} slog_req_t;

// Core 0 only.
static bool     s_ready;
static uint32_t s_boot;
static size_t   s_size;
static uint32_t s_dropped, s_reported;
static int32_t  s_tokens = SYSLOG_BURST;
static int64_t  s_refill_us;

// Lines waiting to be written. See "why lines are buffered" below.
static char     s_buf[SYSLOG_BUF];
static size_t   s_len;
static int64_t  s_first_us;          // when the oldest waiting line arrived
static uint32_t s_writes;

// --- core 0 -----------------------------------------------------------------------------

// WHY LINES ARE BUFFERED
//
// The first version opened, appended to and closed the file for every line.
// Measured on hardware, that cost 52 ms a line, 46 ms of it with core 1
// parked: each close commits LittleFS's copy-on-write of the file's last
// block, so every line meant rewriting - often erasing - a block. At the 20
// lines a second apps are allowed, core 1 would have been parked most of
// every second.
//
// So lines wait in RAM and go out together: once SYSLOG_FLUSH_MS has passed
// since the oldest of them, when the buffer would overflow, and at once for
// the lines that matter most if the system is about to die - "sys" and
// "fault". The cost: a crash can lose up to SYSLOG_FLUSH_MS of routine lines.
// The boot after it still records that it crashed, and why.
// Batches that could not be written, and what they held (stage 5.4d).
static uint32_t s_lost_batches, s_lost_bytes;
static bool     s_write_failing;

uint32_t slog_lost_batches(void) { return s_lost_batches; }
uint32_t slog_lost_bytes(void)   { return s_lost_bytes; }

void slog_flush(void)
{
    if (!s_len) return;

    // Rotate BEFORE a batch that would take log.txt past SYSLOG_MAX, not
    // after: checked after, the file overshot the cap by up to a whole batch
    // (seen on hardware: 33380 bytes). log.txt becomes log.1.txt, replacing
    // the previous one; rename() over an existing file is atomic in LittleFS,
    // so at no point is there no log at all.
    if (s_size && s_size + s_len > SYSLOG_MAX) {
        rename(LOG_PATH, LOG_OLD);
        s_size = 0;
    }

    // Stage 5.4d: a batch that does not reach flash is lost, and until now it
    // was lost in silence - on a full filesystem the log simply stopped, with
    // nothing anywhere to say so. It is still dropped (holding it would grow
    // RAM without end while the disk stays full), but it is counted, and the
    // console is told the first time it happens and again when writing works
    // again.
    FILE *f = fopen(LOG_PATH, "a");
    bool wrote = false;
    if (f) {
        wrote = fwrite(s_buf, 1, s_len, f) == s_len;
        if (wrote) s_size += s_len;
        fclose(f);
        s_writes++;
    }
    if (!wrote) {
        if (!s_write_failing) logf("[slog] cannot write the log (%u bytes lost)\n", (unsigned)s_len);
        s_write_failing = true;
        s_lost_batches++;
        s_lost_bytes += s_len;
    } else if (s_write_failing) {
        s_write_failing = false;
        logf("[slog] writing again; %lu bytes were lost\n", (unsigned long)s_lost_bytes);
    }
    s_len = 0;
}

static void write_line(const char *tag, const char *text)
{
    const int64_t  us = esp_timer_get_time();
    const uint32_t ms = (uint32_t)(us / 1000);
    char line[SYSLOG_LINE + 40];
    const int n = snprintf(line, sizeof(line), "#%-4lu %5lu.%03lu %-5.5s %s\n",
                           (unsigned long)s_boot, (unsigned long)(ms / 1000),
                           (unsigned long)(ms % 1000), tag, text);
    logf("[slog] %s", line);                 // the console sees it too
    if (!s_ready || n <= 0) return;

    const size_t len = strlen(line);
    if (s_len + len > sizeof(s_buf)) slog_flush();
    if (!s_len) s_first_us = us;
    memcpy(s_buf + s_len, line, len);
    s_len += len;

    if (strcmp(tag, "sys") == 0 || strcmp(tag, "fault") == 0) slog_flush();
}

void slog_poll(void)
{
    if (s_len && esp_timer_get_time() - s_first_us >= SYSLOG_FLUSH_MS * 1000LL) slog_flush();
}

void slog(const char *tag, const char *fmt, ...)
{
    char text[SYSLOG_LINE];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    write_line(tag, text);
}

// The budget for lines that did not come from core 0's own system code.
static bool take_token(void)
{
    const int64_t now = esp_timer_get_time();
    const int32_t add = (int32_t)((now - s_refill_us) * SYSLOG_RATE / 1000000);
    if (add > 0) {
        s_tokens += add;
        if (s_tokens > SYSLOG_BURST) s_tokens = SYSLOG_BURST;
        s_refill_us = now;
    }
    if (s_tokens <= 0) return false;
    s_tokens--;
    return true;
}

static size_t tail(char *buf, size_t max)
{
    if (!buf || max < 2) return 0;
    const size_t room = max - 1;
    struct stat a, b;
    const size_t cur = (stat(LOG_PATH, &a) == 0) ? (size_t)a.st_size : 0;
    const size_t old = (stat(LOG_OLD, &b) == 0) ? (size_t)b.st_size : 0;

    // The newest bytes come from log.txt; whatever room is left is filled
    // from the end of log.1.txt, placed in front - so a viewer opened right
    // after a rotation still sees history rather than two lines.
    const size_t from_cur = cur < room ? cur : room;
    const size_t from_old = (room - from_cur) < old ? (room - from_cur) : old;
    size_t used = 0;

    if (from_old) {
        FILE *f = fopen(LOG_OLD, "r");
        if (f) {
            fseek(f, (long)(old - from_old), SEEK_SET);
            used += fread(buf, 1, from_old, f);
            fclose(f);
        }
    }
    if (from_cur) {
        FILE *f = fopen(LOG_PATH, "r");
        if (f) {
            fseek(f, (long)(cur - from_cur), SEEK_SET);
            used += fread(buf + used, 1, from_cur, f);
            fclose(f);
        }
    }
    buf[used] = 0;

    // If the first byte is not the start of a file, it is the middle of a
    // line: drop up to the first newline.
    const bool cut = (from_old && from_old < old) || (!from_old && from_cur < cur);
    if (cut) {
        char *nl = strchr(buf, '\n');
        if (nl) {
            const size_t skip = (size_t)(nl + 1 - buf);
            memmove(buf, nl + 1, used - skip + 1);
            used -= skip;
        }
    }
    return used;
}

static uint32_t h_slog(const k_job_t *j, uint32_t *status)
{
    slog_req_t *r = (slog_req_t *)(uintptr_t)j->a0;
    switch (r->op) {
        case OP_LINE:
            if (!take_token()) { s_dropped++; return 0; }
            if (s_dropped != s_reported) {
                char note[48];
                snprintf(note, sizeof(note), "(%lu lines dropped: over the rate limit)",
                         (unsigned long)(s_dropped - s_reported));
                write_line("log", note);
                s_reported = s_dropped;
            }
            r->tag[sizeof(r->tag) - 1]   = 0;      // trust nothing from a slot
            r->text[sizeof(r->text) - 1] = 0;
            write_line(r->tag, r->text);
            return 0;
        case OP_TAIL:
            slog_flush();                          // the reader sees everything
            r->out = (uint32_t)tail(r->buf, r->len);
            return 0;
        case OP_CLEAR:
            s_len = 0;                             // what was waiting goes too
            unlink(LOG_OLD);
            unlink(LOG_PATH);
            s_size = 0;
            write_line("sys", "log cleared");
            return 0;
        default:
            *status = 1;
            return 0;
    }
}

void slog_init(uint32_t boot)
{
    s_boot      = boot;
    s_ready     = fs_mounted();
    s_refill_us = esp_timer_get_time();
    struct stat st;
    s_size = (s_ready && stat(LOG_PATH, &st) == 0) ? (size_t)st.st_size : 0;
    kmbox_register(SLOG_JOB, h_slog);
}

uint32_t slog_dropped(void) { return s_dropped; }
size_t   slog_size(void)    { return s_size + s_len; }
uint32_t slog_writes(void)  { return s_writes; }

size_t slog_tail_local(char *buf, size_t max)
{
    slog_flush();
    return tail(buf, max);
}

// --- kernel tasks and apps ---------------------------------------------------------------

static void post(slog_req_t *r)
{
    uint32_t res;
    kmbox_call_timeout(SLOG_JOB, (uint32_t)(uintptr_t)r, 0, 0, &res, 0x7FFFFFFFu);
}

static void line_k(const char *tag, const char *fmt, va_list ap)
{
    slog_req_t r;
    memset(&r, 0, sizeof(r));
    r.op = OP_LINE;
    k_snprintf(r.tag, sizeof(r.tag), "%s", tag);
    k_vsnprintf(r.text, sizeof(r.text), fmt, ap);
    post(&r);
}

void slog_k(const char *tag, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    line_k(tag, fmt, ap);
    va_end(ap);
}

void app_log(app_ctx_t *c, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    line_k(c && c->app ? c->app->name : "app", fmt, ap);
    va_end(ap);
}

size_t slog_tail(char *buf, size_t max)
{
    slog_req_t r;
    memset(&r, 0, sizeof(r));
    r.op  = OP_TAIL;
    r.buf = buf;
    r.len = (uint32_t)max;
    post(&r);
    return r.out;
}

int slog_clear(void)
{
    slog_req_t r;
    memset(&r, 0, sizeof(r));
    r.op = OP_CLEAR;
    post(&r);
    return 0;
}
