#include "ui/ui.h"

#include "gfx/text.h"
#include "os/app.h"
#include "os/kfmt.h"

// The margin every list row has used since stage 4.3d: four pixels either
// side of the app area.
#define UI_MARGIN 4

static int row_height(const ui_rows_t *r, int i)
{
    return r->height ? r->height(r->ctx, i) : r->row_h;
}

static bool row_selectable(const ui_rows_t *r, int i)
{
    return r->selectable ? r->selectable(r->ctx, i) : true;
}

void ui_list_move(ui_list_t *l, const ui_rows_t *rows, int step)
{
    const int n = rows->n;
    if (n <= 0) { l->sel = 0; l->top = 0; return; }

    if (l->sel >= n) l->sel = n - 1;
    if (l->sel < 0)  l->sel = 0;

    // Step, then carry on the same way over anything that cannot be chosen.
    // step 0 stays where it is, unless the rows have changed under it and it
    // is now on a header - then it moves on to the next row that can be
    // chosen. A list of nothing but headers leaves the selection alone.
    const int dir = step < 0 ? -1 : 1;
    int i = l->sel + step;
    for (int guard = 0; guard <= n; ++guard) {
        i = ((i % n) + n) % n;
        if (row_selectable(rows, i)) { l->sel = i; break; }
        i += dir;
    }

    if (l->top > l->sel) l->top = l->sel;
    if (l->top < 0) l->top = 0;
    // Scroll down one row at a time until the chosen row's whole height fits.
    for (int guard = 0; guard < n; ++guard) {
        int y = rows->y;
        for (int k = l->top; k <= l->sel && k < n; ++k) y += row_height(rows, k);
        if (y <= rows->y_end || l->top >= l->sel) break;
        l->top++;
    }
}

ui_iter_t ui_list_begin(const ui_list_t *l, const ui_rows_t *rows)
{
    ui_iter_t it = { rows, l->top, rows->y, 0, false };
    return it;
}

bool ui_list_next(ui_iter_t *it)
{
    if (it->started) { it->y += it->h; it->i++; }
    it->started = true;
    if (it->i < 0 || it->i >= it->rows->n) return false;
    it->h = row_height(it->rows, it->i);
    return it->y + it->h <= it->rows->y_end;
}

int ui_list_bottom(const ui_list_t *l, const ui_rows_t *rows)
{
    // The same walk ui_list_next does, so it is that walk: the rule about a
    // row that does not fit lives in one place and cannot drift.
    int y = rows->y;
    for (ui_iter_t it = ui_list_begin(l, rows); ui_list_next(&it); ) y = it.y + it.h;
    return y;
}

void ui_scroll(int *top, int n, int visible, int step)
{
    const int last = n > visible ? n - visible : 0;
    int t = *top + step;
    if (t > last) t = last;
    if (t < 0) t = 0;
    *top = t;
}

uint16_t ui_hl(void)
{
    return fb_rgb(40, 80, 120);
}

void ui_clear(fb_t *fb, const image_t *bg, int x, int y, int w, int h)
{
    if (image_ok(bg)) image_blit_rect(fb, bg, x, y, w, h);
    else              fb_fill_rect(fb, x, y, w, h, app_bg());
}

void ui_row_bg(fb_t *fb, const image_t *bg, int y, int h, bool selected)
{
    const int x = UI_MARGIN, w = fb->w - 2 * UI_MARGIN;
    if (!image_ok(bg)) {
        fb_fill_rect(fb, x, y, w, h, selected ? ui_hl() : app_bg());
        return;
    }
    image_blit_rect(fb, bg, x, y, w, h);
    if (selected) fb_blend_rect(fb, x, y, w, h, ui_hl(), UI_SELECT_PCT);
}

int ui_text(fb_t *fb, int x, int y, const char *s, uint16_t colour, int scale, bool shadow)
{
    if (shadow) fb_text(fb, x + 1, y + 1, s, fb_rgb(8, 10, 14), scale);
    return fb_text(fb, x, y, s, colour, scale);
}

void ui_title(fb_t *fb, int y, const char *title)
{
    ui_title_on(fb, y, title, false);
}

void ui_help(fb_t *fb, int y, const char *line)
{
    ui_help_on(fb, y, line, false);
}

void ui_title_on(fb_t *fb, int y, const char *title, bool shadow)
{
    ui_text(fb, 8, y, title, FB_CYAN, 2, shadow);
}

void ui_help_on(fb_t *fb, int y, const char *line, bool shadow)
{
    // Grey is right on the flat background and too dark on a picture.
    ui_text(fb, 8, y, line, shadow ? fb_rgb(200, 210, 222) : FB_GREY, 1, shadow);
}

void ui_bar(fb_t *fb, int x, int y, int w, int h, int32_t v, int32_t lo, int32_t hi,
            uint16_t frame, uint16_t fill, uint16_t back)
{
    const int inner = w - 2;
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    int f = (hi > lo) ? (int)((int64_t)inner * (v - lo) / (hi - lo)) : 0;
    if (f < 0) f = 0;
    if (f > inner) f = inner;

    fb_rect(fb, x, y, w, h, frame);
    fb_fill_rect(fb, x + 1, y + 1, f, h - 2, fill);
    fb_fill_rect(fb, x + 1 + f, y + 1, inner - f, h - 2, back);
}

bool ui_confirm(ui_confirm_t *cf, int id, const char *key, const char *what,
                char *msg, size_t msg_len)
{
    if (cf->armed && cf->id == id) {
        cf->armed = false;
        return true;
    }
    cf->armed = true;
    cf->id    = id;
    k_snprintf(msg, msg_len, "%s again to %s", key, what);
    return false;
}

bool ui_confirm_cancel(ui_confirm_t *cf, char *msg, size_t msg_len, const char *cancelled)
{
    if (!cf->armed) return false;
    cf->armed = false;
    if (!msg || !msg_len) return true;          // disarm only: the app's own
    if (cancelled) k_snprintf(msg, msg_len, "%s", cancelled);   // message stands
    else msg[0] = 0;
    return true;
}
