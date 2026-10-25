#include "os/shell.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "esp_system.h"
#include "hal/usb_serial_jtag_ll.h"

#include "kernel/kheap.h"
#include "kernel/ktask.h"
#include "gfx/fb.h"
#include "os/app.h"
#include "os/appload.h"
#include "os/boot.h"
#include "os/radio.h"
#include "system.h"
#include "os/evbus.h"
#include "os/fs.h"
#include "os/settings.h"
#include "os/syslog.h"
#include "board/input.h"
#include "board/log.h"

#define PROMPT     "s3> "
#define MAX_ARGS   8

static char     s_line[SHELL_LINE];
static size_t   s_len;
static bool     s_overflow;          // this line is already too long: drop it
static char     s_last;              // to treat \r\n as one end of line
static uint32_t s_commands;

typedef void (*cmd_fn)(int argc, char **argv);
typedef struct {
    const char *name;
    const char *args;
    const char *help;
    cmd_fn      fn;
} cmd_t;

static void cmd_help(int, char **);

// --- helpers -----------------------------------------------------------------------

// The USB Serial/JTAG port sends what it holds at a newline or when a 64-byte
// packet fills. A prompt, and the echo of each typed character, end in
// neither - found on the first run, where "s3> " reached the PC only glued to
// the next heartbeat line. So those are pushed out by hand, in two steps:
// newlib's stdout held them first (line-buffered here, despite log_init's
// setvbuf - flushing only the FIFO was tried and changed nothing), then the
// FIFO.
static void push(void)
{
    fflush(stdout);
    usb_serial_jtag_ll_txfifo_flush();
}

// A path the shell may touch: under /fs, no "..". A relative one is taken
// from /fs. Returns false, with nothing written, for anything else.
static bool fs_path(const char *in, char *out, size_t max)
{
    if (!in || !*in) in = "/fs";
    int n;
    if (in[0] == '/') n = snprintf(out, max, "%s", in);
    else              n = snprintf(out, max, "/fs/%s", in);
    if (n <= 0 || (size_t)n >= max) return false;
    if (strncmp(out, "/fs", 3) != 0 || (out[3] != 0 && out[3] != '/')) return false;
    for (const char *p = out; (p = strstr(p, "..")) != nullptr; p += 2) {
        const bool starts = (p == out || p[-1] == '/');
        const bool ends   = (p[2] == 0 || p[2] == '/');
        if (starts && ends) return false;
    }
    size_t len = strlen(out);                  // "/fs/sys/" names the same as "/fs/sys"
    while (len > 3 && out[len - 1] == '/') out[--len] = 0;
    return true;
}

static int button_index(const char *name)
{
    static const char *const names[] = { "up", "down", "left", "right", "ok", "back" };
    for (int i = 0; i < 6; ++i) if (strcmp(name, names[i]) == 0) return i;
    return -1;
}

#define BTN_INDEX_DOWN_SH 1
#define BTN_INDEX_BACK_SH 5

static void publish(int button, uint8_t kind)
{
    evbus_publish(EV_INPUT, EV_SRC_SHELL, kind, (uint32_t)button);
}

// --- commands ----------------------------------------------------------------------

// The kernel's tasks, with CPU (stage 5.3): the share of core 1 each has had
// since the previous ps - or since boot, the first time - and its total.
static k_cpu_time_t s_ps_core;
static uint64_t     s_ps_cycles[K_MAX_TASKS];
static uint32_t     s_ps_gen[K_MAX_TASKS];

static const char *state_word(k_state_t s)
{
    switch (s) {
        case K_READY:    return "ready";
        case K_RUNNING:  return "run";
        case K_SLEEPING: return "sleep";
        case K_BLOCKED:  return "wait";
        case K_DEAD:     return "dead";
        case K_ZOMBIE:   return "zombie";
        default:         return "-";
    }
}

