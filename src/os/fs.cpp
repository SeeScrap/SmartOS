#include "os/fs.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_attr.h"
#include "esp_littlefs.h"

#include "kernel/kmbox.h"
#include "os/kfmt.h"
#include "s3hal/log.h"

#define FS_BASE   "/fs"
#define FS_APPS   FS_BASE "/apps"
#define FS_SYS    FS_BASE "/sys"
#define PART      "storage"

enum { FS_JOB = KMBOX_OP_EXT + 8 };   // the device service has +0 to +4

enum { OP_WRITE = 1, OP_APPEND, OP_READ, OP_SIZE, OP_REMOVE, OP_LIST, OP_USAGE };

// Lives on the calling app's stack for the length of one call - which is why
// the call never times out; see fs.h.
typedef struct {
    uint32_t op;
    char     dir[64];            // /fs/apps/<app>
    char     name[FS_NAME_MAX + 1];
    void    *buf;
    uint32_t len;                // bytes to write, or room to read into
    uint32_t out;                // bytes read, size, or number of names
} fs_req_t;

static bool s_mounted;
static volatile DRAM_ATTR uint32_t s_boot;      // read from core 1 too

bool fs_name_ok(const char *name)
{
    if (!name || !name[0] || name[0] == '.') return false;
    size_t n = 0;
    for (const char *p = name; *p; ++p, ++n) {
        const char ch = *p;
        const bool ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                        (ch >= '0' && ch <= '9') || ch == '.' || ch == '_' || ch == '-';
        if (!ok || n >= FS_NAME_MAX) return false;
    }
    return true;
}

// --- core 0 -------------------------------------------------------------------------

static int from_errno(int e)
{
    if (e == ENOENT) return FS_ERR_NOENT;
    if (e == ENOSPC) return FS_ERR_FULL;
    return FS_ERR_IO;
}

// The app side builds dir and name; this side checks them again anyway. It is
// the side that owns the filesystem, and it trusts nothing that arrives in a
// mailbox slot.
static bool req_ok(const fs_req_t *r)
{
    const size_t base = strlen(FS_APPS "/");
    if (strncmp(r->dir, FS_APPS "/", base) != 0) return false;
    if (!fs_name_ok(r->dir + base)) return false;              // the app's name
    return r->op == OP_LIST || r->op == OP_USAGE || fs_name_ok(r->name);
}

static int do_write(const fs_req_t *r, bool append)
{
    mkdir(r->dir, 0775);                                      // exists is fine
    char path[sizeof(r->dir) + FS_NAME_MAX + 8], tmp[sizeof(path)];
    snprintf(path, sizeof(path), "%s/%s", r->dir, r->name);

    if (append) {
        FILE *f = fopen(path, "ab");
        if (!f) return from_errno(errno);
        const size_t w = fwrite(r->buf, 1, r->len, f);
        const int e = errno;
        fclose(f);
        return (w == r->len) ? FS_OK : from_errno(e);
    }

    // Replace: all of it into a hidden temporary, then one rename. LittleFS
    // renames atomically, so a reader - or a reboot - sees the old file or the
    // new one, never a mixture.
    snprintf(tmp, sizeof(tmp), "%s/.%s.tmp", r->dir, r->name);
    FILE *f = fopen(tmp, "wb");
    if (!f) return from_errno(errno);
    const size_t w = fwrite(r->buf, 1, r->len, f);
    const int e = errno;
    if (fclose(f) != 0 || w != r->len) {
        unlink(tmp);
        return from_errno(e ? e : EIO);
    }
    if (rename(tmp, path) != 0) {
        const int e2 = errno;
        unlink(tmp);
        return from_errno(e2);
    }
    return FS_OK;
}

static int do_read(fs_req_t *r)
{
    char path[sizeof(r->dir) + FS_NAME_MAX + 2];
    snprintf(path, sizeof(path), "%s/%s", r->dir, r->name);
    FILE *f = fopen(path, "rb");
    if (!f) return from_errno(errno);
    r->out = (uint32_t)fread(r->buf, 1, r->len, f);
    fclose(f);
    return FS_OK;
}

