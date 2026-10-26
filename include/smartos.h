#pragma once

// ===========================================================================
// SmartOS - the kernel, for a program written on it.
// SmartOS - เคอร์เนล สำหรับโปรแกรมที่เขียนอยู่บนมัน
// ===========================================================================
//
// Include this and you have tasks, locks, a clock, a heap and a way to print.
// Nothing else is needed and nothing else should be reached for.
//
// This is the whole surface a program needs if it wants the kernel and not
// the rest of the system: no screen, no apps, no filesystem. examples/ has a
// program that includes only this file and nothing else from the tree, which
// is how we know the claim is true rather than merely intended.
//
// ไฟล์นี้คือพื้นผิวทั้งหมดที่โปรแกรมต้องใช้ ถ้าต้องการแค่เคอร์เนล ไม่เอาส่วนอื่นของ
// ระบบ ไม่มีหน้าจอ ไม่มีแอป ไม่มีระบบไฟล์
//
// FOR APPLICATIONS, include smartos_app.h instead. It includes this file and
// adds everything an app on this OS can use.
//
// WHAT IS DELIBERATELY NOT HERE
//
// The scheduler's own structures, the interrupt handlers' hooks and the raw
// scheduler lock are in kernel/internal/ktask_internal.h. The spinlocks the
// kernel builds its locks out of are in kernel/kspin.h. The cross-core
// mailbox is in kernel/kmbox.h. Those are the kernel talking to itself; a
// program written on it has mutexes, semaphores and queues instead, and they
// are better in every way that matters to a program.
// ---------------------------------------------------------------------------

#include "smartos_config.h"

// Tasks: create, yield, sleep, end, and ask how they are doing.
#include "kernel/ktask.h"

// Mutexes, semaphores and queues. Each has a blocking call and a try-once
// call - k_mutex_lock / k_mutex_try_lock, and the same shape for the other
// two.
//
// WHAT IS NOT HERE YET, said plainly because this header is a promise and a
// promise that overstates is worse than none. Until R8-A1e this line claimed
// "all of them with timeouts", and none of them has one.
//
//   no timeouts            a blocking call waits for ever, or you use the
//                          try-once form and decide for yourself
//   no recursive mutex     locking one twice from the same task is reported,
//                          not allowed
//   no priority inheritance  a low-priority task holding a mutex that a
//                          high-priority task wants is not lifted. Priority
//                          inversion can happen here and nothing detects it
//   no task notifications
//
// They are listed because they are what a port of ESP-IDF's drivers onto this
// kernel would need first, not because they are planned.
#include "kernel/ksync.h"

// The clock, sleeping, and the preemption controls.
#include "kernel/ktime.h"

// The kernel's own allocator: two pools, one of them PSRAM.
#include "kernel/kheap.h"

// klog(): printing from a task, safely, on either core.
#include "kernel/klog.h"

// k_snprintf and friends. Not libc's: no locks, no heap, and it works on
// core 1, which libc's printf does not.
#include "kernel/kfmt.h"