static void cmd_ps(int, char **)
{
    static k_task_info_t info[K_MAX_TASKS];        // 1.3 KB: not on this stack
    const int n = k_task_snapshot(info, K_MAX_TASKS);
    k_cpu_time_t now;
    k_cpu_time(1, &now);
    const uint64_t all = (now.busy - s_ps_core.busy) + (now.idle - s_ps_core.idle) +
                         (now.park - s_ps_core.park);
    const uint64_t mhz = K_CPU_MHZ;                // CCOUNT runs at the CPU clock

    logf("  id name        state  core pri   cpu1%%  cpu ms  stack used/total\n");
    logf("  -- ----------- ------ ---- ---  ------ ------- ----------------\n");
    int dead = 0;
    uint32_t dead_bytes = 0;
    for (int i = 0; i < n; ++i) {
        const k_task_info_t *t = &info[i];
        if (t->state == K_DEAD) {
            // Ended. Its stack should be back in the heap (stage 5.3c); a
            // stack_used above 0 means one is still held.
            if (t->stack_used) {
                dead++;
                dead_bytes += t->stack_size + K_STACK_GUARD;
            }
            continue;
        }
        const uint64_t before = (s_ps_gen[t->id] == t->gen) ? s_ps_cycles[t->id] : 0;
        const uint32_t pm = all ? (uint32_t)((t->cycles - before) * 1000 / all) : 0;
        char aff = (t->affinity == K_CPU_ANY) ? '*' : (char)('0' + t->affinity);
        logf("  %2u %-11s %-6s  %c   %2u  %3lu.%lu%% %7lu  %5lu/%5lu\n", t->id,
             t->name ? t->name : "?", state_word(t->state), aff, t->prio,
             (unsigned long)(pm / 10), (unsigned long)(pm % 10),
             (unsigned long)(t->cycles / (mhz * 1000)),
             (unsigned long)t->stack_used, (unsigned long)t->stack_size);
        s_ps_cycles[t->id] = t->cycles;
        s_ps_gen[t->id]    = t->gen;
    }
    if (all) {
        logf("  core 1: busy %lu.%lu%%, parked %lu.%lu%%, idle %lu.%lu%% over %lu ms\n",
             (unsigned long)((now.busy - s_ps_core.busy) * 1000 / all / 10),
             (unsigned long)((now.busy - s_ps_core.busy) * 1000 / all % 10),
             (unsigned long)((now.park - s_ps_core.park) * 1000 / all / 10),
             (unsigned long)((now.park - s_ps_core.park) * 1000 / all % 10),
             (unsigned long)((now.idle - s_ps_core.idle) * 1000 / all / 10),
             (unsigned long)((now.idle - s_ps_core.idle) * 1000 / all % 10),
             (unsigned long)(all / (mhz * 1000)));
    }
    s_ps_core = now;
    if (dead) {
        logf("  %d ended task(s) still holding their stacks: %lu bytes of kernel heap\n",
             dead, (unsigned long)dead_bytes);
    }
    logf("\n");
    logf("  id app          state             events  inputs  frames  queued dropped\n");
    logf("  -- ------------ --------------- -------- ------- ------- ------- -------\n");
    const int focus = app_focused();
    for (int i = 0; i < APP_MAX; ++i) {
        const app_ctx_t *c = app_get(i);
        if (!c) continue;
        const evbus_stats_t st = evbus_stats(c->sub);
        logf("  %2d %-12s %-15s %8lu %7lu %7lu %7d %7lu\n", i, c->app->name,
             c->killed ? "killed" : (c->stopping || c->stop_ms) ? "closing"
                 : (i == focus ? "running, focus" : "running"),
             (unsigned long)c->events, (unsigned long)c->inputs, (unsigned long)c->frames,
             evbus_pending(c->sub),
             (unsigned long)st.dropped);
    }
    logf("  %d app(s) open besides Home\n", app_user_count());
    // Stage 5.4d/5.4e: what the system has had to refuse, and what it did
    // not send. All three read zero on a board that has been left alone.
    logf("  frames: %lu sent, %lu taken while the screen was off; "
         "timers refused %lu; log batches lost %lu\n",
         (unsigned long)app_frames_flushed(), (unsigned long)app_frames_dark(),
         (unsigned long)app_timers_denied(), (unsigned long)slog_lost_batches());
}

// Stage 6.1: the boot screen, painted again over whatever is on the panel -
// for looking at it, and for "bootscreen" followed by "screenshot". The app in
// front draws over it at its next frame, so it is a picture, not a mode.
static void cmd_bootscreen(int, char **)
{
    boot_screen_preview(system_fb());
    logf("  boot screen painted; it was up %lu ms at boot, %lu steps, %lu frames\n",
         (unsigned long)boot_screen_ms(), (unsigned long)boot_steps(),
         (unsigned long)boot_frames());
}