static int do_size(fs_req_t *r)
{
    char path[sizeof(r->dir) + FS_NAME_MAX + 2];
    snprintf(path, sizeof(path), "%s/%s", r->dir, r->name);
    struct stat st;
    if (stat(path, &st) != 0) return from_errno(errno);
    r->out = (uint32_t)st.st_size;
    return FS_OK;
}

static int do_remove(fs_req_t *r)
{
    char path[sizeof(r->dir) + FS_NAME_MAX + 2];
    snprintf(path, sizeof(path), "%s/%s", r->dir, r->name);
    return (unlink(path) == 0) ? FS_OK : from_errno(errno);
}

static int do_list(fs_req_t *r)
{
    char  *out = (char *)r->buf;
    size_t used = 0;
    r->out = 0;
    if (r->len) out[0] = 0;

    DIR *d = opendir(r->dir);
    if (!d) return (errno == ENOENT) ? FS_OK : from_errno(errno);   // no files yet
    struct dirent *e;
    while ((e = readdir(d)) != nullptr) {
        if (e->d_name[0] == '.') continue;                    // temporaries
        r->out++;
        const size_t n = strlen(e->d_name);
        if (used + n + 2 <= r->len) {                         // name, '\n', NUL
            memcpy(out + used, e->d_name, n);
            used += n;
            out[used++] = '\n';
            out[used]   = 0;
        }
    }
    closedir(d);
    return FS_OK;
}

static uint32_t h_fs(const k_job_t *j, uint32_t *status)
{
    fs_req_t *r = (fs_req_t *)(uintptr_t)j->a0;
    int rc;
    if (!s_mounted)       rc = FS_ERR_IO;
    else if (!req_ok(r))  rc = FS_ERR_NAME;
    else {
        switch (r->op) {
            case OP_WRITE:  rc = do_write(r, false); break;
            case OP_APPEND: rc = do_write(r, true);  break;
            case OP_READ:   rc = do_read(r);         break;
            case OP_SIZE:   rc = do_size(r);         break;
            case OP_REMOVE: rc = do_remove(r);       break;
            case OP_LIST:   rc = do_list(r);         break;
            case OP_USAGE: {
                size_t t = 0, u = 0;
                fs_info(&t, &u);
                ((uint32_t *)r->buf)[0] = (uint32_t)t;
                ((uint32_t *)r->buf)[1] = (uint32_t)u;
                rc = FS_OK;
                break;
            }
            default:        rc = FS_ERR_IO;          break;
        }
    }
    if (rc != FS_OK) *status = 1;
    return (uint32_t)rc;
}

bool fs_init(void)
{
    esp_vfs_littlefs_conf_t conf = {};
    conf.base_path              = FS_BASE;
    conf.partition_label        = PART;
    conf.format_if_mount_failed = 1;      // first boot: an empty partition
    conf.grow_on_mount          = 1;

    const esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK) {
        logf("[fs] mount failed: %s\n", esp_err_to_name(err));
        return false;
    }
    mkdir(FS_APPS, 0775);
    mkdir(FS_SYS, 0775);
    s_mounted = true;
    kmbox_register(FS_JOB, h_fs);
    return true;
}

bool fs_mounted(void) { return s_mounted; }
uint32_t fs_boot_number(void) { return s_boot; }

void fs_info(size_t *total, size_t *used)
{
    size_t t = 0, u = 0;
    if (s_mounted) esp_littlefs_info(PART, &t, &u);
    if (total) *total = t;
    if (used)  *used  = u;
}

uint32_t fs_boot_count(void)
{
    uint32_t n = 0;
    FILE *f = fopen(FS_SYS "/boots", "r");
    if (f) {
        if (fscanf(f, "%lu", (unsigned long *)&n) != 1) n = 0;
        fclose(f);
    }
    n++;
    s_boot = n;
    // The same replace-by-rename as fs_write: a reset between the two steps
    // leaves the old count, never an empty or half-written file.
    f = fopen(FS_SYS "/.boots.tmp", "w");
    if (f) {
        fprintf(f, "%lu\n", (unsigned long)n);
        fclose(f);
        rename(FS_SYS "/.boots.tmp", FS_SYS "/boots");
    }
    return n;
}

