#include "gfx/fb.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "s3hal/lcd_panel.h"
#include "s3hal/log.h"

bool fb_init(fb_t *fb, uint16_t w, uint16_t h)
{
    fb->px          = nullptr;
    fb->w           = w;
    fb->h           = h;
    fb->bytes       = (size_t)w * h * sizeof(uint16_t);
    fb->ndamage     = 0;
    fb->all_damaged = false;
    fb->oy          = 0;

    fb->px = (uint16_t *)heap_caps_malloc(fb->bytes, MALLOC_CAP_SPIRAM);
    if (!fb->px) {
        logf("[fb] no room for a %ux%u framebuffer (%u bytes)\n",
             w, h, (unsigned)fb->bytes);
        return false;
    }
    return true;
}

void fb_free(fb_t *fb)
{
    if (fb->px) heap_caps_free(fb->px);
    fb->px = nullptr;
}

// Clips a span to the framebuffer. Returns false if nothing is left of it.
//
// Every primitive goes through this rather than trusting its caller. Drawing
// is the one place in this system that will be called from application code,
// and an off-screen coordinate should cost a comparison, not a corrupted heap.
static inline bool clip_rect(const fb_t *fb, int *x, int *y, int *w, int *h)
{
    if (*w <= 0 || *h <= 0) return false;

    if (*x < 0) { *w += *x; *x = 0; }
    if (*y < 0) { *h += *y; *y = 0; }
    if (*x >= fb->w || *y >= fb->h) return false;

    if (*x + *w > fb->w) *w = fb->w - *x;
    if (*y + *h > fb->h) *h = fb->h - *y;

    return (*w > 0 && *h > 0);
}

void fb_clear(fb_t *fb, uint16_t color)
{
    if (!fb->px) return;

    // memset is only usable when both halves of the pixel are the same byte,
    // which is true of black and white and nothing else. The general case has
    // to write pixels, and on PSRAM that is worth measuring - see the stage 3.2
    // numbers in the README.
    fb_damage_all(fb);

    uint8_t lo = (uint8_t)(color & 0xFF);
    uint8_t hi = (uint8_t)(color >> 8);
    if (lo == hi) {
        memset(fb->px, lo, fb->bytes);
        return;
    }

    size_t n = (size_t)fb->w * fb->h;
    uint16_t *p = fb->px;
    for (size_t i = 0; i < n; ++i) p[i] = color;
}

void fb_pixel(fb_t *fb, int x, int y, uint16_t color)
{
    if (!fb->px) return;
    if (x < 0 || y < 0 || x >= fb->w || y >= fb->h) return;
    fb->px[(size_t)y * fb->w + x] = color;
    fb_damage(fb, x, y, 1, 1);
}

void fb_fill_rect(fb_t *fb, int x, int y, int w, int h, uint16_t color)
{
    if (!fb->px || !clip_rect(fb, &x, &y, &w, &h)) return;

    // Marked with the CLIPPED rectangle, not what the caller asked for. A flush
    // turns these straight into panel addresses, so a damage record that runs
    // off the screen would be a command the controller rejects.
    fb_damage(fb, x, y, w, h);

    for (int row = 0; row < h; ++row) {
        uint16_t *p = fb->px + (size_t)(y + row) * fb->w + x;
        for (int i = 0; i < w; ++i) p[i] = color;
    }
}

void fb_blend_rect(fb_t *fb, int x, int y, int w, int h, uint16_t color, uint8_t pct)
{
    if (!fb->px || pct > 100) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > fb->w) w = fb->w - x;
    if (y + h > fb->h) h = fb->h - y;
    if (w <= 0 || h <= 0) return;

    // Pixels are stored byte-swapped (see the note in fb.h), so each one is
    // swapped back, mixed in its three fields, and swapped again.
    const uint16_t c  = (uint16_t)((color >> 8) | (color << 8));
    const int cr = (c >> 11) & 31, cg = (c >> 5) & 63, cb = c & 31;
    const int keep = 100 - (int)pct;

    for (int row = 0; row < h; ++row) {
        uint16_t *p = fb->px + (size_t)(y + row) * fb->w + x;
        for (int i = 0; i < w; ++i) {
            const uint16_t v = (uint16_t)((p[i] >> 8) | (p[i] << 8));
            const int r = (((v >> 11) & 31) * keep + cr * pct) / 100;
            const int g = (((v >> 5) & 63)  * keep + cg * pct) / 100;
            const int b = ((v & 31)         * keep + cb * pct) / 100;
            const uint16_t o = (uint16_t)((r << 11) | (g << 5) | b);
            p[i] = (uint16_t)((o >> 8) | (o << 8));
        }
    }
    fb_damage(fb, x, y, w, h);
}