// Stage 6.3: the radio. It is not up at boot - it costs internal RAM and a
// launcher does not need it - so "wifi up" is how it starts, and "wifi scan"
// is how what is on the air gets measured. With no argument it says what the
// silicon can do, which needs no radio at all.
static void cmd_wifi(int argc, char **argv)
{
    const char *what = argc > 1 ? argv[1] : "";

    if (strcmp(what, "up") == 0) {
        const bool ok = radio_up();
        logf("  radio %s; %u bytes of internal RAM, %u KB free now\n",
             ok ? "up" : "DID NOT START", (unsigned)radio_ram_cost(),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024));
        return;
    }
    if (strcmp(what, "down") == 0) {
        radio_down();
        logf("  radio down; %u KB of internal RAM free\n",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024));
        return;
    }
    if (strcmp(what, "country") == 0 && argc > 2) {
        const bool ok = radio_set_country(argv[2]);
        logf("  channel plan: %s%s\n", argv[2], ok ? "" : " REFUSED");
        return;
    }
    if (strcmp(what, "scan") == 0) {
        if (!radio_is_up() && !radio_up()) { logf("  the radio will not start\n"); return; }
        static radio_ap_t ap[40];
        uint32_t ms = 0;
        const int n = radio_scan(ap, 40, &ms);
        if (n < 0) { logf("  scan failed\n"); return; }

        logf("  %d access point(s) in %lu ms\n", n, (unsigned long)ms);
        logf("  ch   MHz  rssi  auth    bssid              ssid\n");
        logf("  --  ----  ----  ------  -----------------  ------------------------\n");
        for (int i = 0; i < n; ++i) {
            // The BSSID, not the name: it is what identifies one radio across
            // two scans, which is what comparing antennas needs.
            logf("  %2u  %4lu  %4d  %-6s  %02x:%02x:%02x:%02x:%02x:%02x  %s\n", ap[i].channel,
                 (unsigned long)radio_channel_mhz(ap[i].channel), ap[i].rssi,
                 radio_auth_name(ap[i].auth),
                 ap[i].bssid[0], ap[i].bssid[1], ap[i].bssid[2],
                 ap[i].bssid[3], ap[i].bssid[4], ap[i].bssid[5],
                 ap[i].ssid[0] ? ap[i].ssid : "(hidden)");
        }

        // What each channel of the band actually carried, which is the
        // question an antenna is judged by.
        char cc[4] = "??";
        uint8_t first = 1, count = 13;
        radio_channels(cc, &first, &count);
        logf("  per channel (country %s, channels %u..%u):\n", cc, first, first + count - 1);
        for (int ch = first; ch < first + count; ++ch) {
            int seen = 0, best = -127;
            for (int i = 0; i < n; ++i) {
                if (ap[i].channel != ch) continue;
                seen++;
                if (ap[i].rssi > best) best = ap[i].rssi;
            }
            if (!seen) continue;
            logf("   %2d  %lu MHz  %d ap(s), strongest %d dBm\n", ch,
                 (unsigned long)radio_channel_mhz((uint8_t)ch), seen, best);
        }
        return;
    }

    radio_chip_t c;
    radio_chip(&c);
    logf("  %s rev %u.%u, %u core(s)\n", c.model, c.revision_major, c.revision_minor, c.cores);
    logf("  2.4 GHz Wi-Fi b/g/n: %s      5 GHz: %s\n", c.wifi_bgn ? "yes" : "no",
         c.wifi_5g ? "yes" : "no, not on this chip");
    logf("  Bluetooth LE: %s   classic: %s   802.15.4: %s\n", c.ble ? "yes" : "no",
         c.bt_classic ? "yes" : "no", c.ieee802154 ? "yes" : "no");
    logf("  MAC  wifi %02x:%02x:%02x:%02x:%02x:%02x  ap %02x:%02x:%02x:%02x:%02x:%02x  bt %02x:%02x:%02x:%02x:%02x:%02x\n",
         c.mac_sta[0], c.mac_sta[1], c.mac_sta[2], c.mac_sta[3], c.mac_sta[4], c.mac_sta[5],
         c.mac_ap[0], c.mac_ap[1], c.mac_ap[2], c.mac_ap[3], c.mac_ap[4], c.mac_ap[5],
         c.mac_bt[0], c.mac_bt[1], c.mac_bt[2], c.mac_bt[3], c.mac_bt[4], c.mac_bt[5]);

    if (!radio_is_up()) {
        logf("  radio is down (wifi up to start it, wifi scan to look around)\n");
        return;
    }
    char cc[4] = "??";
    uint8_t first = 1, count = 13;
    radio_channels(cc, &first, &count);
    const int p10 = radio_tx_power_dbm10();
    logf("  radio up: country %s, channels %u..%u (%lu..%lu MHz), max TX %d.%d dBm\n",
         cc, first, first + count - 1,
         (unsigned long)radio_channel_mhz(first),
         (unsigned long)radio_channel_mhz((uint8_t)(first + count - 1)),
         p10 / 10, p10 % 10);
}

