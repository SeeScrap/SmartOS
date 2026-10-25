// ===========================================================================
// SmartOS, on its own.
// SmartOS แบบยืนเดี่ยว
// ===========================================================================
//
// This program is the whole point of the restructuring. It builds against the
// SmartOS kernel and nothing else: no event bus, no app manager, no
// filesystem, no screen, no toolkit. If it links, those layers really are
// separable, and "you can write your own program on this kernel" is a fact
// rather than a claim in a README.
//
// โปรแกรมนี้คือเหตุผลทั้งหมดของการรื้อโครงสร้าง มันบิลด์กับเคอร์เนล SmartOS
// อย่างเดียว ไม่มีบัสเหตุการณ์ ไม่มีตัวจัดการแอป ไม่มีระบบไฟล์ ไม่มีหน้าจอ
// ถ้ามันลิงก์ติด แปลว่าชั้นต่างๆ แยกออกจากกันได้จริง
//
//     PLATFORMIO_BUILD_DIR=.pio/cli pio run -e kernel-blink -t upload
//
// WHAT IT SHOWS
//   two tasks sharing a counter through a mutex   - the scheduler and locks
//   a task that sleeps and wakes on time          - the tick
//   a task driving a pin                          - a task reaching hardware
//   the whole thing ending and reporting          - and the heap, underneath
//
// WHY IT INCLUDES FOUR HEADERS AND NOT ONE
//
// smartos.h is what a program written ON the kernel uses: tasks, locks, the
// clock, the heap, printing. Starting the kernel is a different job - on this
// chip the kernel lives on a core that ESP-IDF leaves asleep, so somebody has
// to wake it. That is integration, not application code, and it is the part
// that differs most between chips, so it is reached for by name.
//
// เหตุผลที่ include สี่ไฟล์ ไม่ใช่ไฟล์เดียว: smartos.h คือสิ่งที่โปรแกรม*บน*
// เคอร์เนลใช้ ส่วนการ*ปลุก*เคอร์เนลเป็นงานคนละอย่าง และเป็นส่วนที่ต่างกันมาก
// ที่สุดระหว่างชิป จึงต้องเรียกถึงตรงๆ
// ---------------------------------------------------------------------------

#include "smartos.h"            // the kernel: tasks, locks, clock, heap, klog

#include "kernel/kfault.h"      // bringing it up: watch for a crash on core 1
#include "kernel/kmbox.h"       //                 core 1 asking core 0 for things
#include "port/kcore1.h"        //                 waking core 1
#include "port/kpark.h"         //                 parking it around flash writes

#include "soc/gpio_reg.h"       // the blink, without a driver component
#include "esp_rom_gpio.h"
#include "esp_rom_sys.h"

// The backlight, because it is the one pin on this board whose state you can
// see from across the room. Any free pin would do.
#define BLINK_PIN 47

// --- what the kernel runs ---------------------------------------------------

static K_DRAM k_mutex_t   s_lock;
static volatile K_DRAM uint32_t s_counter;
static volatile K_DRAM uint32_t s_ticks_seen;
static volatile K_DRAM uint32_t s_blinks;
static volatile K_DRAM bool     s_done;

// Two of these run. Each adds to the same counter a thousand times, under a
// mutex. If the final count is two thousand, the lock held and no increment
// was lost between a read and a write.
static void K_IRAM counter_task(void *arg)
{
    const char *who = (const char *)arg;
    for (int i = 0; i < 1000; ++i) {
        k_mutex_lock(&s_lock);
        s_counter = s_counter + 1;
        k_mutex_unlock(&s_lock);
        if ((i % 250) == 0) k_yield();      // give the other one a real chance
    }
    klog("[%s] done\n", who);
}

// Sleeps on the kernel's own clock. Nothing polls: the task is marked
// SLEEPING and the tick interrupt is what makes it runnable again.
static void K_IRAM sleeper_task(void *arg)
{
    (void)arg;
    const uint32_t t0 = k_millis();
    for (int i = 0; i < 20; ++i) {
        k_sleep_ms(25);
        s_ticks_seen = k_millis() - t0;
    }
}

