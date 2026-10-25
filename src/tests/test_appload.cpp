#include "tests/tests.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "board/log.h"
#include "kernel/kheap.h"
#include "os/app.h"
#include "os/appload.h"
#include "tests/harness.h"
#include "tests/test_app_blob.h"

// ---------------------------------------------------------------------------
// Stage 7 - installing an app, checked on every boot.
//
// The whole of stage 7 was verified by hand with a cable: compile an app,
// send it down, open it, press buttons, reboot, uninstall. That proved it
// worked once. This is what notices when it stops.
//
// The board installs an app on itself: it writes the bytes of
// examples/apps/selftest.sap - compiled on a computer and embedded by
// tools/mkappblob.py - into /fs/installed, loads them through exactly the
// path the shell uses, runs it, and takes it away again. Nothing here is a
// shortcut around the real loader; the only thing left out is the cable.
//
// ทั้ง stage 7 ถูกพิสูจน์ด้วยมือผ่านสาย ซึ่งพิสูจน์ได้ว่ามันเคยทำงาน ไฟล์นี้คือ
// สิ่งที่จะรู้ตัวเมื่อมันเลิกทำงาน บอร์ดติดตั้งแอปใส่ตัวเองผ่านเส้นทางเดียวกับที่
// เชลล์ใช้ สิ่งเดียวที่ตัดออกคือสาย
// ---------------------------------------------------------------------------

#define TEST_APP "selftest"

static bool write_blob(void)
{
    mkdir(APPLOAD_DIR, 0775);
    FILE *f = fopen(APPLOAD_DIR "/" TEST_APP APPLOAD_EXT, "wb");
    if (!f) return false;
    const size_t n = fwrite(k_test_app, 1, sizeof(k_test_app), f);
    fclose(f);
    return n == sizeof(k_test_app);
}

static bool in_registry(const char *name)
{
    for (int i = 0; i < app_registry_count(); ++i) {
        if (strcmp(app_registry_at(i)->name, name) == 0) return true;
    }
    return false;
}

void appload_test(void)
{
    logln();
    logln("--- stage 7: installing an app ---");
    char d[150];

    // Start from nothing, whatever an earlier run or a real install left.
    if (appload_count() > 0) appload_forget(TEST_APP);
    unlink(APPLOAD_DIR "/" TEST_APP APPLOAD_EXT);

    const uint32_t before = appload_bytes();
    const int reg_before  = app_registry_count();

    // 1. The file, written the way the shell writes it.
    {
        const bool ok = write_blob();
        snprintf(d, sizeof(d), "%u bytes to " APPLOAD_DIR "/" TEST_APP APPLOAD_EXT,
                 (unsigned)sizeof(k_test_app));
        sdk_case("an app arrives as a file", ok, d);
    }

    // 2. Loaded, checked and registered - the whole of appload_one.
    const bool loaded = appload_one(TEST_APP);
    {
        snprintf(d, sizeof(d), "%s; registry %d -> %d, %lu B of the kernel pool",
                 loaded ? "loaded" : appload_last_error(),
                 reg_before, app_registry_count(),
                 (unsigned long)(appload_bytes() - before));
        sdk_case("it loads and registers", loaded && in_registry(TEST_APP) &&
                 app_registry_count() == reg_before + 1 && appload_bytes() > before, d);
    }

    // 3. It runs. Opening it calls init, focusing it calls draw, and a button
    //    reaches its event callback - all three are the app's own bytecode
    //    going through the interpreter and out to the system, so a pass here
    //    means the machine, the system call table and the loader all still
    //    agree with each other.
    //
    //    An unfocused app never draws (that is the app model, checked in
    //    test_apps.cpp), so focusing it is part of the test rather than a
    //    convenience.
    if (loaded) {
        const int id = app_start(app_find(TEST_APP));
        if (id >= 0) app_focus(id);
        pump(300);
        tap(0);                                  // UP, the way a finger sends it
        pump(300);
        const app_ctx_t *c = (id >= 0) ? app_get(id) : nullptr;
        snprintf(d, sizeof(d), "id %d, %lu frame(s), %lu event(s)", id,
                 c ? (unsigned long)c->frames : 0ul, c ? (unsigned long)c->events : 0ul);
        sdk_case("it opens, draws and hears a button",
                 id >= 0 && c && c->frames > 0 && c->events > 0, d);

        if (id >= 0) {
            app_stop(id);
            pump(400);
        }
    }

    // 4. Taken away again, and every byte it held comes back.
    if (loaded) {
        const bool gone = appload_forget(TEST_APP);
        snprintf(d, sizeof(d), "%s; registry back to %d, pool back to %lu B",
                 gone ? "forgotten" : appload_last_error(),
                 app_registry_count(), (unsigned long)appload_bytes());
        sdk_case("it uninstalls cleanly", gone && !in_registry(TEST_APP) &&
                 app_registry_count() == reg_before && appload_bytes() == before, d);
    }

    // 5. The refusals. A file that is not an app, and one built for an ABI
    //    this firmware does not speak, both have to be turned away with a
    //    reason rather than run.
    {
        FILE *f = fopen(APPLOAD_DIR "/junk" APPLOAD_EXT, "wb");
        if (f) { fputs("this is not an app at all", f); fclose(f); }
        const bool refused = !appload_one("junk");
        snprintf(d, sizeof(d), "%s", appload_last_error());
        sdk_case("rubbish is refused", refused, d);
        unlink(APPLOAD_DIR "/junk" APPLOAD_EXT);
    }
    {
        unsigned char bad[sizeof(k_test_app)];
        memcpy(bad, k_test_app, sizeof(bad));
        bad[4] = (unsigned char)(SMARTOS_ABI_VERSION + 1);   // a future ABI
        FILE *f = fopen(APPLOAD_DIR "/future" APPLOAD_EXT, "wb");
        if (f) { fwrite(bad, 1, sizeof(bad), f); fclose(f); }
        const bool refused = !appload_one("future");
        snprintf(d, sizeof(d), "%s", appload_last_error());
        sdk_case("another ABI is refused", refused, d);
        unlink(APPLOAD_DIR "/future" APPLOAD_EXT);
    }

    unlink(APPLOAD_DIR "/" TEST_APP APPLOAD_EXT);
}