static void cmd_free(int, char **)
{
    const uint32_t in_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    logf("  internal RAM  %6u KB free, %6u KB at the lowest, largest block %u KB\n",
         (unsigned)(heap_caps_get_free_size(in_caps) / 1024),
         (unsigned)(heap_caps_get_minimum_free_size(in_caps) / 1024),
         (unsigned)(heap_caps_get_largest_free_block(in_caps) / 1024));
    logf("  PSRAM         %6u KB free of %u KB\n",
         (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
         (unsigned)(heap_caps_get_total_size(MALLOC_CAP_SPIRAM) / 1024));
    for (int pl = 0; pl < KHEAP_POOLS; ++pl) {
        kheap_stats_t k;
        kheap_stats_pool((kheap_pool_t)pl, &k);
        logf("  kernel %-7s %7u B used of %u B, largest free %u B, %u blocks (%u free)\n",
             pl == KHEAP_INTERNAL ? "heap" : "psram", (unsigned)k.used, (unsigned)k.total,
             (unsigned)k.largest_free, (unsigned)k.blocks, (unsigned)k.free_blocks);
    }
    logf("  one app may take %u B of it (stack and state); the bus has %d of %d slots free\n",
         (unsigned)APP_KERNEL_MAX, evbus_subs_free(), EVBUS_MAX_SUBS);
}

static void cmd_df(int, char **)
{
    if (!fs_mounted()) { logf("  no filesystem mounted\n"); return; }
    size_t total = 0, used = 0;
    fs_info(&total, &used);
    logf("  /fs  %u KB used of %u KB (%u%%), %u KB free\n", (unsigned)(used / 1024),
         (unsigned)(total / 1024), total ? (unsigned)(used * 100 / total) : 0u,
         (unsigned)((total - used) / 1024));
}

static void cmd_log(int argc, char **argv)
{
    int n = 20;
    if (argc > 1) {
        char *end;
        n = (int)strtol(argv[1], &end, 10);
        if (*end || n < 1 || n > 200) { logf("  log [1..200]\n"); return; }
    }
    const size_t max = 16 * 1024;
    char *buf = (char *)malloc(max);
    if (!buf) { logf("  out of memory\n"); return; }
    const size_t len = slog_tail_local(buf, max);

    // Back from the end to the start of the n-th last line.
    size_t start = len;
    int lines = 0;
    while (start > 0) {
        if (buf[start - 1] == '\n' && start != len && ++lines == n) break;
        start--;
    }
    logf("%s", buf + start);
    free(buf);
}

static void cmd_ls(int argc, char **argv)
{
    char path[80];
    if (!fs_path(argc > 1 ? argv[1] : "/fs", path, sizeof(path))) {
        logf("  only paths under /fs\n");
        return;
    }
    DIR *d = opendir(path);
    if (!d) { logf("  %s: no such directory\n", path); return; }
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != nullptr) {
        char full[80 + 34];
        snprintf(full, sizeof(full), "%s/%.32s", path, e->d_name);
        struct stat st;
        if (stat(full, &st) == 0 && S_ISDIR(st.st_mode)) {
            logf("  %8s  %s/\n", "", e->d_name);
        } else {
            logf("  %8ld  %s\n", stat(full, &st) == 0 ? (long)st.st_size : -1L, e->d_name);
        }
        n++;
    }
    closedir(d);
    if (!n) logf("  (empty)\n");
}