// A task with a pin. Writing the GPIO register directly keeps this program
// free of ESP-IDF's driver component - the kernel does not need it, so the
// example does not either.
static void K_IRAM blink_task(void *arg)
{
    (void)arg;
    const uint32_t bit = 1u << (BLINK_PIN & 31);
    for (int i = 0; i < 10; ++i) {
        REG_WRITE(GPIO_OUT1_W1TS_REG, bit);
        k_sleep_ms(50);
        REG_WRITE(GPIO_OUT1_W1TC_REG, bit);
        k_sleep_ms(50);
        s_blinks = s_blinks + 1;
    }
}

// Runs last, at the lowest priority, so it reports once the rest have
// finished. The lines are in the format tools/boot_check.py reads.
static void K_IRAM report_task(void *arg)
{
    (void)arg;
    k_sleep_ms(1500);

    klog("\n--- SmartOS, kernel only ---\n");
    klog("  %-28s %-30s %s\n", "two tasks, one mutex",
         s_counter == 2000 ? "2000 increments, none lost" : "COUNT WRONG",
         s_counter == 2000 ? "OK" : "FAIL");
    klog("  %-28s %-30s %s\n", "the tick wakes a sleeper",
         s_ticks_seen >= 480 && s_ticks_seen <= 620 ? "20 x 25 ms slept, on time" : "OFF THE CLOCK",
         s_ticks_seen >= 480 && s_ticks_seen <= 620 ? "OK" : "FAIL");
    klog("  %-28s %-30s %s\n", "a task drove a pin",
         s_blinks == 10 ? "10 blinks on GPIO 47" : "WRONG COUNT",
         s_blinks == 10 ? "OK" : "FAIL");

    kheap_stats_t h;
    kheap_stats(&h);
    klog("  %-28s %u of %u bytes used%-11s %s\n", "the kernel heap",
         (unsigned)h.used, (unsigned)h.total, "",
         (h.used > 0 && h.used < h.total) ? "OK" : "FAIL");
    klog("  %-28s %-30s %s\n", "no task overran its stack",
         k_stack_overflow_id() == 0xFF ? "every guard intact" : "A GUARD FIRED",
         k_stack_overflow_id() == 0xFF ? "OK" : "FAIL");

    ktop();
    s_done = true;
}

// --- bringing the kernel up -------------------------------------------------

// The first thing core 1 runs. From here down nothing is ESP-IDF.
static void K_IRAM core1_main(void)
{
    k_init();                   // the kernel's own heap, laid out on this core
    kpark_init_core1();         // before the tick: interrupts open with it
    k_tick_start(K_TICK_HZ);    // CCOMPARE0, this core only

    k_mutex_init(&s_lock, "counter");
    k_task_create("countA", counter_task, (void *)"countA");
    k_task_create("countB", counter_task, (void *)"countB");
    k_task_create("sleeper", sleeper_task, nullptr);
    k_task_create("blink",  blink_task,  nullptr);
    k_task_create_pri("report", report_task, nullptr, K_CPU_ANY, K_PRIO_LOW);

    k_preempt_enable();
    k_start();                  // switches into the first task; does not return
}

extern "C" void app_main(void)
{
    esp_rom_printf("\n=== SmartOS, kernel only ===\n");

    // The pin, before core 1 touches it.
    esp_rom_gpio_pad_select_gpio(BLINK_PIN);
    REG_WRITE(GPIO_ENABLE1_W1TS_REG, 1u << (BLINK_PIN & 31));

    kmbox_init();               // core 0 owns setting the mailbox up
    if (!kheap_reserve()) {     // the kernel's memory, taken here, on core 0
        esp_rom_printf("!! could not reserve the kernel's pools\n");
        return;
    }
    if (!kcore1_start(core1_main, 500)) {
        esp_rom_printf("!! core 1 never reported in\n");
        return;
    }
    esp_rom_printf("core 1 is running the kernel\n");

    // Core 0's whole job from here: answer core 1 when it asks, and notice if
    // it crashes. The real OS does a great deal more in this loop; a program
    // on the kernel need not.
    for (int i = 0; i < 2000 && !s_done; ++i) {
        kmbox_service();
        if (kfault_pending()) { kfault_report(); break; }
        esp_rom_delay_us(2000);
    }
    esp_rom_printf("=== done ===\n");
}
