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
// ---------------------------------------------------------------------------

extern const image_t g_wallpaper;