void fb_hline(fb_t *fb, int x, int y, int w, uint16_t color)
{
    fb_fill_rect(fb, x, y, w, 1, color);
}

void fb_vline(fb_t *fb, int x, int y, int h, uint16_t color)
{
    fb_fill_rect(fb, x, y, 1, h, color);
}

void fb_rect(fb_t *fb, int x, int y, int w, int h, uint16_t color)
{
    if (w <= 0 || h <= 0) return;
    fb_hline(fb, x, y,         w, color);
    fb_hline(fb, x, y + h - 1, w, color);
    fb_vline(fb, x,         y, h, color);
    fb_vline(fb, x + w - 1, y, h, color);
}

// A pixel with bounds checking and no damage mark: the shapes below mark their
// whole bounding box once instead.
static inline void plot(fb_t *fb, int x, int y, uint16_t color)
{
    if ((unsigned)x < fb->w && (unsigned)y < fb->h) fb->px[(size_t)y * fb->w + x] = color;
}

void fb_line(fb_t *fb, int x0, int y0, int x1, int y1, uint16_t color)
{
    if (!fb->px) return;

    // Straight lines are rectangles, and fb_fill_rect is much faster.
    if (y0 == y1) { fb_hline(fb, x0 < x1 ? x0 : x1, y0, (x0 < x1 ? x1 - x0 : x0 - x1) + 1, color); return; }
    if (x0 == x1) { fb_vline(fb, x0, y0 < y1 ? y0 : y1, (y0 < y1 ? y1 - y0 : y0 - y1) + 1, color); return; }

    // Bresenham, all octants. Pixels off the buffer are skipped one at a time,
    // which is simple and exact; lines that run far off-screen are rare enough
    // not to need clipping up front.
    const int dx = x1 > x0 ? x1 - x0 : x0 - x1;
    const int dy = y1 > y0 ? y0 - y1 : y1 - y0;      // negative
    const int sx = x0 < x1 ? 1 : -1;
    const int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    int x = x0, y = y0;
    for (;;) {
        plot(fb, x, y, color);
        if (x == x1 && y == y1) break;
        const int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x += sx; }
        if (e2 <= dx) { err += dx; y += sy; }
    }

    const int lx = x0 < x1 ? x0 : x1, ly = y0 < y1 ? y0 : y1;
    fb_damage(fb, lx, ly, dx + 1, -dy + 1);
}

void fb_circle(fb_t *fb, int cx, int cy, int r, uint16_t color)
{
    if (!fb->px || r < 0) return;

    // Midpoint circle: one octant computed, eight plotted.
    int x = r, y = 0, err = 1 - r;
    while (x >= y) {
        plot(fb, cx + x, cy + y, color); plot(fb, cx - x, cy + y, color);
        plot(fb, cx + x, cy - y, color); plot(fb, cx - x, cy - y, color);
        plot(fb, cx + y, cy + x, color); plot(fb, cx - y, cy + x, color);
        plot(fb, cx + y, cy - x, color); plot(fb, cx - y, cy - x, color);
        ++y;
        if (err < 0) err += 2 * y + 1;
        else { --x; err += 2 * (y - x) + 1; }
    }
    fb_damage(fb, cx - r, cy - r, 2 * r + 1, 2 * r + 1);
}

void fb_fill_circle(fb_t *fb, int cx, int cy, int r, uint16_t color)
{
    if (!fb->px || r < 0) return;

    // One horizontal span per row, found from the same midpoint walk, and each
    // span drawn as a clipped fill - so no pixel is written twice.
    int x = r, y = 0, err = 1 - r;
    int last_y = -1;
    while (x >= y) {
        // Rows cy +/- y are x wide; rows cy +/- x are y wide.
        if (y != last_y) {
            fb_fill_rect(fb, cx - x, cy + y, 2 * x + 1, 1, color);
            if (y) fb_fill_rect(fb, cx - x, cy - y, 2 * x + 1, 1, color);
            last_y = y;
        }
        if (err >= 0 && x != y) {
            fb_fill_rect(fb, cx - y, cy + x, 2 * y + 1, 1, color);
            fb_fill_rect(fb, cx - y, cy - x, 2 * y + 1, 1, color);
        }
        ++y;
        if (err < 0) err += 2 * y + 1;
        else { --x; err += 2 * (y - x) + 1; }
    }
}

