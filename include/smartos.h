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

// Mutexes, semaphores and queues, all of them with timeouts.
#include "kernel/ksync.h"

// The clock, sleeping, and the preemption controls.
#include "kernel/ktime.h"

// The kernel's own allocator: two pools, one of them PSRAM.
#include "kernel/kheap.h"

// klog(): printing from a task, safely, on either core.
#include "kernel/klog.h"

// k_snprintf and friends. Not libc's: no locks, no heap, and it works on
// core 1, which libc's printf does not.
#include "os/kfmt.h"