static void cmd_cat(int argc, char **argv)
{
    char path[80];
    if (argc < 2) { logf("  cat <path>\n"); return; }
    if (!fs_path(argv[1], path, sizeof(path))) { logf("  only paths under /fs\n"); return; }
    if (strcmp(path, "/fs/sys/log.txt") == 0) slog_flush();   // what is waiting too
    FILE *f = fopen(path, "r");
    if (!f) { logf("  %s: cannot open\n", path); return; }
    char buf[256];
    size_t got;
    bool newline = true;
    while ((got = fread(buf, 1, sizeof(buf), f)) > 0) {
        logf("%.*s", (int)got, buf);
        newline = (buf[got - 1] == '\n');
    }
    fclose(f);
    if (!newline) logf("\n");
}

static void cmd_open(int argc, char **argv)
{
    if (argc < 2) { logf("  open <app>   (see: ps, or help for the list)\n"); return; }
    const app_t *a = app_find(argv[1]);
    if (!a) { logf("  no app called '%s'\n", argv[1]); return; }
    int id = app_running(a);
    const bool was = (id >= 0);
    if (!was) id = app_start(a);
    if (id < 0) { logf("  could not start %s\n", a->name); return; }
    app_focus(id);
    logf("  %s %s, id %d, now in front\n", a->name, was ? "was already open" : "opened", id);
    slog("shell", "open %s (id %d)", a->name, id);
}

static void cmd_close(int argc, char **argv)
{
    if (argc < 2) { logf("  close <app>\n"); return; }
    const app_t *a = app_find(argv[1]);
    if (!a) { logf("  no app called '%s'\n", argv[1]); return; }
    if (a->flags & APP_FLAG_SYSTEM) { logf("  %s is part of the system\n", a->name); return; }
    const int id = app_running(a);
    if (id < 0) { logf("  %s is not open\n", a->name); return; }
    app_stop(id);
    logf("  asked %s (id %d) to close - it exits at its next event\n", a->name, id);
    slog("shell", "close %s (id %d)", a->name, id);
}

// --- stage 7: installing an app -------------------------------------------
//
// The file arrives as base64, in lines, because the shell is a line protocol
// and tools/shell_test.py already drives it. A binary mode would be faster
// and would cost the whole of that: an app is a few hundred bytes, and a few
// hundred bytes of base64 over USB is not worth a second protocol.
//
// ไฟล์มาเป็น base64 ทีละบรรทัด เพราะเชลล์เป็นโปรโตคอลบรรทัด และเครื่องมือ
// ทดสอบที่มีอยู่ขับมันได้ทันที โหมดไบนารีจะเร็วกว่าแต่ต้องทิ้งเครื่องมือทั้งชุด

static FILE    *s_rx;
static char     s_rx_name[APPLOAD_NAME_MAX];
static uint32_t s_rx_left;

static int b64val(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return (c == '=') ? -2 : -1;
}

bool shell_rx_active(void) { return s_rx != nullptr; }

// One line while an install is in progress. Returns false if there is no
// install going on, in which case the line is an ordinary command.
bool shell_rx_line(const char *line)
{
    if (!s_rx) return false;

    if (strcmp(line, "end") == 0) {
        fclose(s_rx);
        s_rx = nullptr;
        if (s_rx_left != 0) {
            logf("  %lu byte(s) short - not installed\n", (unsigned long)s_rx_left);
            return true;
        }
        // Installing over something already installed is how a newer
        // version arrives, so the old one is forgotten first rather than
        // the new one being refused. appload_forget says no if the app is
        // open, and then so does the load.
        const bool was = (appload_forget(s_rx_name) || true);
        (void)was;
        if (appload_one(s_rx_name)) {
            logf("  %s installed; it is in Home now\n", s_rx_name);
            slog("shell", "installed %s", s_rx_name);
        } else {
            logf("  refused: %s\n", appload_last_error());
        }
        return true;
    }

    uint8_t out[3];
    int have = 0, bits = 0, acc = 0;
    for (const char *c = line; *c; ++c) {
        const int v = b64val(*c);
        if (v == -1) continue;              // whitespace and the like
        if (v == -2) break;                 // padding: the end of the data
        acc = (acc << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out[have++] = (uint8_t)((acc >> bits) & 0xFF);
            if (have == 3) {
                const uint32_t n = (s_rx_left < 3) ? s_rx_left : 3;
                fwrite(out, 1, n, s_rx);
                s_rx_left -= n;
                have = 0;
            }
        }
    }
    if (have) {
        const uint32_t n = (s_rx_left < (uint32_t)have) ? s_rx_left : (uint32_t)have;
        fwrite(out, 1, n, s_rx);
        s_rx_left -= n;
    }
    return true;
}