void fb_blit(fb_t *fb, int x, int y, int w, int h, const uint16_t *src)
{
    if (!fb->px || !src) return;

    int ox = x, oy = y, ow = w, oh = h;
    if (!clip_rect(fb, &x, &y, &w, &h)) return;

    // How much of the source was clipped away, so the copy stays aligned to it.
    int skip_x = x - ox;
    int skip_y = y - oy;
    (void)oh;

    fb_damage(fb, x, y, w, h);

    for (int row = 0; row < h; ++row) {
        const uint16_t *s = src + (size_t)(skip_y + row) * ow + skip_x;
        uint16_t       *d = fb->px + (size_t)(y + row) * fb->w + x;
        memcpy(d, s, (size_t)w * sizeof(uint16_t));
    }
}

void fb_view(fb_t *view, const fb_t *parent, uint16_t y, uint16_t h)
{
    if (y > parent->h)         y = parent->h;
    if (y + h > parent->h)     h = (uint16_t)(parent->h - y);
    view->px          = parent->px ? parent->px + (size_t)y * parent->w : nullptr;
    view->w           = parent->w;
    view->h           = h;
    view->bytes       = (size_t)view->w * h * sizeof(uint16_t);
    view->ndamage     = 0;
    view->all_damaged = false;
    view->oy          = (uint16_t)(parent->oy + y);
}

void fb_present(fb_t *fb)
{
    if (!fb->px) return;
    lcd_blit_wire(0, fb->oy, fb->w, fb->h, (const uint8_t *)fb->px,
                  (size_t)fb->w * sizeof(uint16_t));
    fb_damage_reset(fb);
}

// ---------------------------------------------------------------------------
// Damage tracking
// ---------------------------------------------------------------------------

static inline int32_t rect_area(const fb_rect_t *r)
{
    return (int32_t)r->w * r->h;
}

static fb_rect_t rect_union(const fb_rect_t *a, const fb_rect_t *b)
{
    int x0 = a->x < b->x ? a->x : b->x;
    int y0 = a->y < b->y ? a->y : b->y;
    int x1 = (a->x + a->w) > (b->x + b->w) ? (a->x + a->w) : (b->x + b->w);
    int y1 = (a->y + a->h) > (b->y + b->h) ? (a->y + a->h) : (b->y + b->h);

    fb_rect_t r = { (int16_t)x0, (int16_t)y0, (int16_t)(x1 - x0), (int16_t)(y1 - y0) };
    return r;
}

// Should these two regions be sent as one transfer instead of two?
//
// Purely a question of cost, and that is worth being precise about, because
// the first version of this got it wrong. It forced a merge whenever two
// regions overlapped, on the theory that the overlap would otherwise be sent
// twice. It would - but the bytes are read out of the framebuffer at flush
// time, so both copies are identical and the panel cannot tell. Overlap costs
// time and nothing else.
//
// What that mistaken rule did cost was a box outline. Its top edge and its left
// edge share exactly one corner pixel, which made the merge mandatory and
// turned four thin lines - 638 pixels - into their bounding box, 24,000. The
// commonest shape in an interface was also the worst case.
//
// So: arithmetic only. A separate transfer costs about 41 us of fixed overhead,
// measured in stage 3.3b - a 40x20 flush spends 49 us per row against 8 us of
// wire time, and a full-screen flush spends 105 us per row against 64. At the
// 6.1 MB/s the panel actually sustains, 41 us buys roughly 125 pixels, so a
// merge is worth it whenever the union adds fewer than that.
//
// The ratio that follows is the same trade at a size where the constant stops
// being meaningful: on large regions, 125 pixels is noise, and what matters is
// not inflating a transfer by more than a quarter.
//
// Overlap needs no special case. Two regions that overlap have a union smaller
// than the sum of their areas, so the waste is negative and the first test
// merges them - which is the right answer for the right reason.
static bool should_merge(const fb_rect_t *a, const fb_rect_t *b)
{
    fb_rect_t u = rect_union(a, b);
    int32_t   waste = rect_area(&u) - rect_area(a) - rect_area(b);

    if (waste <= 128) return true;                  // under one transfer's worth
    return rect_area(&u) * 4 <= (rect_area(a) + rect_area(b)) * 5;
}

