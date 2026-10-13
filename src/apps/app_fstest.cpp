// Test only, and deliberately not registered: the stage 4.4 file checks, run
// from where apps really call them - a task on core 1, through the mailbox, to
// LittleFS on core 0, under the global park hook. Its init() runs every check
// and leaves the results in g_fscheck for the test script on core 0 to print.

#include "apps/apps.h"

#include <string.h>

#include "kernel/ktime.h"
#include "os/fs.h"
#include "os/kfmt.h"

fscheck_t     g_fscheck[FSCHECK_MAX];
volatile int  g_fscheck_n;
volatile bool g_fscheck_done;

static uint8_t s_big[16384];          // internal RAM: the test's own large file
static uint8_t s_back[16384];

static void check(const char *what, bool ok, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

static void check(const char *what, bool ok, const char *fmt, ...)
{
    if (g_fscheck_n >= FSCHECK_MAX) return;
    fscheck_t *c = &g_fscheck[g_fscheck_n];
    c->what = what;
    c->ok   = ok;
    va_list ap;
    va_start(ap, fmt);
    k_vsnprintf(c->detail, sizeof(c->detail), fmt, ap);
    va_end(ap);
    g_fscheck_n = g_fscheck_n + 1;
}

static void init(app_ctx_t *c)
{
    char   buf[128];
    size_t got = 0, sz = 0;
    int    rc;

    // Leftovers from an earlier run would make the checks below lie.
    fs_remove(c, "a.txt");
    fs_remove(c, "log");
    fs_remove(c, "big");

    rc = fs_write(c, "a.txt", "hello", 5);
    const int r2 = fs_read(c, "a.txt", buf, sizeof(buf), &got);
    check("write, read back", rc == FS_OK && r2 == FS_OK && got == 5 && memcmp(buf, "hello", 5) == 0,
          "write %d, read %d, %u bytes", rc, r2, (unsigned)got);

    // Replacing with something shorter must leave only the new bytes: a
    // replace that merely overwrote in place would read "hillo".
    rc = fs_write(c, "a.txt", "hi", 2);
    fs_read(c, "a.txt", buf, sizeof(buf), &got);
    fs_size(c, "a.txt", &sz);
    check("replace, shorter", rc == FS_OK && got == 2 && sz == 2 && memcmp(buf, "hi", 2) == 0,
          "%u bytes read, size %u", (unsigned)got, (unsigned)sz);

    fs_append(c, "log", "one,", 4);
    fs_append(c, "log", "two,", 4);
    fs_append(c, "log", "three", 5);
    fs_read(c, "log", buf, sizeof(buf) - 1, &got);
    buf[got] = 0;
    check("append x3", strcmp(buf, "one,two,three") == 0, "\"%s\"", buf);

    // Two files, and no hidden temporary left behind by the replaces.
    const int n = fs_list(c, buf, sizeof(buf));
    const bool both = strstr(buf, "a.txt\n") && strstr(buf, "log\n") && !strstr(buf, ".tmp");
    check("list", n == 2 && both, "%d names", n);

    rc = fs_remove(c, "a.txt");
    const int r3 = fs_size(c, "a.txt", &sz);
    check("remove", rc == FS_OK && r3 == FS_ERR_NOENT, "remove %d, then size %d", rc, r3);

    // Names that would reach outside the app's folder, or are not names.
    static const char *bad[] = { "../x", "a/b", ".hidden", "", "a\\b",
                                 "a_name_that_is_far_too_long_for_it_1" };
    int refused = 0;
    for (const char *b : bad) if (fs_write(c, b, "x", 1) == FS_ERR_NAME) refused++;
    check("bad names refused", refused == 6, "%d of 6", refused);

    // A file big enough to span several LittleFS blocks, timed both ways.
    for (size_t i = 0; i < sizeof(s_big); ++i) s_big[i] = (uint8_t)(i * 31 + 7);
    uint32_t t0 = k_millis();
    rc = fs_write(c, "big", s_big, sizeof(s_big));
    const uint32_t wms = k_millis() - t0;
    memset(s_back, 0, sizeof(s_back));
    t0 = k_millis();
    fs_read(c, "big", s_back, sizeof(s_back), &got);
    const uint32_t rms = k_millis() - t0;
    const bool same = (got == sizeof(s_big)) && memcmp(s_big, s_back, sizeof(s_big)) == 0;
    check("16 KB file", rc == FS_OK && same,
          "write %lu ms (%lu KB/s), read %lu ms (%lu KB/s), identical: %s",
          (unsigned long)wms, (unsigned long)(16000 / (wms ? wms : 1)),
          (unsigned long)rms, (unsigned long)(16000 / (rms ? rms : 1)), same ? "yes" : "NO");

    fs_remove(c, "log");
    fs_remove(c, "big");
    g_fscheck_done = true;
}

static void draw(app_ctx_t *c, fb_t *fb, bool full)
{
    (void)c; (void)fb; (void)full;
}

const app_t app_fstest = {
    .name = "fstest",
    .init = init,
    .draw = draw,
};
