#include "os/vm_sys.h"

#include <string.h>

#include "gfx/text.h"
#include "kernel/kfmt.h"
#include "kernel/ktime.h"
#include "os/krand.h"
#include "os/settings.h"
#include "os/syslog.h"
#include "port/kport.h"

// ---------------------------------------------------------------------------
// The system calls an installed app can make. See os/vm_sys.h for why the
// numbers in that header are the thing that must not change.
//
// Every one of these is written to the same shape: check the argument count,
// check anything that indexes into the app's memory, then do the work. There
// is no fast path and no unchecked variant, because the caller is a file
// somebody put on the board and the only reason it is allowed to run is that
// nothing here trusts it.
// ---------------------------------------------------------------------------

#define ARGS(n) do { if (argc != (n)) { *err = VM_ERR_SYSCALL; return 0; } } while (0)

// Resolves a string handle. Returns null if it points anywhere it should not,
// and the caller turns that into a stopped app.
static const char *str_of(vm_app_t *a, int32_t h)
{
    if (h >= 0) {
        if (!a->prog->strings || (uint32_t)h >= a->prog->strings_len) return nullptr;
        // The blob is NUL-terminated at the end by the loader, so a string
        // starting inside it always ends inside it.
        return a->prog->strings + h;
    }
    const int slot = -h - 1;
    if (slot < 0 || slot >= VM_SCRATCH_SLOTS) return nullptr;
    return a->scratch[slot];
}

// Hands out the next scratch buffer, round-robin. Two is enough for one call
// that formats a string and immediately draws it; an app that wants three
// live at once has to draw one of them first.
static char *scratch_next(vm_app_t *a, int32_t *handle)
{
    const uint8_t i = a->next_scratch;
    a->next_scratch = (uint8_t)((i + 1) % VM_SCRATCH_SLOTS);
    *handle = -(int32_t)i - 1;
    return a->scratch[i];
}

// Drawing is only legal while draw() is running. Outside it there is no
// framebuffer to draw into, and an app that tries is stopped rather than
// quietly ignored - a drawing call that does nothing is a bug that hides.
#define NEED_FB() do { if (!a->fb) { *err = VM_ERR_SYSCALL; return 0; } } while (0)

static int32_t s_invalidate(void *u, const int32_t *, int argc, vm_err_t *err)
{
    vm_app_t *a = (vm_app_t *)u; ARGS(0);
    app_invalidate(a->app);
    return 0;
}
static int32_t s_millis(void *, const int32_t *, int argc, vm_err_t *err)
{ ARGS(0); return (int32_t)k_millis(); }

static int32_t s_rand(void *, const int32_t *v, int argc, vm_err_t *err)
{ ARGS(1); return v[0] > 0 ? (int32_t)(k_rand() % (uint32_t)v[0]) : 0; }

static int32_t s_rgb(void *, const int32_t *v, int argc, vm_err_t *err)
{ ARGS(3); return (int32_t)fb_rgb((uint8_t)v[0], (uint8_t)v[1], (uint8_t)v[2]); }

static int32_t s_bg(void *, const int32_t *, int argc, vm_err_t *err)
{ ARGS(0); return (int32_t)app_bg(); }

static int32_t s_clear(void *u, const int32_t *v, int argc, vm_err_t *err)
{
    vm_app_t *a = (vm_app_t *)u; ARGS(1); NEED_FB();
    fb_clear(a->fb, (uint16_t)v[0]);
    return 0;
}
static int32_t s_fill_rect(void *u, const int32_t *v, int argc, vm_err_t *err)
{
    vm_app_t *a = (vm_app_t *)u; ARGS(5); NEED_FB();
    fb_fill_rect(a->fb, v[0], v[1], v[2], v[3], (uint16_t)v[4]);
    return 0;
}
static int32_t s_rect(void *u, const int32_t *v, int argc, vm_err_t *err)
{
    vm_app_t *a = (vm_app_t *)u; ARGS(5); NEED_FB();
    fb_rect(a->fb, v[0], v[1], v[2], v[3], (uint16_t)v[4]);
    return 0;
}
static int32_t s_line(void *u, const int32_t *v, int argc, vm_err_t *err)
{
    vm_app_t *a = (vm_app_t *)u; ARGS(5); NEED_FB();
    fb_line(a->fb, v[0], v[1], v[2], v[3], (uint16_t)v[4]);
    return 0;
}
static int32_t s_text(void *u, const int32_t *v, int argc, vm_err_t *err)
{
    vm_app_t *a = (vm_app_t *)u; ARGS(5); NEED_FB();
    const char *s = str_of(a, v[2]);
    if (!s) { *err = VM_ERR_RANGE; return 0; }
    return fb_text(a->fb, v[0], v[1], s, (uint16_t)v[3], v[4]);
}
static int32_t s_text_bg(void *u, const int32_t *v, int argc, vm_err_t *err)
{
    vm_app_t *a = (vm_app_t *)u; ARGS(6); NEED_FB();
    const char *s = str_of(a, v[2]);
    if (!s) { *err = VM_ERR_RANGE; return 0; }
    return fb_text_bg(a->fb, v[0], v[1], s, (uint16_t)v[3], (uint16_t)v[4], v[5]);
}
static int32_t s_text_width(void *u, const int32_t *v, int argc, vm_err_t *err)
{
    vm_app_t *a = (vm_app_t *)u; ARGS(2);
    const char *s = str_of(a, v[0]);
    if (!s) { *err = VM_ERR_RANGE; return 0; }
    return fb_text_width(s, v[1]);
}

