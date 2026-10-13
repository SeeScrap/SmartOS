#pragma once
#include "gfx/fb.h"

// ---------------------------------------------------------------------------
// Stage 3.5 - text.
//
// A 5x8 bitmap font drawn in a 6x10 cell, scaled by whole numbers. At scale 1
// that is 53 columns by 24 lines on a 320x240 panel; at scale 2, 26 by 12.
// The glyphs live in tools/font5x8.txt as pictures and are compiled into
// font5x8.h by tools/gen_font.py - edit the pictures, never the header.
//
// ONE DAMAGE REGION PER CALL
//
// Text writes pixels directly rather than through fb_pixel(), and that is the
// design decision in this file, not an optimisation. fb_pixel() marks damage
// per pixel, so a forty-character string would feed some 2,400 one-pixel
// rectangles through the merge logic in stage 3.3 - correct in the end, and
// thousands of times more work than the drawing itself. Here the whole string
// is marked once, as its bounding box.
//
// OPAQUE OR TRANSPARENT
//
// fb_text_bg() paints the whole cell, gaps included, so rewriting a number in
// place leaves nothing of the previous one behind - the normal case for a
// status line or a counter. fb_text() paints ink only, for text over a picture;
// changing it means redrawing whatever was underneath first.
// ---------------------------------------------------------------------------

#define TEXT_CELL_W 6
#define TEXT_CELL_H 10

// Largest scale drawn; anything above is drawn at this. 8 is a 48x80 cell,
// a third of the screen's height per line, which is already past any use.
// The limit exists because a scaled row is expanded into a buffer on the stack.
#define TEXT_MAX_SCALE 8

// Both return the x just past the last character drawn, so calls can be
// chained along a line. '\n' returns to the starting x one line down.
// Characters outside printable ASCII draw as '?'. Scale is clamped to
// 1..TEXT_MAX_SCALE.
int fb_text(fb_t *fb, int x, int y, const char *s, uint16_t fg, int scale);
int fb_text_bg(fb_t *fb, int x, int y, const char *s,
               uint16_t fg, uint16_t bg, int scale);

// Size of the box a string will occupy - its longest line, and every line.
int fb_text_width(const char *s, int scale);
int fb_text_height(const char *s, int scale);
