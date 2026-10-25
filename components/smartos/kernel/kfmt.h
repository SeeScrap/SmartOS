#pragma once
#include <stdarg.h>
#include <stddef.h>

// ---------------------------------------------------------------------------
// Stage 4.3b - formatting for code that runs on core 1.
//
// libc's snprintf is not safe there. newlib reaches its per-task state through
// FreeRTOS's current task, and core 1 has no FreeRTOS task at all - it would be
// reading core 0's bookkeeping, or nobody's. Apps run on core 1, so they get
// this instead: no locks, no heap, no per-task state, nothing but the buffer
// it was handed.
//
// Supported: %d %i %u %x %X %s %c %%, with an optional '-' (left-justify),
// '0' (zero-pad), a field width, and for %s a precision (%.10s takes at most
// ten characters of the string). 'l' is accepted and ignored, since int and
// long are the same size here. No floating point: print a fixed-point value
// as two integers instead.
//
// Always NUL-terminates when n > 0. Returns the length that would have been
// written, like snprintf, so a caller can tell it was truncated.
//
// snprintf ของ libc ใช้บนคอร์ 1 ไม่ได้ เพราะ newlib เข้าถึงสถานะรายงานผ่าน
// task ปัจจุบันของ FreeRTOS แต่คอร์ 1 ไม่มี task ของ FreeRTOS เลย มันจะไปอ่าน
// ของคอร์ 0 หรือของใครก็ไม่รู้ แอปรันบนคอร์ 1 จึงได้ตัวนี้แทน: ไม่มีล็อก ไม่มี heap
// ไม่มีสถานะรายงาน มีแค่บัฟเฟอร์ที่ถูกส่งเข้ามา
//
// ไม่รองรับทศนิยม ถ้าต้องพิมพ์ fixed-point ให้พิมพ์เป็นจำนวนเต็มสองตัว
// ---------------------------------------------------------------------------

int k_snprintf(char *buf, size_t n, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
int k_vsnprintf(char *buf, size_t n, const char *fmt, va_list ap);