static void cmd_install(int argc, char **argv)
{
    if (argc < 3) {
        logf("  install <name> <bytes>, then base64 lines, then a line: end\n");
        return;
    }
    if (s_rx) { fclose(s_rx); s_rx = nullptr; }     // an install that stalled

    const uint32_t n = (uint32_t)strtoul(argv[2], nullptr, 10);
    if (n == 0 || n > 65536) { logf("  that size is not believable\n"); return; }

    mkdir(APPLOAD_DIR, 0775);
    char path[96];
    snprintf(path, sizeof(path), APPLOAD_DIR "/%s" APPLOAD_EXT, argv[1]);
    s_rx = fopen(path, "wb");
    if (!s_rx) { logf("  cannot write %s\n", path); return; }

    snprintf(s_rx_name, sizeof(s_rx_name), "%s", argv[1]);
    s_rx_left = n;
    logf("  send %lu bytes as base64 lines, then a line saying: end\n", (unsigned long)n);
}

static void cmd_uninstall(int argc, char **argv)
{
    if (argc < 2) { logf("  uninstall <app>\n"); return; }
    if (!appload_forget(argv[1])) { logf("  %s\n", appload_last_error()); return; }
    char path[96];
    snprintf(path, sizeof(path), APPLOAD_DIR "/%s" APPLOAD_EXT, argv[1]);
    unlink(path);
    logf("  %s uninstalled, and its file is gone\n", argv[1]);
    slog("shell", "uninstalled %s", argv[1]);
}

static void cmd_apps(int, char **)
{
    logf("  %d installed app(s), %lu B of the kernel pool\n",
         appload_count(), (unsigned long)appload_bytes());
    for (int i = 0; i < appload_count(); ++i) {
        logf("    %s\n", appload_name_at(i));
    }
}

static void cmd_kill(int argc, char **argv)
{
    if (argc < 2) { logf("  kill <app>   (close asks; kill does not)\n"); return; }
    const app_t *a = app_find(argv[1]);
    if (!a) { logf("  no app called '%s'\n", argv[1]); return; }
    const int id = app_running(a);
    if (id < 0) { logf("  %s is not open\n", a->name); return; }
    const int r = app_kill(id);
    switch (r) {
        case K_KILL_DONE:    logf("  %s (id %d) killed\n", a->name, id); break;
        case K_KILL_PENDING: logf("  %s (id %d) killed - it was running; gone at its next switch\n",
                                  a->name, id); break;
        case K_KILL_GONE:    logf("  %s (id %d) had already ended\n", a->name, id); break;
        case -2:             logf("  %s is part of the system\n", a->name); break;
        default:             logf("  %s (id %d) cannot be killed now (%d)\n", a->name, id, r); break;
    }
    if (r == K_KILL_DONE || r == K_KILL_PENDING) slog("shell", "kill %s (id %d)", a->name, id);
}

static void cmd_set(int argc, char **argv)
{
    if (argc == 1) {
        for (int i = 0; i < settings_count(); ++i) {
            const char *k = settings_key_at(i);
            int32_t lo = 0, hi = 0;
            settings_range(k, &lo, &hi);
            logf("  %-15s %ld   (%ld..%ld)\n", k, (long)settings_get(k), (long)lo, (long)hi);
        }
        return;
    }
    int32_t lo, hi;
    if (!settings_range(argv[1], &lo, &hi)) { logf("  no setting called '%s'\n", argv[1]); return; }
    if (argc < 3) { logf("  %s = %ld\n", argv[1], (long)settings_get(argv[1])); return; }
    char *end;
    const long v = strtol(argv[2], &end, 10);
    if (*end || end == argv[2]) { logf("  '%s' is not a number\n", argv[2]); return; }
    settings_set(argv[1], (int32_t)v);
    const int32_t now = settings_get(argv[1]);
    logf("  %s = %ld%s\n", argv[1], (long)now,
         now != v ? "   (clamped to its range)" : "");
    slog("shell", "set %s %ld", argv[1], (long)now);
}

