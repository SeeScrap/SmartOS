// ---------------------------------------------------------------------------
// Project S3 - stage 2.0: same bring-up, now on bare ESP-IDF
//
// Nothing about what this program checks has changed. What changed is who is
// underneath it: the Arduino core is gone, so sdkconfig is ours. FreeRTOS is
// pinned to core 0 (CONFIG_FREERTOS_UNICORE) and core 1 is never started,
// which is what stage 2.1 will claim for the kernel.
// ---------------------------------------------------------------------------
// Builds that replace the kernel's default task set with a test of their own.
// Apps are not started in these: several of the tests need core 1 to
// themselves, and none of them create the app manager.
#if defined(S3_PREEMPT_SOAK) || defined(S3_TEST_SLEEP_CLAIM) ||     defined(S3_TEST_MBOX_STRESS) || defined(S3_TEST_EVBUS) ||     defined(S3_TEST_PARK) || defined(S3_TEST_PARK_OFF)
#define S3_KERNEL_TEST_BUILD 1
#endif

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_chip_info.h"
#include "esp_cpu.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_partition.h"
#include "esp_psram.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_mac.h"
#include "soc/rtc_cntl_reg.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "board/devkitc_n16r8.h"
#include "s3hal/hal_time.h"
#include "s3hal/input.h"
#include "s3hal/lcd_bus.h"
#include "gfx/fb.h"
#include "gfx/text.h"
#include "os/evbus.h"
#include "os/app.h"
#include "os/boot.h"
#include "apps/apps.h"
#include "os/dev.h"
#include "os/fs.h"
#include "os/screen.h"
#include "os/settings.h"
#include "boot_gfx.h"
#include "system.h"
#include "tests/harness.h"
#include "tests/tests.h"
#include "os/shell.h"
#include "os/sysinfo.h"
#include "os/syslog.h"
#include "os/kfmt.h"
#include "os/krand.h"
#include "s3hal/lcd_panel.h"
#include "s3hal/lcd_probe.h"
#include "s3hal/log.h"

#include "kernel/kcore1.h"
#include "kernel/kfault.h"

extern "C" volatile uint32_t k_chain_end_hit;
extern "C" volatile uint32_t k_spin_stuck;
#include "kernel/kheap.h"
#include "kernel/kmbox.h"
#include "kernel/ksync.h"
#include "kernel/ktask.h"
#include "kernel/ktime.h"
#include "kernel/kpark.h"
#include "kernel/klog.h"


// Why the chip last reset, in words. Used by the boot report on the console
// and, since stage 4.5, by the first line each boot writes to the system log.
static const char *reset_reason_name(void)
{
    const char *why = "?";
    switch (esp_reset_reason()) {
        case ESP_RST_POWERON:  why = "power-on";                       break;
        case ESP_RST_EXT:      why = "external reset pin";             break;
        case ESP_RST_SW:       why = "software restart";               break;
        case ESP_RST_PANIC:    why = "panic / exception";              break;
        case ESP_RST_INT_WDT:  why = "interrupt watchdog";             break;
        case ESP_RST_TASK_WDT: why = "task watchdog";                  break;
        case ESP_RST_WDT:      why = "other watchdog";                 break;
        case ESP_RST_BROWNOUT: why = "BROWNOUT - the supply sagged!";  break;
        case ESP_RST_SDIO:     why = "SDIO";                           break;
        case ESP_RST_DEEPSLEEP:why = "deep sleep wake";                break;
        // Stage 4.5 found these missing: the USB one is what an upload or a
        // serial monitor's reset looks like, and it read "unknown" in the log.
        case ESP_RST_USB:      why = "USB (upload or serial monitor)"; break;
        case ESP_RST_JTAG:     why = "JTAG";                           break;
        case ESP_RST_EFUSE:    why = "efuse error";                    break;
        case ESP_RST_PWR_GLITCH: why = "power glitch";                 break;
        case ESP_RST_CPU_LOCKUP: why = "CPU lock-up (double exception)"; break;
        default:               why = "unknown";                        break;
    }
    return why;
}