// A merge grows a rectangle, and the grown rectangle may now qualify to absorb
// others that it did not reach before. Runs until nothing more will merge;
// with eight slots the worst case is trivial.
static void coalesce(fb_t *fb)
{
    bool again = true;
    while (again) {
        again = false;
        for (int i = 0; i < fb->ndamage && !again; ++i) {
            for (int j = i + 1; j < fb->ndamage; ++j) {
                if (!should_merge(&fb->damage[i], &fb->damage[j])) continue;

                fb->damage[i] = rect_union(&fb->damage[i], &fb->damage[j]);
                fb->damage[j] = fb->damage[fb->ndamage - 1];
                fb->ndamage--;
                again = true;
                break;
            }
        }
    }
}

void fb_damage(fb_t *fb, int x, int y, int w, int h)
{
    // Already sending everything - there is nothing a further region can add,
    // and skipping the walk makes drawing after a clear cost nothing to track.
    if (fb->all_damaged) return;
    if (!clip_rect(fb, &x, &y, &w, &h)) return;

    fb_rect_t r = { (int16_t)x, (int16_t)y, (int16_t)w, (int16_t)h };

    for (int i = 0; i < fb->ndamage; ++i) {
        if (should_merge(&fb->damage[i], &r)) {
            fb->damage[i] = rect_union(&fb->damage[i], &r);
            coalesce(fb);
            return;
        }
    }

    if (fb->ndamage < FB_DAMAGE_MAX) {
        fb->damage[fb->ndamage++] = r;
        return;
    }

    // The list is full, so something has to give. Merge into whichever existing
    // region grows least - the cheapest lie we can tell about what changed.
    // Note this is the ONLY place the fixed-size list costs anything, and it
    // costs pixels, never correctness: the union always covers the real damage.
    int     best = 0;
    int32_t best_cost = INT32_MAX;
    for (int i = 0; i < fb->ndamage; ++i) {
        fb_rect_t u = rect_union(&fb->damage[i], &r);
        int32_t   cost = rect_area(&u) - rect_area(&fb->damage[i]);
        if (cost < best_cost) { best_cost = cost; best = i; }
    }
    fb->damage[best] = rect_union(&fb->damage[best], &r);
    coalesce(fb);
}

void fb_damage_all(fb_t *fb)
{
    fb->damage[0].x = 0;
    fb->damage[0].y = 0;
    fb->damage[0].w = (int16_t)fb->w;
    fb->damage[0].h = (int16_t)fb->h;
    fb->ndamage     = 1;
    fb->all_damaged = true;
}

void fb_damage_reset(fb_t *fb)
{
    fb->ndamage     = 0;
    fb->all_damaged = false;
}

int fb_damage_count(const fb_t *fb)
{
    return fb->ndamage;
}

size_t fb_damage_pixels(const fb_t *fb)
{
    size_t n = 0;
    for (int i = 0; i < fb->ndamage; ++i)
        n += (size_t)fb->damage[i].w * fb->damage[i].h;
    return n;
}

const fb_rect_t *fb_damage_at(const fb_t *fb, int i)
{
    if (i < 0 || i >= fb->ndamage) return nullptr;
    return &fb->damage[i];
}

void fb_flush(fb_t *fb)
{
    if (!fb->px) return;

    const size_t stride = (size_t)fb->w * sizeof(uint16_t);

    for (int i = 0; i < fb->ndamage; ++i) {
        const fb_rect_t *r = &fb->damage[i];
        const uint8_t   *src =
            (const uint8_t *)(fb->px + (size_t)r->y * fb->w + r->x);
        lcd_blit_wire((uint16_t)r->x, (uint16_t)(r->y + fb->oy),
                      (uint16_t)r->w, (uint16_t)r->h, src, stride);
    }

    fb_damage_reset(fb);
}
