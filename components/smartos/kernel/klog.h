#pragma once
#include "kernel/kspin.h"
#include "kernel/ktime.h"

// ---------------------------------------------------------------------------
// Logging that is safe to call from core 1.
//
// printf() and the ESP_LOG macros take newlib/FreeRTOS locks. Those locks
// belong to the scheduler on core 0, and core 1 is not a FreeRTOS core at all -
// it has no task control block, so blocking on one of those locks would hang
// the core with no way to recover and nothing printed.
//
// esp_rom_printf() is a ROM routine that pushes characters straight at the
// console peripheral with no locking, which is exactly why IDF's own
// ESP_EARLY_LOGx uses it during startup and on the second core.
//
// THREE THINGS HAVE TO BE HELD OFF AROUND IT
//
// 1. The other core. Core 0 prints through printf and core 1 through this, and
//    both end up writing the same USB-serial-JTAG FIFO. Without the console
//    spinlock the very first bring-up faulted inside the ROM printf with both
//    cores in it.
//
// 2. The scheduler. A task switched out halfway through a line lets another
//    task start its own, so the output interleaves.
//
// 3. Re-entering the ROM at all. esp_rom_printf is a ROM routine with static
//    state, and switching stacks out of the middle of it is what made the
//    preemption bring-up fault: every failing trace had the interrupted PC
//    inside ROM. k_preempt_hold covers 2 and 3 together, and it does it without
//    masking interrupts, so the tick keeps counting while the line goes out.
//
// Limitations to remember: no %f, and the format set is the small ROM one
// (%c %d %u %x %s %p and width/zero padding).
//
// printf() และมาโคร ESP_LOG จับล็อกของ newlib/FreeRTOS ซึ่งเป็นของตัวจัดคิว
// บนคอร์ 0 คอร์ 1 ไม่ใช่คอร์ของ FreeRTOS และไม่มี task control block
// ถ้าไปบล็อกบนล็อกพวกนั้นคอร์จะค้างโดยไม่พิมพ์อะไรออกมาเลย
//
// จึงใช้ esp_rom_printf ซึ่งยัดตัวอักษรเข้าคอนโซลตรงๆ โดยไม่จับล็อก
// และต้องกันสามอย่างรอบมัน: คอร์อีกตัว, ตัวจัดคิว, และการเข้า ROM ซ้อน
// ---------------------------------------------------------------------------
#define klog(...)                                     \
    do {                                              \
        k_preempt_hold();                             \
        bool _kl = k_spin_lock(&k_console_lock);     \
        kport_rom_printf(__VA_ARGS__);                \
        if (_kl) k_spin_unlock(&k_console_lock);     \
        k_preempt_release();                          \
    } while (0)