static void report_memory(void)
{
    esp_chip_info_t info;
    esp_chip_info(&info);

    uint32_t flash_sz = 0;
    esp_flash_get_size(NULL, &flash_sz);

    logln();
    logln("=== Project S3 / bring-up (ESP-IDF) ===");
    logf("chip      : ESP32-S3 rev %d, %d core(s) @ %d MHz\n",
         info.revision, info.cores, CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ);
    logf("flash     : %lu bytes\n", (unsigned long)flash_sz);
    logf("internal  : %lu bytes free\n", (unsigned long)esp_get_free_heap_size());

    size_t psram = esp_psram_get_size();
    logf("PSRAM     : %lu bytes\n", (unsigned long)psram);
    if (psram < 4u * 1024 * 1024) {
        logln("  !! expected ~8 MB. Check CONFIG_SPIRAM_MODE_OCT in sdkconfig.defaults");
    }

    // The whole point of stage 2.0: prove FreeRTOS is confined to one core.
    // Power-supply forensics. A rail that is sagging leaves fingerprints here
    // long before it stops the board outright: the brownout detector trips at
    // roughly 2.5 V on the 3.3 V rail and forces a reset, and flash and PSRAM
    // are the first things to misbehave when they are underfed.
    const char *why = reset_reason_name();
    logf("reset cause: %s\n", why);

    logf("running on: core %d\n", (int)xPortGetCoreID());
#if CONFIG_FREERTOS_UNICORE
    logln("core 1    : asleep, never started by IDF  <-- ours to claim in 2.1");
#else
    logln("core 1    : !! FreeRTOS owns it - CONFIG_FREERTOS_UNICORE is not set");
#endif
}

// The stage 2 kernel demo - every task that used to live here - is now in
// tests/test_kernel.cpp. core1_main below starts it with one call.





// --- stage 2.6b: core 0 joins the same scheduler ---------------------------

// A kernel task pinned to core 0 whose entire job is to give the slice back.
//
// Core 0 still owes FreeRTOS its USB console, its flash driver and the mailbox
// worker. Donating the core to our scheduler outright would starve all three,
// so the kernel keeps one task there that hands the time straight back and then
// asks for it again. It is the price of borrowing a core that already has a job.
static volatile DRAM_ATTR uint32_t s_cpu0_loops;

static void IRAM_ATTR cpu0_idle_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(1);      // FreeRTOS gets core 0 for a tick
        s_cpu0_loops = s_cpu0_loops + 1;

        k_yield();          // then our scheduler gets first refusal again
    }
}


// The FreeRTOS task that donates core 0. Pinned there, and at a high priority
// so that the donation actually happens rather than waiting behind IDF's
// housekeeping.
static void smp_host_task(void *arg)
{
    (void)arg;
    logln("[core0] joining the kernel's scheduler");
    for (;;) {
        k_smp_join();       // returns only while there is nothing it may run
        vTaskDelay(1);
    }
}

