#include "gfx/image.h"

#include <string.h>

// Both calls end up here: a clipped rectangle of the picture, copied row by
// row. The source is normally flash, read through the cache - which is why
// core 1 must be parked while core 0 writes flash, and is (stage 4.3a).
static void blit(fb_t *fb, const image_t *img, int dx, int dy, int sx, int sy, int w, int h)
{
    if (!fb || !fb->px || !image_ok(img)) return;

    if (sx < 0) { w += sx; dx -= sx; sx = 0; }
    if (sy < 0) { h += sy; dy -= sy; sy = 0; }
    if (dx < 0) { w += dx; sx -= dx; dx = 0; }
    if (dy < 0) { h += dy; sy -= dy; dy = 0; }
    if (dx + w > fb->w)  w = fb->w - dx;
    if (dy + h > fb->h)  h = fb->h - dy;
    if (sx + w > img->w) w = img->w - sx;
    if (sy + h > img->h) h = img->h - sy;
    if (w <= 0 || h <= 0) return;

    for (int row = 0; row < h; ++row) {
        memcpy(fb->px + (size_t)(dy + row) * fb->w + dx,
               img->px + (size_t)(sy + row) * img->w + sx,
               (size_t)w * sizeof(uint16_t));
    }
    fb_damage(fb, dx, dy, w, h);
}

void image_blit(fb_t *fb, const image_t *img, int x, int y)
{
    if (!image_ok(img)) return;
    blit(fb, img, x, y, 0, 0, img->w, img->h);
}

void image_blit_rect(fb_t *fb, const image_t *img, int x, int y, int w, int h)
{
    blit(fb, img, x, y, x, y, w, h);
}
