#include "os/appload.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "board/log.h"
#include "kernel/klog.h"
#include "kernel/kfmt.h"
#include "kernel/kheap.h"
#include "gfx/text.h"
#include "os/app.h"
#include "os/syslog.h"
#include "os/vm.h"
#include "os/vm_sys.h"

// ---------------------------------------------------------------------------
// Reading an installed app off the filesystem and putting it in the registry.
// See os/appload.h for the file's shape and why Home needs no changes.
//
// WHICH CORE. All of this runs on core 0, at boot, and reads the files with
// plain stdio rather than through os/fs.h. fs.h is the app-facing interface:
// it posts a mailbox job from core 1 to core 0 and sandboxes the path to the
// calling app's own directory. The loader is already on core 0 and is not an
// app, so going through it would be a round trip to itself for a path it is
// not allowed to ask for.
//
// ทั้งหมดนี้รันบน core 0 และอ่านไฟล์ด้วย stdio ตรงๆ ไม่ผ่าน os/fs.h เพราะ
// fs.h เป็นอินเทอร์เฟซของแอป มันส่งงานข้ามคอร์มาหา core 0 และบังคับให้อยู่ใน
// โฟลเดอร์ของแอปที่เรียก ตัวโหลดอยู่บน core 0 อยู่แล้วและไม่ใช่แอป
// ---------------------------------------------------------------------------

#define INSTALLED_DIR "/fs/installed"

static char s_error[96];

// One installed app. The app_t has to live as long as the registry holds a
// pointer to it, so it lives here rather than on a stack somewhere.
typedef struct {
    bool       used;
    app_t      meta;                       // what the registry was given
    char       name[APPLOAD_NAME_MAX];
    uint8_t   *block;                      // one allocation: consts, code, strings
    uint32_t   block_len;
    vm_program_t prog;
    vm_app_t   vm;                         // the syscall side's context
    uint32_t   entry[4];                   // init, event, draw, stop
    bool       stopped;                    // it broke a rule and will not run again
    vm_err_t   why;
} installed_t;

static installed_t s_apps[APPLOAD_MAX];

const char *appload_last_error(void) { return s_error; }

uint32_t appload_bytes(void)
{
    uint32_t n = 0;
    for (int i = 0; i < APPLOAD_MAX; ++i) if (s_apps[i].used) n += s_apps[i].block_len;
    return n;
}

int appload_count(void)
{
    int n = 0;
    for (int i = 0; i < APPLOAD_MAX; ++i) if (s_apps[i].used) n++;
    return n;
}

const char *appload_name_at(int i)
{
    for (int k = 0; k < APPLOAD_MAX; ++k) {
        if (!s_apps[k].used) continue;
        if (i-- == 0) return s_apps[k].name;
    }
    return nullptr;
}

// Which installed app a callback belongs to. The registry hands back the
// app_t it was given, and each one is inside its own record.
static installed_t *find(app_ctx_t *c)
{
    if (!c || !c->app) return nullptr;
    for (int i = 0; i < APPLOAD_MAX; ++i) {
        if (!s_apps[i].used || &s_apps[i].meta != c->app) continue;
        // The context is bound here, on the way in, and nowhere else.
        //
        // The first version set it when the app was loaded, which is before
        // the app has a context at all - so vm.app stayed null and the first
        // sys invalidate called app_invalidate(nullptr) and took the board
        // down. An app's context exists only while it is running, so binding
        // it on entry is the only moment that is always right.
        s_apps[i].vm.app = c;
        return &s_apps[i];
    }
    return nullptr;
}

// --- running one of its callbacks ------------------------------------------
// Every entry into an app's code goes through here, so the budget, the
// framebuffer's lifetime and what happens after a rule is broken are all
// decided in one place.
//
// An app that breaks a rule is stopped for good rather than retried. It has
// shown that it is not the program it claimed to be, and running it again
// just fills the log.
static void run(installed_t *a, int which, fb_t *fb)
{
    if (!a || a->stopped) return;
    const uint32_t entry = a->entry[which];
    if (entry == APPLOAD_NONE) return;

    a->vm.fb = fb;
    const vm_syscalls_t sys = { vm_sys_table(), vm_sys_count(), &a->vm };
    int32_t out = 0;
    const vm_err_t e = vm_run(&a->prog, &sys, entry, VM_BUDGET_PER_CALL, &out);
    a->vm.fb = nullptr;

    if (e != VM_OK && e != VM_HALTED) {
        a->stopped = true;
        a->why     = e;
        // klog, NOT logf. An app's callbacks run in its task on core 1, and
        // logf is core 0's stdio: it takes newlib and FreeRTOS locks that
        // core 1 has no task to block on. The first version called logf
        // here and the board died with an interrupt watchdog timeout the
        // moment an app was stopped - which is to say the error path was
        // the one thing that could not report an error.
        slog("app", "%s stopped: %s", a->name, vm_err_name(e));
        klog("[appload] %s stopped: %s\n", a->name, vm_err_name(e));
    }
}