static void IRAM_ATTR core1_main(void)
{
    klog("[core1] vector table is ours, faults are recorded not silent\n");

#ifdef S3_TEST_CORE1_FAULT
    klog("[core1] deliberately dereferencing NULL to test the fault path\n");
    volatile uint32_t *boom = (volatile uint32_t *)0;
    (void)*boom;
    klog("[core1] !! still running - the fault path did NOT trigger\n");
#endif

    klog("[core1] bringing up the kernel\n");
    k_init();                       // also brings up the kernel's own heap
    kpark_init_core1();             // before the tick: interrupts open with it
    k_tick_start(K_TICK_HZ);        // CCOMPARE0, 1 ms, this core only

#ifdef S3_PREEMPT_SOAK
    // Bisection build: the smallest thing that can prove preemption works.
    kernel_soak_start();        // tests/test_kernel.cpp
#elif defined(S3_TEST_PARK) || defined(S3_TEST_PARK_OFF)
    // Flash-resident code on core 1, pinned there, never yielding - the
    // workload that needs the park. Preemption keeps the kernel's own tasks
    // running around it.
    kernel_park_work_start();       // tests/test_park.cpp
#elif defined(S3_TEST_EVBUS)
    kernel_evbus_stress_start();    // tests/test_kernel_stress.cpp
#ifdef S3_ENABLE_SMP
    k_task_create_on("cpu0idle", cpu0_idle_task, nullptr, 0);
#endif
#elif defined(S3_TEST_MBOX_STRESS)
    kernel_mbox_stress_start();     // tests/test_kernel_stress.cpp
#ifdef S3_ENABLE_SMP
    k_task_create_on("cpu0idle", cpu0_idle_task, nullptr, 0);
#endif
#elif defined(S3_TEST_SLEEP_CLAIM)
    kernel_claim_start();           // tests/test_kernel_stress.cpp
#ifdef S3_ENABLE_SMP
    k_task_create_on("cpu0idle", cpu0_idle_task, nullptr, 0);
#endif
#else
    // Created first, on purpose. The table holds K_MAX_TASKS (16) and the demo
    // below already fills 15, so a debug flag that adds one more would silently
    // lose whichever task came last. Losing a demo task is a shorter log;
    // losing this one is a dead keypad.
    //
    // Pinned to core 1 - the kernel's own core, sampling on the kernel's own
    // tick - and HIGH, because it sleeps nearly all the time and has to run
    // on time when it wakes.
    if (k_task_create_pri("input", input_task, nullptr, 1, K_PRIO_HIGH) < 0) {
        klog("[core1] !! no task slot for the input sampler\n");
    }
    // The event bus's timers. Same reasoning: a system service, on the
    // kernel's core, and HIGH so a timer fires on time rather than when the
    // demo tasks happen to let it.
    if (k_task_create_pri("appmgr", app_manager_task, nullptr, 1, K_PRIO_HIGH) < 0) {
        klog("[core1] !! no task slot for the app manager\n");
    }
    if (k_task_create_pri("evtimer", evbus_timer_task, nullptr, 1, K_PRIO_HIGH) < 0) {
        klog("[core1] !! no task slot for the event bus timers\n");
    }
    kernel_demo_start();            // tests/test_kernel.cpp
#ifdef S3_ENABLE_SMP
    // Pinned to core 0 so core 1 never picks it up - and deliberately at
    // NORMAL, not a low priority. It is the only thing handing core 0 back to
    // FreeRTOS, so starving it would take the USB console and the flash driver
    // down with it. "Idle" is its job, not its rank.
    //
    // Created here, between the demo and its SMP pair, because that is the
    // order it has always been created in and task ids follow creation order.
    k_task_create_on("cpu0idle", cpu0_idle_task, nullptr, 0);
    kernel_smp_demo_start();
#endif
#endif

    // From here the tick does not merely ask for a reschedule, it performs one.
    // On by default now. A task that never yields no longer owns the core: the
    // demo's hog task spins for 200 ms without a single yield while the ticker
    // keeps printing every 25 ms right through it.
#ifdef S3_ENABLE_PREEMPT
    k_preempt_enable();
#endif

    klog("[core1] starting the scheduler - nothing below this line is IDF\n");
    k_start();

    klog("[core1] scheduler returned (it should not)\n");
}

