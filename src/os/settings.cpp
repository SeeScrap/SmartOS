#include "os/settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_timer.h"

#include "kernel/kmbox.h"
#include "os/evbus.h"
#include "os/fs.h"
#include "kernel/kfmt.h"
#include "os/syslog.h"

#define SETTINGS_PATH "/fs/sys/settings"
#define SETTINGS_TMP  "/fs/sys/.settings.tmp"

enum { SETTINGS_JOB = KMBOX_OP_EXT + 14 };   // syslog has +12

typedef struct {
    char             key[SETTINGS_KEY_MAX + 1];
    volatile int32_t value;          // read from either core, written by core 0
    int32_t          def, min, max;
    settings_hook_t  hook;           // core 0
} setting_t;

// DRAM: settings_get() runs on core 1 too.
static DRAM_ATTR setting_t s_tab[SETTINGS_MAX];
static DRAM_ATTR int       s_n;

// Core 0 only.
static bool     s_dirty;
static int64_t  s_changed_us;
static uint32_t s_saves;

typedef struct {
    char     key[SETTINGS_KEY_MAX + 1];
    int32_t  value;
} settings_req_t;

static int find(const char *key)
{
    for (int i = 0; i < s_n; ++i) if (strcmp(s_tab[i].key, key) == 0) return i;
    return -1;
}

static int32_t clamp(const setting_t *s, int32_t v)
{
    return v < s->min ? s->min : (v > s->max ? s->max : v);
}

bool settings_define(const char *key, int32_t def, int32_t min, int32_t max,
                     settings_hook_t on_change)
{
    if (s_n >= SETTINGS_MAX || !key || strlen(key) > SETTINGS_KEY_MAX || find(key) >= 0) {
        return false;
    }
    setting_t *s = &s_tab[s_n];
    strcpy(s->key, key);
    s->min   = min;
    s->max   = max;
    s->def   = def;
    s->value = def;
    s->hook  = on_change;
    s_n++;
    return true;
}

static uint32_t h_settings(const k_job_t *j, uint32_t *status);

void settings_load(void)
{
    kmbox_register(SETTINGS_JOB, h_settings);
    FILE *f = fopen(SETTINGS_PATH, "r");
    int loaded = 0;
    if (f) {
        char line[64];
        while (fgets(line, sizeof(line), f)) {
            char *eq = strchr(line, '=');
            if (!eq) continue;
            *eq = 0;
            const int i = find(line);
            if (i < 0) continue;                 // a setting this build no longer has
            s_tab[i].value = clamp(&s_tab[i], (int32_t)strtol(eq + 1, nullptr, 10));
            loaded++;
        }
        fclose(f);
    }
    for (int i = 0; i < s_n; ++i) if (s_tab[i].hook) s_tab[i].hook(s_tab[i].value);
    slog("sys", "settings: %d defined, %d loaded from flash", s_n, loaded);
}

int32_t settings_get(const char *key)
{
    const int i = find(key);
    return i >= 0 ? s_tab[i].value : 0;
}

bool settings_range(const char *key, int32_t *min, int32_t *max)
{
    const int i = find(key);
    if (i < 0) return false;
    if (min) *min = s_tab[i].min;
    if (max) *max = s_tab[i].max;
    return true;
}

int settings_set(const char *key, int32_t value)
{
    const int i = find(key);
    if (i < 0) return -1;
    setting_t *s = &s_tab[i];
    const int32_t v = clamp(s, value);
    if (v == s->value) return 0;
    s->value = v;
    if (s->hook) s->hook(v);
    evbus_publish(EV_SYS, EV_SRC_SYSTEM, SYS_SETTING_CHANGED, (uint32_t)i);
    s_dirty      = true;
    s_changed_us = esp_timer_get_time();
    return 0;
}

void settings_poll(void)
{
    if (!s_dirty || esp_timer_get_time() - s_changed_us < SETTINGS_SAVE_MS * 1000LL) return;
    s_dirty = false;

    // Replace-by-rename, like every other file the system keeps: a reset in
    // the middle leaves the previous settings, never half of them.
    FILE *f = fopen(SETTINGS_TMP, "w");
    if (!f) return;
    for (int i = 0; i < s_n; ++i) fprintf(f, "%s=%ld\n", s_tab[i].key, (long)s_tab[i].value);
    fclose(f);
    rename(SETTINGS_TMP, SETTINGS_PATH);
    s_saves++;
}

uint32_t settings_saves(void) { return s_saves; }

// --- through the mailbox ---------------------------------------------------------------

static uint32_t h_settings(const k_job_t *j, uint32_t *status)
{
    settings_req_t *r = (settings_req_t *)(uintptr_t)j->a0;
    r->key[sizeof(r->key) - 1] = 0;
    if (settings_set(r->key, r->value) != 0) *status = 1;
    return 0;
}

int settings_set_k(const char *key, int32_t value)
{
    settings_req_t r;
    memset(&r, 0, sizeof(r));
    k_snprintf(r.key, sizeof(r.key), "%s", key);
    r.value = value;
    uint32_t res;
    // The request is on this stack and core 0 reads it, so - as for files and
    // log lines - there is no timeout: giving up while core 0 was mid-read
    // would leave it reading a stack frame already reused.
    return kmbox_call_timeout(SETTINGS_JOB, (uint32_t)(uintptr_t)&r, 0, 0, &res,
                              0x7FFFFFFFu) ? 0 : -1;
}

// --- an app's own, in its folder ------------------------------------------------------------

int32_t app_setting_get(app_ctx_t *c, const char *key, int32_t def)
{
    char buf[256];               // app stacks are 2 KB: kept small
    size_t got = 0;
    if (fs_read(c, "settings", buf, sizeof(buf) - 1, &got) != FS_OK) return def;
    buf[got] = 0;
    const size_t klen = strlen(key);
    for (char *p = buf; p && *p; ) {
        char *nl = strchr(p, '\n');
        if (strncmp(p, key, klen) == 0 && p[klen] == '=') return (int32_t)strtol(p + klen + 1, nullptr, 10);
        p = nl ? nl + 1 : nullptr;
    }
    return def;
}

int app_setting_set(app_ctx_t *c, const char *key, int32_t value)
{
    char in[256], out[256];      // app stacks are 2 KB: kept small
    size_t got = 0;
    if (fs_read(c, "settings", in, sizeof(in) - 1, &got) != FS_OK) got = 0;
    in[got] = 0;

    // Every other line kept as it was, this key's replaced (or added at the end).
    size_t used = 0;
    const size_t klen = strlen(key);
    for (char *p = in; *p; ) {
        char *nl = strchr(p, '\n');
        const size_t len = nl ? (size_t)(nl - p) : strlen(p);
        const bool mine = strncmp(p, key, klen) == 0 && p[klen] == '=';
        if (!mine && len && used + len + 1 < sizeof(out)) {
            memcpy(out + used, p, len);
            used += len;
            out[used++] = '\n';
        }
        if (!nl) break;
        p = nl + 1;
    }
    used += (size_t)k_snprintf(out + used, sizeof(out) - used, "%s=%ld\n", key, (long)value);
    if (used >= sizeof(out)) return FS_ERR_FULL;
    return fs_write(c, "settings", out, used);
}

int         settings_count(void)     { return s_n; }
const char *settings_key_at(int i)   { return (i >= 0 && i < s_n) ? s_tab[i].key : nullptr; }