static void cb_init(app_ctx_t *c)                        { run(find(c), 0, nullptr); }
static void cb_stop(app_ctx_t *c)                        { run(find(c), 3, nullptr); }
static void cb_draw(app_ctx_t *c, fb_t *fb, bool full)
{
    installed_t *a = find(c);
    if (!a) return;
    // The app is told whether this is a fresh screen through a global the
    // loader reserves: globals[0]. An app that does not care ignores it.
    if (a->prog.global_len > 0) a->prog.globals[0] = full ? 1 : 0;
    run(a, 2, fb);
    // An app that was stopped mid-draw leaves the area blank rather than
    // half-finished, and says why, so the user is not left guessing.
    if (a->stopped && fb) {
        char line[64];
        k_snprintf(line, sizeof(line), "%s stopped: %s", a->name, vm_err_name(a->why));
        fb_clear(fb, app_bg());
        fb_text(fb, 8, 8, line, FB_RED, 1);
    }
}
static void cb_event(app_ctx_t *c, const ev_t *ev)
{
    installed_t *a = find(c);
    if (!a || a->prog.global_len < 4) return;
    // The event is handed over in globals 1..3, which the loader reserves.
    a->prog.globals[1] = (int32_t)ev->type;
    a->prog.globals[2] = (int32_t)ev->a;
    a->prog.globals[3] = (int32_t)ev->b;
    run(a, 1, nullptr);
}

// --- reading the file -------------------------------------------------------

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static bool fail(const char *what, const char *name)
{
    k_snprintf(s_error, sizeof(s_error), "%s: %s", name, what);
    logf("[appload] refused %s\n", s_error);
    return false;
}

static installed_t *free_slot(void)
{
    for (int i = 0; i < APPLOAD_MAX; ++i) if (!s_apps[i].used) return &s_apps[i];
    return nullptr;
}