static void cmd_key(int argc, char **argv)
{
    if (argc < 2) { logf("  key <up|down|left|right|ok|back>...\n"); return; }
    int b[MAX_ARGS];
    for (int i = 1; i < argc; ++i) {           // all or nothing
        b[i] = button_index(argv[i]);
        if (b[i] < 0) { logf("  no button called '%s'\n", argv[i]); return; }
    }
    for (int i = 1; i < argc; ++i) {
        publish(b[i], INPUT_PRESS);
        publish(b[i], INPUT_RELEASE);
    }
    logf("  %d key(s) sent\n", argc - 1);
}

static void cmd_hold(int argc, char **argv)
{
    const int b = argc > 1 ? button_index(argv[1]) : -1;
    if (b < 0) { logf("  hold <up|down|left|right|ok|back>\n"); return; }
    // What a finger held past INPUT_REPEAT_DELAY_MS produces: press, the first
    // repeat, release. The first repeat is what "hold BACK" acts on.
    publish(b, INPUT_PRESS);
    publish(b, INPUT_REPEAT);
    publish(b, INPUT_RELEASE);
    logf("  held %s\n", argv[1]);
}

// The panel's framebuffer, as hex: "SHOT w h", one line per row of 4-digit
// pixels in the buffer's own (wire) byte order, then "END". tools/screenshot.py
// turns it into a PNG - how a UI gets checked by eye without the board in view.
static const fb_t *s_screen;

void shell_set_screen(const fb_t *fb) { s_screen = fb; }

static void cmd_screenshot(int, char **)
{
    if (!s_screen || !s_screen->px) { logf("  no screen\n"); return; }
    const fb_t *fb = s_screen;
    static const char hex[] = "0123456789abcdef";
    char *row = (char *)malloc((size_t)fb->w * 4 + 2);
    if (!row) { logf("  out of memory\n"); return; }
    logf("SHOT %u %u\n", fb->w, fb->h);
    for (int y = 0; y < fb->h; ++y) {
        const uint16_t *p = fb->px + (size_t)y * fb->w;
        char *o = row;
        for (int x = 0; x < fb->w; ++x) {
            const uint16_t v = p[x];
            *o++ = hex[(v >> 12) & 15];
            *o++ = hex[(v >> 8) & 15];
            *o++ = hex[(v >> 4) & 15];
            *o++ = hex[v & 15];
        }
        *o++ = '\n';
        *o = 0;
        logf("%s", row);
    }
    logf("END\n");
    free(row);
}

static void cmd_min(int, char **)
{
    // What a finger does: hold DOWN, press BACK, let go of both. The app
    // manager reads it as the minimise chord (stage 5.5).
    publish(BTN_INDEX_DOWN_SH, INPUT_PRESS);
    publish(BTN_INDEX_BACK_SH, INPUT_PRESS);
    publish(BTN_INDEX_BACK_SH, INPUT_RELEASE);
    publish(BTN_INDEX_DOWN_SH, INPUT_RELEASE);
    logf("  DOWN + BACK sent: the app in front goes to the background\n");
}

static void cmd_reboot(int, char **)
{
    logf("  rebooting\n");
    slog("sys", "reboot from the shell");   // "sys": written to flash at once
    esp_restart();
}

