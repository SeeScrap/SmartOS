#pragma once

// Console output for core 0. logf() and logln() are kport_printf() and
// kport_println() under shorter names - the bodies moved to
// components/smartos/port/kport.cpp in R4b, together with the console lock
// they take, and only the names stayed behind.
//
// WHY A CONSOLE LIVES IN board/
//
// It is not board code, and that was listed as a debt until R8. It stays
// here anyway, for a reason worth writing down rather than re-deciding:
// board/ is the lowest layer inside src/, so os/, gfx/, ui/, apps/ and
// tests/ may all use it. Moving it to os/ would make board/lcd_probe.cpp
// depend on os/, which is upward, and that is a worse problem than a file
// with a slightly wrong name.
//
// The alternative - deleting these three names and calling kport_* at all
// 29 files and several hundred call sites - was measured against what it
// buys and not done. One shim with one line of forwarding is cheaper to
// read than a rename of that size is to review.
//
// คอนโซลของคอร์ 0 ไม่ใช่โค้ดของบอร์ดจริงๆ แต่อยู่ที่นี่เพราะ board/ คือชั้น
// ล่างสุดใน src/ ชั้นบนทุกชั้นจึงเรียกได้ ถ้าย้ายไป os/ จะกลายเป็น board/
// ต้องพึ่ง os/ ซึ่งเป็นการพึ่งพาย้อนขึ้น และแย่กว่าชื่อไฟล์ที่ไม่ตรงนัก
void log_init(void);
void logf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void logln(const char *s = "");
