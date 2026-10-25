#pragma once

#include "gfx/image.h"

// ---------------------------------------------------------------------------
// Stage 6.2 - Home's background.
//
// The picture itself is src/gfx/wallpaper_data.cpp, which is generated:
//
//     python tools/mkimage.py <a picture> src/gfx/wallpaper_data.cpp
//
// 320x216 - Home's area, the status bar excluded - in the panel's wire order,
// in flash. Changing the wallpaper is that one command and a rebuild; the
// "wallpaper" setting turns it off without one.
//
// ตัวรูปอยู่ใน src/gfx/wallpaper_data.cpp ซึ่งถูกสร้างขึ้นด้วย tools/mkimage.py
// ขนาด 320x216 คือพื้นที่ของ Home ที่หักแถบสถานะออกแล้ว เรียงตามลำดับสายของจอ
// และอยู่ในแฟลช การเปลี่ยนภาพพื้นหลังคือคำสั่งเดียวบวกการบิลด์ใหม่
// ส่วนค่าตั้งค่า "wallpaper" ปิดมันได้โดยไม่ต้องบิลด์
// ---------------------------------------------------------------------------

extern const image_t g_wallpaper;