static void wake_core1(void)
{
    logln();
    logln("--- stage 2.1-2.3: core 1 and the kernel ---");

    kmbox_init();                   // core 0 owns setting this up
    // The kernel's memory, taken from ESP-IDF here - on core 0, before core 1
    // exists - never by the kernel itself (stage 5.3c; see kheap_reserve).
    if (!kheap_reserve()) {
        logln("[core0] !! could not reserve the kernel's memory pools");
    } else {
        // Stage 6.3: how much it got, because since the radio was linked in it
        // is not always the whole KHEAP_INTERNAL_SIZE.
        kheap_stats_t in, ps;
        kheap_stats_pool(KHEAP_INTERNAL, &in);
        kheap_stats_pool(KHEAP_PSRAM, &ps);
        logf("[core0] kernel pools: %u KB internal (asked %u), %u KB PSRAM\n",
             (unsigned)((in.total + in.blocks * 16) / 1024),
             (unsigned)(KHEAP_INTERNAL_SIZE / 1024),
             (unsigned)((ps.total + ps.blocks * 16) / 1024));
    }
    bool ok = kcore1_start(core1_main, 500);

    // Core 1 prints through the ROM console with no locking, so give its
    // output a moment to drain before core 0 talks over it.
    hal_delay_ms(20);

    if (ok) {
        logln("[core0] core 1 answered - it is running our code now");
#ifdef S3_ENABLE_SMP
        // Donate core 0 straight away, while there is still a demo to share.
        // Pinned to core 0 and high priority so the donation actually happens
        // instead of queueing behind IDF's housekeeping.
        xTaskCreatePinnedToCore(smp_host_task, "s3os_host", 4096, nullptr,
                                10, nullptr, 0);
#endif
        // Serve core 1's mailbox while its demo runs, and watch for a fault.
        for (int i = 0; i < 400; ++i) {
            kmbox_service();
            if (k_chain_end_hit) {
                logln("[core0] !! core 1 unwound its register-window chain off the");
                logln("        bottom of a task stack - k_stack_init got it wrong");
                break;
            }
            if (kfault_pending()) { kfault_report(); break; }
            hal_delay_ms(2);
        }
#ifdef S3_ENABLE_SMP
        logf("[core0] cpu0idle asked the scheduler %lu times; spin stuck on 0x%08lx\n",
             (unsigned long)s_cpu0_loops, (unsigned long)k_spin_stuck);
#endif
        logf("[core0] preemption trace (%lu entries):\n",
             (unsigned long)k_trace_count());
        for (uint32_t i = 0; i < k_trace_count(); ++i) {
            const k_trace_t *e = k_trace_at(i);
            if (!e) break;
            logf("   #%2lu t=%-4lu pc=0x%08lx sp=0x%08lx task=0x%08lx hold=%lu state=%lu\n",
                 (unsigned long)i, (unsigned long)e->tick,
                 (unsigned long)e->pc, (unsigned long)e->sp,
                 (unsigned long)e->task, (unsigned long)e->hold,
                 (unsigned long)e->state);
        }

        uint32_t mb_e = 0, mb_p = 0, mb_d = 0, mb_b = 0;
        kmbox_census(&mb_e, &mb_p, &mb_d, &mb_b);
        logf("[core0] served %lu job(s); slots now empty=%lu posted=%lu done=%lu busy=%lu\n",
             (unsigned long)kmbox_served_count(),
             (unsigned long)mb_e, (unsigned long)mb_p, (unsigned long)mb_d,
             (unsigned long)mb_b);
        // The tick lives entirely inside core 1, so the only proof it is
        // running is watching its counter move from over here.
        uint32_t t1 = k_tick_count;
        hal_delay_ms(100);
        logf("[core0] core 1 tick counter: %lu -> %lu over 100 ms (%s)\n",
             (unsigned long)t1, (unsigned long)k_tick_count,
             (k_tick_count > t1) ? "CCOMPARE0 interrupt is firing"
                                  : "NOT ticking - the timer interrupt never fired");
    } else {
        logln("[core0] !! core 1 never reported in.");
        logln("        The wake sequence did not take - stop here rather than");
        logln("        building the kernel on a core that is not actually ours.");
    }
}

// The panel, its bring-up and the benchmark are in boot_gfx.cpp.
// Core 0's loop, the status bar and system_pass are in system.cpp.

// The park and its two tests are in tests/test_park.cpp.


