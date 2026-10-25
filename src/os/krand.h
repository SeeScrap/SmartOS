#pragma once
#include <stdint.h>

// ---------------------------------------------------------------------------
// Stage 4.3c - random numbers for apps. Games, mostly.
//
// xorshift32: fast, small, and good enough to place food on a snake board -
// not for anything that must be unpredictable to an adversary. Seeded from
// the hardware RNG at start-up, or explicitly with k_srand() when a test needs
// the same sequence twice.
//
// One shared state for every caller. Two apps drawing numbers at the same
// instant can interleave their updates and see related values; for games that
// is harmless, and it keeps this free of locks.
//
// xorshift32: เร็ว เล็ก และดีพอสำหรับวางอาหารบนกระดานงู **ไม่ใช่**สำหรับอะไร
// ที่ต้องเดาไม่ได้จากฝ่ายตรงข้าม หว่านเมล็ดจาก RNG ของฮาร์ดแวร์ตอนเริ่ม
// หรือกำหนดเองด้วย k_srand() เมื่อการทดสอบต้องการลำดับเดิมสองรอบ
//
// สถานะเดียวใช้ร่วมกันทุกผู้เรียก สองแอปที่สุ่มพร้อมกันอาจได้ค่าที่สัมพันธ์กัน
// สำหรับเกมถือว่าไม่เป็นไร และทำให้ตรงนี้ไม่ต้องมีล็อกเลย
// ---------------------------------------------------------------------------

void     k_srand(uint32_t seed);          // 0 re-seeds from the hardware
uint32_t k_rand(void);
uint32_t k_rand_below(uint32_t n);        // uniform in [0, n); 0 if n is 0