// Formatting, with exactly one conversion: %d, up to three times. Not
// printf: the app's format string is data from a file, and handing data to a
// real printf is how a program gets talked into reading memory that is not
// its own. Anything that is not "%d" is copied through as itself.
static int32_t s_fmt(void *u, const int32_t *v, int argc, vm_err_t *err)
{
    // One to four: the format string, then as many values as it mentions.
    // Insisting on four would make every caller pad with zeros, which is the
    // compiler's business leaking into the interface.
    vm_app_t *a = (vm_app_t *)u;
    if (argc < 1 || argc > 4) { *err = VM_ERR_SYSCALL; return 0; }
    const char *f = str_of(a, v[0]);
    if (!f) { *err = VM_ERR_RANGE; return 0; }
    int32_t handle = 0;
    char *out = scratch_next(a, &handle);
    size_t o = 0;
    int arg = 1;
    for (size_t i = 0; f[i] && o + 1 < VM_SCRATCH_BYTES; ++i) {
        if (f[i] == '%' && f[i + 1] == 'd' && arg < argc) {
            char num[12];
            k_snprintf(num, sizeof(num), "%ld", (long)v[arg++]);
            for (size_t j = 0; num[j] && o + 1 < VM_SCRATCH_BYTES; ++j) out[o++] = num[j];
            i++;                       // step over the 'd'
        } else {
            out[o++] = f[i];
        }
    }
    out[o] = 0;
    return handle;
}

static int32_t s_num(void *u, const int32_t *v, int argc, vm_err_t *err)
{
    vm_app_t *a = (vm_app_t *)u; ARGS(1);
    int32_t handle = 0;
    char *out = scratch_next(a, &handle);
    k_snprintf(out, VM_SCRATCH_BYTES, "%ld", (long)v[0]);
    return handle;
}

static int32_t s_setting_get(void *u, const int32_t *v, int argc, vm_err_t *err)
{
    vm_app_t *a = (vm_app_t *)u; ARGS(1);
    const char *k = str_of(a, v[0]);
    if (!k) { *err = VM_ERR_RANGE; return 0; }
    return settings_get(k);
}
static int32_t s_setting_set(void *u, const int32_t *v, int argc, vm_err_t *err)
{
    vm_app_t *a = (vm_app_t *)u; ARGS(2);
    const char *k = str_of(a, v[0]);
    if (!k) { *err = VM_ERR_RANGE; return 0; }
    return settings_set(k, v[1]) ? 1 : 0;
}
static int32_t s_log(void *u, const int32_t *v, int argc, vm_err_t *err)
{
    vm_app_t *a = (vm_app_t *)u; ARGS(1);
    const char *s = str_of(a, v[0]);
    if (!s) { *err = VM_ERR_RANGE; return 0; }
    slog("app", "%s", s);
    return 0;
}

static int32_t s_width(void *, const int32_t *, int argc, vm_err_t *err)
{ ARGS(0); return LCD_WIDTH; }
static int32_t s_height(void *, const int32_t *, int argc, vm_err_t *err)
{ ARGS(0); return APP_AREA_H; }

// The table. Its order IS the numbering in os/vm_sys.h, and a static_assert
// below holds the two together - an entry added to one and not the other
// stops the build rather than renumbering every app on the board.
static const vm_syscall_fn s_table[] = {
    s_invalidate, s_millis, s_rand, s_rgb, s_bg,
    s_clear, s_fill_rect, s_rect, s_line,
    s_text, s_text_bg, s_text_width,
    s_fmt, s_num,
    s_setting_get, s_setting_set, s_log,
    s_width, s_height,
};
static_assert(sizeof(s_table) / sizeof(s_table[0]) == VMSYS_COUNT_,
              "the table in vm_sys.cpp and the numbers in vm_sys.h disagree - "
              "every installed app depends on those numbers, so fix the table");

const vm_syscall_fn *vm_sys_table(void) { return s_table; }
uint16_t             vm_sys_count(void) { return VMSYS_COUNT_; }