static const cmd_t s_cmds[] = {
    { "help",   "",              "this list",                                  cmd_help   },
    { "ps",     "",              "kernel tasks, then apps",                    cmd_ps     },
    { "free",   "",              "internal RAM, PSRAM, the kernel heap",       cmd_free   },
    { "bootscreen", "",          "paint the boot screen again, to look at it", cmd_bootscreen },
    { "wifi",   "[up|down|scan|country XX]", "the radio, and what is on the air", cmd_wifi },
    { "df",     "",              "the filesystem",                             cmd_df     },
    { "log",    "[n]",           "the last n lines of the system log (20)",    cmd_log    },
    { "ls",     "[path]",        "files under /fs",                            cmd_ls     },
    { "cat",    "path",          "a file under /fs",                           cmd_cat    },
    { "open",   "app",           "open an app and bring it to the front",      cmd_open   },
    { "close",  "app",           "ask an app to close (killed if it ignores that)", cmd_close },
    { "install","name bytes",    "install an app: base64 lines, then 'end'",   cmd_install },
    { "uninstall","app",         "remove an installed app and its file",       cmd_uninstall },
    { "apps",   "",              "apps the user installed",                    cmd_apps   },
    { "kill",   "app",           "end an app now, whatever it is doing",       cmd_kill   },
    { "set",    "[key value]",   "list the settings, or change one",           cmd_set    },
    { "key",    "btn...",        "tap buttons: up down left right ok back",    cmd_key    },
    { "hold",   "btn",           "press and hold a button",                    cmd_hold   },
    { "min",    "",              "minimise the app in front (DOWN + BACK)",    cmd_min    },
    { "screenshot", "",          "the screen, as hex (tools/screenshot.py)",   cmd_screenshot },
    { "reboot", "",              "restart the board",                          cmd_reboot },
};

static void cmd_help(int, char **)
{
    for (const cmd_t &c : s_cmds) logf("  %-7s %-12s %s\n", c.name, c.args, c.help);
    logf("  apps:");
    for (int i = 0; i < app_registry_count(); ++i) logf(" %s", app_registry_at(i)->name);
    logf("\n");
}

// --- the line --------------------------------------------------------------------------

static void run(char *line)
{
    // Stage 7: while an app is being installed, a line is a line of its
    // file rather than a command. Checked before anything is parsed, so a
    // line of base64 that happens to start with a command's name is still
    // just data.
    if (shell_rx_line(line)) return;

    char *argv[MAX_ARGS];
    int argc = 0;
    for (char *p = strtok(line, " \t"); p; p = strtok(nullptr, " \t")) {
        if (argc == MAX_ARGS) { logf("  too many words (at most %d)\n", MAX_ARGS); return; }
        argv[argc++] = p;
    }
    if (!argc) return;
    s_commands++;
    for (const cmd_t &c : s_cmds) {
        if (strcmp(argv[0], c.name) == 0) { c.fn(argc, argv); return; }
    }
    logf("  unknown command '%s' - try help\n", argv[0]);
}

void shell_init(void)
{
    logf("[shell] type help and press Enter\n" PROMPT);
    push();
}

void shell_poll(void)
{
    uint8_t in[32];
    uint32_t n;
    while ((n = usb_serial_jtag_ll_read_rxfifo(in, sizeof(in))) > 0) {
        for (uint32_t i = 0; i < n; ++i) {
            const char ch = (char)in[i];
            const char last = s_last;
            s_last = ch;

            if (ch == '\r' || ch == '\n') {
                if (ch == '\n' && last == '\r') continue;     // one Enter, not two
                logf("\n");
                bool was_data = false;
                if (s_overflow) {
                    logf("  line too long (at most %d characters)\n", SHELL_LINE - 1);
                } else {
                    s_line[s_len] = 0;
                    was_data = shell_rx_active();
                    run(s_line);
                }
                s_len = 0;
                s_overflow = false;
                // Stage 7: no prompt for a line that was part of a file.
                //
                // A prompt per line is right for a person typing and wrong
                // for a program streaming: the sender would have to read one
                // back after every line, or they pile up and it matches an
                // old one instead of the answer it is waiting for. Found
                // that way on the first install.
                //
                // The condition is "was this line data", not "is a transfer
                // going on". The install command itself and the "end" that
                // closes one are commands and get their prompt - suppressing
                // those was the second thing found, when the sender then
                // waited for a prompt that was never coming.
                if (!was_data || !shell_rx_active()) logf(PROMPT);
                push();
            } else if (ch == '\b' || ch == 0x7F) {
                if (s_len && !s_overflow) { s_len--; logf("\b \b"); push(); }
            } else if (ch >= ' ' && ch < 0x7F) {
                if (s_len < SHELL_LINE - 1) {
                    s_line[s_len++] = ch;
                    if (!shell_rx_active()) { logf("%c", ch); push(); }
                }
                else s_overflow = true;
            }
            // anything else - escape sequences, arrows - is ignored
        }
    }
}

uint32_t shell_commands(void) { return s_commands; }