extern "C" void app_main(void)
{
    log_init();
    hal_delay_ms(1000);                // let the USB console attach

    report_memory();
    alloc_framebuffer();

    logln();
    lcd_ctrl_t ctrl = lcd_probe_run();
#ifdef LCD_FORCE_CTRL
    ctrl = LCD_FORCE_CTRL;   // set in platformio.ini when the panel cannot be read
#endif
    logf("[lcd] controller: %s\n", lcd_ctrl_name(ctrl));

    esp_err_t err = lcd_bus_init(LCD_SPI_HZ);
    if (err != ESP_OK) {
        logf("[lcd] SPI bus init failed: %d\n", (int)err);
        return;
    }
    if (!lcd_panel_init(ctrl)) return;

    lcd_set_rotation(1);               // landscape 480x320
    // Stage 6.1: the backlight stays down until the boot screen. The
    // benchmark and the graphics self-tests below still draw and still flush
    // over the same SPI at the same cost - there is just nobody watching.
    lcd_backlight_pct(0);
    logf("[lcd] up: %ux%u, %u bytes/pixel\n",
         lcd_width(), lcd_height(), lcd_bytes_per_pixel());

    benchmark();
    gfx_bringup();
#ifdef S3_GFX_SELFTEST
    // Four differently-coloured corners and one wide bar: the smallest picture
    // that separates inversion, colour order and rotation from each other.
    // Keep it - the next panel will need it too.
    gfx_selftest();
#else
    // gfx_demo_scene() used to be called here and could not be seen: the
    // backlight is still at 0 until the boot screen fades in, and the boot
    // screen then paints over it. It runs after the boot screen now - see
    // system_loop() in system.cpp.
    gfx_damage_demo();
    gfx_chunk_demo();
    gfx_text_demo();
#endif

    // Stage 6.1: from here the panel shows one screen, and each step below
    // moves its spinner on.
    boot_screen_begin(system_fb());

    // Pins and the event queue exist before core 1 does - its first task is
    // the sampler, and it must not find either half-built. The scripted test
    // runs here too, while nothing else is feeding the queue.
    boot_step("the event bus");
    evbus_init();
    app_system_init(system_fb());         // the manager's queue exists before core 1
    boot_step("the device service");
    dev_init();                     // hardware service for apps, on this core
    {
        // Mounted before core 1 exists, so the first mount - which formats an
        // empty partition - cannot catch anything of core 1's mid-flight.
        boot_step("the filesystem");
        const int64_t t0 = esp_timer_get_time();
        const bool ok = fs_init();
        size_t total = 0, used = 0;
        fs_info(&total, &used);
        logf("[fs] LittleFS on 'storage': %s in %lu ms, %lu KB total, %lu KB used\n",
             ok ? "mounted" : "NOT mounted",
             (unsigned long)((esp_timer_get_time() - t0) / 1000),
             (unsigned long)(total / 1024), (unsigned long)(used / 1024));
    }

    // Stage 4.5: the system log and settings, as early as the filesystem
    // allows, so the first thing each boot records is itself - its number and
    // why the previous one ended. A crash overnight is then a line on flash,
    // not something that happened to a console nobody was watching.
    {
        boot_step("the system log and settings");
        const uint32_t boot = fs_mounted() ? fs_boot_count() : 0;
        slog_init(boot);
        slog("sys", "boot #%lu, last reset: %s, firmware %s %s", (unsigned long)boot,
             reset_reason_name(), __DATE__, __TIME__);
        kfault_set_sink([](unsigned cause, const char *name, unsigned pc, unsigned addr,
                           bool dbl) {
            slog("fault", "core 1 fault %u (%s) at pc 0x%08x, address 0x%08x%s", cause,
                 name, pc, addr, dbl ? ", DOUBLE fault" : "");
        });
        settings_define("backlight", 100, 5, 100,
                        [](int32_t v) { lcd_backlight_pct((uint8_t)v); });
        settings_define("screenoff", 60, 0, 600, nullptr);   // seconds; 0 = never (5.4)
        settings_define("wallpaper", 1, 0, 1, nullptr);      // Home's background (6.2)
        settings_load();
        screen_init();                  // stage 5.4: the idle timer starts now
    }
    boot_step("the buttons");
    input_init();
    boot_step("checking itself");
    input_logic_test();
    evbus_logic_test();
    sdk_logic_test();
    ui_logic_test();

    // The kernel starts only now. Measuring the display first keeps core 0 to
    // ourselves for the benchmark - once k_smp_join runs, core 0 is shared with
    // whatever the scheduler puts there and the numbers stop meaning anything.
#if defined(S3_TEST_PARK) || defined(S3_TEST_PARK_OFF)
    park_psram_alloc();             // tests/test_park.cpp owns the buffer
#endif

    boot_step("the kernel on core 1");
    wake_core1();

#if defined(S3_TEST_PARK) || defined(S3_TEST_PARK_OFF)
    park_flash_test();
#endif
    park_smoke_test();

    // Stage 4.4, every boot: bump the boot counter kept on flash. Core 1 is
    // running now, so this write goes through the global park hook - no
    // explicit kpark call anywhere near it - and the stats must show it parked.
    {
        logln();
        logln("--- stage 4.4 / 4.5: files, the system log, settings ---");
        // Core 1 is running now, so this log line - a flash write - goes
        // through the global park hook with no explicit kpark call near it,
        // and the stats must show it parked. (Until 4.5 the boot counter was
        // written here for the same purpose; it moved to the start of boot.)
        const kpark_stats_t p0 = kpark_stats();
        slog("sys", "kernel up on core 1");
        const kpark_stats_t p1 = kpark_stats();
        char d[128];
        snprintf(d, sizeof(d), "boot #%lu; %lu flash ops, %lu parks, %lu unparked",
                 (unsigned long)fs_boot_number(), (unsigned long)(p1.flash_ops - p0.flash_ops),
                 (unsigned long)(p1.parks - p0.parks),
                 (unsigned long)(p1.unparked - p0.unparked));
        sdk_case("log write, parked", fs_boot_number() >= 1 && p1.flash_ops > p0.flash_ops &&
                 p1.parks > p0.parks && p1.unparked == p0.unparked, d);

        // This boot's first line must be in the file, read back off flash.
        char want[24];
        snprintf(want, sizeof(want), "boot #%lu,", (unsigned long)fs_boot_number());
        const bool found = log_has(want);
        snprintf(d, sizeof(d), "\"%s\" in the log: %s; log %lu bytes", want,
                 found ? "yes" : "NO", (unsigned long)slog_size());
        sdk_case("boot recorded", found, d);

        // The brightness setting, loaded from flash, is what the PWM is doing.
        const int32_t bl = settings_get("backlight");
        const uint32_t duty = lcd_backlight_duty();
        snprintf(d, sizeof(d), "backlight %ld %%, PWM duty %lu of %lu (want %lu)", (long)bl,
                 (unsigned long)duty, (unsigned long)LCD_BACKLIGHT_MAX_DUTY,
                 (unsigned long)(bl * LCD_BACKLIGHT_MAX_DUTY / 100));
        sdk_case("settings applied",
                 duty == (uint32_t)(bl * LCD_BACKLIGHT_MAX_DUTY / 100), d);

        logln("  files kept by apps:");
        fs_print_tree();
    }

#ifdef S3_TEST_PANIC_REBOOT
    // Crash core 0 on purpose while core 1 is running, and check the boot
    // after it. A marker file makes it crash exactly once: written just before
    // the crash, found - and removed - by the boot that follows. Without the
    // restart wrap in kcore1.cpp, that boot met a core 1 still running the old
    // kernel over the new one's memory.
    {
        logln();
        logln("--- stage 4.4: a crash while core 1 runs, then the next boot ---");
        struct stat st;
        if (stat("/fs/sys/panic_test", &st) != 0) {
            FILE *f = fopen("/fs/sys/panic_test", "w");
            if (f) { fputs("crash once\n", f); fclose(f); }
            logln("  core 1 is running; crashing core 0 on purpose now");
            hal_delay_ms(50);
            abort();
        }
        unlink("/fs/sys/panic_test");
        const uint32_t t0 = k_tick_count;
        hal_delay_ms(100);
        const uint32_t ticks = k_tick_count - t0;
        char d[96];
        snprintf(d, sizeof(d), "reset: %s; core 1 faults %u; %lu ticks in 100 ms",
                 esp_reset_reason() == ESP_RST_PANIC ? "panic" : "other",
                 kfault_reported(), (unsigned long)ticks);
        sdk_case("clean boot after a crash", esp_reset_reason() == ESP_RST_PANIC &&
                 kfault_reported() == 0 && ticks >= 95 && ticks <= 105, d);
    }
#endif

    system_loop();
}