void fs_print_tree(void)
{
    DIR *apps = opendir(FS_APPS);
    if (!apps) { logln("  (no app files)"); return; }
    int total = 0;
    struct dirent *a;
    while ((a = readdir(apps)) != nullptr) {
        if (a->d_name[0] == '.') continue;
        // Names here are at most FS_NAME_MAX; the precision says so to the
        // compiler, which otherwise has to assume a directory entry's 255.
        char dir[96];
        snprintf(dir, sizeof(dir), FS_APPS "/%.32s", a->d_name);
        DIR *d = opendir(dir);
        if (!d) continue;
        struct dirent *e;
        while ((e = readdir(d)) != nullptr) {
            if (e->d_name[0] == '.') continue;
            char path[160];
            snprintf(path, sizeof(path), "%s/%.32s", dir, e->d_name);
            struct stat st;
            const long sz = (stat(path, &st) == 0) ? (long)st.st_size : -1;
            logf("  %-10s %-20s %8ld bytes\n", a->d_name, e->d_name, sz);
            total++;
        }
        closedir(d);
    }
    closedir(apps);
    if (!total) logln("  (no app files)");
}

// --- core 1: the app side --------------------------------------------------------------

static int call(app_ctx_t *c, uint32_t op, const char *name, void *buf, size_t len,
                uint32_t *out)
{
    if (!c || !c->app) return FS_ERR_IO;
    if (op != OP_LIST && op != OP_USAGE && !fs_name_ok(name)) return FS_ERR_NAME;

    fs_req_t r;
    memset(&r, 0, sizeof(r));
    r.op  = op;
    r.buf = buf;
    r.len = (uint32_t)len;
    k_snprintf(r.dir, sizeof(r.dir), FS_APPS "/%s", c->app->name);
    if (name) k_snprintf(r.name, sizeof(r.name), "%s", name);

    // No timeout: see "these calls block" in fs.h. 0x7FFFFFFF ms is 24 days.
    uint32_t res = 0;
    const bool ok = kmbox_call_timeout(FS_JOB, (uint32_t)(uintptr_t)&r, 0, 0, &res,
                                       0x7FFFFFFFu);
    if (out) *out = r.out;
    if (ok) return FS_OK;
    return ((int32_t)res < 0) ? (int32_t)res : FS_ERR_IO;
}

int fs_write(app_ctx_t *c, const char *name, const void *data, size_t len)
{
    return call(c, OP_WRITE, name, (void *)data, len, nullptr);
}

int fs_append(app_ctx_t *c, const char *name, const void *data, size_t len)
{
    return call(c, OP_APPEND, name, (void *)data, len, nullptr);
}

int fs_read(app_ctx_t *c, const char *name, void *buf, size_t max, size_t *got)
{
    uint32_t n = 0;
    const int rc = call(c, OP_READ, name, buf, max, &n);
    if (got) *got = (rc == FS_OK) ? n : 0;
    return rc;
}

int fs_size(app_ctx_t *c, const char *name, size_t *size)
{
    uint32_t n = 0;
    const int rc = call(c, OP_SIZE, name, nullptr, 0, &n);
    if (size) *size = (rc == FS_OK) ? n : 0;
    return rc;
}

int fs_remove(app_ctx_t *c, const char *name)
{
    return call(c, OP_REMOVE, name, nullptr, 0, nullptr);
}

int fs_usage(app_ctx_t *c, uint32_t *total, uint32_t *used)
{
    uint32_t tu[2] = { 0, 0 };
    const int rc = call(c, OP_USAGE, nullptr, tu, sizeof(tu), nullptr);
    if (total) *total = tu[0];
    if (used)  *used  = tu[1];
    return rc;
}

int fs_list(app_ctx_t *c, char *buf, size_t max)
{
    uint32_t n = 0;
    const int rc = call(c, OP_LIST, nullptr, buf, max, &n);
    return (rc == FS_OK) ? (int)n : rc;
}