bool appload_one(const char *name)
{
    if (!name || !*name) return fail("no name", "?");
    for (int i = 0; i < APPLOAD_MAX; ++i) {
        if (s_apps[i].used && strcmp(s_apps[i].name, name) == 0)
            return fail("already installed", name);
    }
    if (app_find(name)) return fail("a built-in app has that name", name);

    installed_t *a = free_slot();
    if (!a) return fail("no room for another installed app", name);

    char path[96];
    k_snprintf(path, sizeof(path), INSTALLED_DIR "/%s%s", name, APPLOAD_DIR);
    FILE *f = fopen(path, "rb");
    if (!f) return fail("no such file", name);

    uint8_t hdr[APPLOAD_HDR];
    if (fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr)) { fclose(f); return fail("too short", name); }

    if (rd32(hdr) != APPLOAD_MAGIC)   { fclose(f); return fail("not an app file", name); }
    const uint16_t abi = rd16(hdr + 4);
    if (abi != SMARTOS_ABI_VERSION) {
        fclose(f);
        char w[64];
        k_snprintf(w, sizeof(w), "built for ABI %u, this is %u - rebuild it",
                   (unsigned)abi, (unsigned)SMARTOS_ABI_VERSION);
        return fail(w, name);
    }
    if (rd16(hdr + 6) != 0) { fclose(f); return fail("unknown flags", name); }

    // The file carries its own name and so does the filename. Two sources of
    // truth is one too many, so they have to agree - tools/install_app.py
    // takes the name out of the file for exactly this reason, and a
    // mismatch means somebody renamed the file by hand.
    char inside[APPLOAD_NAME_MAX + 1];
    memcpy(inside, hdr + 8, APPLOAD_NAME_MAX);
    inside[APPLOAD_NAME_MAX] = 0;
    if (strcmp(inside, name) != 0) {
        fclose(f);
        char w[72];
        k_snprintf(w, sizeof(w), "the file calls itself '%s'", inside);
        return fail(w, name);
    }

    const uint16_t order   = rd16(hdr + 24);
    const uint16_t nglob   = rd16(hdr + 26);
    const uint16_t nstack  = rd16(hdr + 28);
    const uint16_t nconst  = rd16(hdr + 30);
    const uint32_t codelen = rd32(hdr + 32);
    const uint32_t strlen_ = rd32(hdr + 36);

    // Four globals are the loader's: globals[0] is draw()'s "full" flag and
    // 1..3 carry an event. An app that declares fewer cannot be given one.
    if (nglob < 4)  { fclose(f); return fail("needs at least four globals", name); }
    if (nstack < 8) { fclose(f); return fail("stack too small", name); }

    // Everything the file claims, checked against the file's actual size
    // before a byte of it is believed.
    struct stat st;
    if (stat(path, &st) != 0) { fclose(f); return fail("cannot size it", name); }
    const uint32_t body = (uint32_t)nconst * 4 + codelen + strlen_;
    if ((uint32_t)st.st_size != APPLOAD_HDR + body) {
        fclose(f); return fail("header and file size disagree", name);
    }
    if (codelen == 0 || codelen > 32768 || strlen_ > 8192 || nconst > 1024) {
        fclose(f); return fail("unreasonable sizes", name);
    }

    // One allocation from the kernel pool for everything it owns. The
    // strings get one extra byte, always zero, so a string that starts
    // inside the blob always ends inside it.
    const uint32_t need = body + (uint32_t)nglob * 4 + (uint32_t)nstack * 4 + 1;
    uint8_t *block = (uint8_t *)kcalloc(need, 1);
    if (!block) { fclose(f); return fail("not enough room in the kernel pool", name); }

    if (fread(block, 1, body, f) != body) {
        kfree(block); fclose(f); return fail("could not read it all", name);
    }
    fclose(f);

    uint8_t *p   = block;
    int32_t *kst = (int32_t *)p;                       p += (uint32_t)nconst * 4;
    uint8_t *code = p;                                 p += codelen;
    char    *strs = (char *)p;                         p += strlen_ + 1;
    int32_t *globals = (int32_t *)p;                   p += (uint32_t)nglob * 4;
    int32_t *stack   = (int32_t *)p;

    memset(a, 0, sizeof(*a));
    k_snprintf(a->name, sizeof(a->name), "%s", name);
    a->block      = block;
    a->block_len  = need;
    a->prog.code        = code;
    a->prog.code_len    = codelen;
    a->prog.consts      = kst;
    a->prog.const_len   = nconst;
    a->prog.strings     = strs;
    a->prog.strings_len = strlen_;
    a->prog.globals     = globals;
    a->prog.global_len  = nglob;
    a->prog.stack       = stack;
    a->prog.stack_len   = nstack;
    a->vm.prog = &a->prog;

    for (int i = 0; i < 4; ++i) {
        a->entry[i] = rd32(hdr + 40 + i * 4);
        if (a->entry[i] != APPLOAD_NONE && a->entry[i] >= codelen) {
            kfree(block); return fail("an entry point outside its code", name);
        }
    }
    if (a->entry[2] == APPLOAD_NONE) { kfree(block); return fail("no draw", name); }

    a->meta.name      = a->name;
    a->meta.about     = "installed";
    a->meta.user_size = 0;              // its state is in the VM, not in c->user
    a->meta.init      = cb_init;
    a->meta.on_event  = cb_event;
    a->meta.draw      = cb_draw;
    a->meta.stop      = cb_stop;
    a->used = true;

    // Installed apps sit after the built-in ones unless they ask otherwise,
    // so installing something cannot push Home's own entries around.
    if (!app_register(&a->meta, order ? order : 100)) {
        kfree(block);
        a->used = false;
        return fail("the registry is full", name);
    }
    logf("[appload] %s: %lu B code, %u consts, %lu B strings, %u globals\n",
         name, (unsigned long)codelen, (unsigned)nconst,
         (unsigned long)strlen_, (unsigned)nglob);
    return true;
}

int appload_scan(void)
{
    mkdir(INSTALLED_DIR, 0775);
    DIR *d = opendir(INSTALLED_DIR);
    if (!d) return 0;

    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != nullptr) {
        const size_t len = strlen(e->d_name);
        const size_t ext = strlen(APPLOAD_DIR);
        if (len <= ext || strcmp(e->d_name + len - ext, APPLOAD_DIR) != 0) continue;
        char name[APPLOAD_NAME_MAX];
        const size_t keep = len - ext;
        if (keep >= sizeof(name)) continue;
        memcpy(name, e->d_name, keep);
        name[keep] = 0;
        if (appload_one(name)) n++;
    }
    closedir(d);
    if (n) logf("[appload] %d installed app(s) registered\n", n);
    return n;
}

bool appload_forget(const char *name)
{
    for (int i = 0; i < APPLOAD_MAX; ++i) {
        installed_t *a = &s_apps[i];
        if (!a->used || strcmp(a->name, name) != 0) continue;
        if (app_is_open(name)) return fail("it is open - close it first", name);
        app_unregister(&a->meta);
        kfree(a->block);
        a->used = false;
        return true;
    }
    return fail("not installed", name);
}
