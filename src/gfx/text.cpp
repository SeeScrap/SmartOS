#include "gfx/text.h"

#include <string.h>

#include "gfx/font5x8.h"

static inline const uint8_t *glyph_for(char c)
{
    unsigned char u = (unsigned char)c;
    if (u < FONT5X8_FIRST || u > FONT5X8_LAST) u = '?';
    return font5x8[u - FONT5X8_FIRST];
}

// ---------------------------------------------------------------------------
// Drawing one cell.
//
// THIS FIRMWARE IS BUILT AT -Og, and the shape of this code follows from that.
// Two earlier versions, measured per character of a 43-character line:
//
//   walk screen pixels, divide by scale for the glyph bit   6.74 us, any mode
//   walk glyph rows and columns, repeat each `s` times     12.22 us opaque
//                                                           4.77 us transparent
//
// The second removed two divisions per pixel and still got slower, because at
// -Og nothing is unrolled: four nested loops, the innermost running exactly once
// at scale 1, cost more per pixel than the divisions did. So the hot cases are
// written the way an optimiser would have left them - scale 1 as straight-line
// stores, larger scales as one expanded row copied `s` times - and neither
// depends on the compiler to be fast.
//
// Cells that straddle a screen edge take a separate, slower path with a bounds
// test per pixel. They are a few per string at most, and keeping the check out
// of the common path is the whole reason the common path is fast.
// ---------------------------------------------------------------------------

static void draw_cell_clipped(fb_t *fb, int x, int y, const uint8_t *g,
                              uint16_t fg, uint16_t bg, bool opaque, int s)
{
    for (int gr = 0; gr < TEXT_CELL_H; ++gr) {
        const uint8_t bits = gr < FONT5X8_H ? g[gr] : 0;
        if (!bits && !opaque) continue;

        for (int sy = 0; sy < s; ++sy) {
            const int yy = y + gr * s + sy;
            if (yy < 0 || yy >= fb->h) continue;

            uint16_t *row = fb->px + (size_t)yy * fb->w;
            for (int gc = 0; gc < TEXT_CELL_W; ++gc) {
                const bool ink = gc < FONT5X8_W && (bits & (0x10 >> gc));
                if (!ink && !opaque) continue;
                for (int sx = 0; sx < s; ++sx) {
                    const int xx = x + gc * s + sx;
                    if (xx >= 0 && xx < fb->w) row[xx] = ink ? fg : bg;
                }
            }
        }
    }
}

static void draw_cell(fb_t *fb, int x, int y, const uint8_t *g,
                      uint16_t fg, uint16_t bg, bool opaque, int s)
{
    const bool inside = x >= 0 && y >= 0 &&
                        x + TEXT_CELL_W * s <= fb->w &&
                        y + TEXT_CELL_H * s <= fb->h;
    if (!inside) {
        draw_cell_clipped(fb, x, y, g, fg, bg, opaque, s);
        return;
    }

    uint16_t *row = fb->px + (size_t)y * fb->w + x;

    if (s == 1) {
        for (int gr = 0; gr < TEXT_CELL_H; ++gr, row += fb->w) {
            const uint8_t b = gr < FONT5X8_H ? g[gr] : 0;
            if (opaque) {
                row[0] = (b & 0x10) ? fg : bg;
                row[1] = (b & 0x08) ? fg : bg;
                row[2] = (b & 0x04) ? fg : bg;
                row[3] = (b & 0x02) ? fg : bg;
                row[4] = (b & 0x01) ? fg : bg;
                row[5] = bg;                        // the gap column
            } else if (b) {
                if (b & 0x10) row[0] = fg;
                if (b & 0x08) row[1] = fg;
                if (b & 0x04) row[2] = fg;
                if (b & 0x02) row[3] = fg;
                if (b & 0x01) row[4] = fg;
            }
        }
        return;
    }

    // Scaled. Every one of the `s` screen rows a glyph row covers is identical,
    // so build it once and copy it: the expansion is paid per glyph row, and
    // what repeats is a memcpy.
    uint16_t     line[TEXT_CELL_W * TEXT_MAX_SCALE];
    const int    cw    = TEXT_CELL_W * s;
    const size_t bytes = (size_t)cw * sizeof(uint16_t);

    for (int gr = 0; gr < TEXT_CELL_H; ++gr) {
        const uint8_t b = gr < FONT5X8_H ? g[gr] : 0;

        if (opaque) {
            uint16_t *d = line;
            for (int gc = 0; gc < TEXT_CELL_W; ++gc) {
                const uint16_t c = (gc < FONT5X8_W && (b & (0x10 >> gc))) ? fg : bg;
                for (int sx = 0; sx < s; ++sx) *d++ = c;
            }
            for (int sy = 0; sy < s; ++sy, row += fb->w) memcpy(row, line, bytes);
        } else {
            // Transparent cannot copy a whole row - the gaps must be left alone -
            // but a blank glyph row can be skipped outright.
            if (b) {
                for (int sy = 0; sy < s; ++sy) {
                    uint16_t *r = row + (size_t)sy * fb->w;
                    for (int gc = 0; gc < FONT5X8_W; ++gc) {
                        if (!(b & (0x10 >> gc))) continue;
                        uint16_t *d = r + gc * s;
                        for (int sx = 0; sx < s; ++sx) d[sx] = fg;
                    }
                }
            }
            row += (size_t)s * fb->w;
        }
    }
}

static int text_run(fb_t *fb, int x, int y, const char *s,
                    uint16_t fg, uint16_t bg, bool opaque, int scale)
{
    if (!fb->px || !s) return x;
    if (scale < 1) scale = 1;
    if (scale > TEXT_MAX_SCALE) scale = TEXT_MAX_SCALE;

    const int cw = TEXT_CELL_W * scale;
    const int ch = TEXT_CELL_H * scale;

    int cx = x, cy = y, right = x;

    for (const char *p = s; *p; ++p) {
        if (*p == '\n') { cx = x; cy += ch; continue; }

        // Entirely off screen: skip the cell, but still advance past it, so
        // text that starts off the left edge lands where it would have.
        if (cx < fb->w && cy < fb->h && cx + cw > 0 && cy + ch > 0) {
            draw_cell(fb, cx, cy, glyph_for(*p), fg, bg, opaque, scale);
        }
        cx += cw;
        if (cx > right) right = cx;
    }

    // One region for the lot. fb_damage clips it, so a string running off the
    // screen marks only the part that is on it.
    fb_damage(fb, x, y, right - x, cy + ch - y);
    return cx;
}

int fb_text(fb_t *fb, int x, int y, const char *s, uint16_t fg, int scale)
{
    return text_run(fb, x, y, s, fg, 0, false, scale);
}

int fb_text_bg(fb_t *fb, int x, int y, const char *s,
               uint16_t fg, uint16_t bg, int scale)
{
    return text_run(fb, x, y, s, fg, bg, true, scale);
}

int fb_text_width(const char *s, int scale)
{
    if (!s) return 0;
    if (scale < 1) scale = 1;

    int longest = 0, n = 0;
    for (const char *p = s; *p; ++p) {
        if (*p == '\n') { n = 0; continue; }
        if (++n > longest) longest = n;
    }
    return longest * TEXT_CELL_W * scale;
}

int fb_text_height(const char *s, int scale)
{
    if (!s) return 0;
    if (scale < 1) scale = 1;

    int lines = 1;
    for (const char *p = s; *p; ++p) if (*p == '\n') ++lines;
    return lines * TEXT_CELL_H * scale;
}
