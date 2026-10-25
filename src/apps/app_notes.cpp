// Notes: the first thing on this board that can be typed into without a
// computer attached (stage 6.4a).
//
//   UP DOWN LEFT RIGHT   move over the keys, wrapping; held, they repeat
//   OK                   type the key under the cursor
//   BACK (a tap)         delete a character
//   Done                 write the line to the note file
//
// Holding BACK still closes the app - that is the system's rule, not this
// app's - so there is a Del key on the grid for anyone who would rather not
// risk the hold.
//
// The note is one line in the app's own folder, /fs/apps/notes/note, written
// whole each time (fs_write replaces atomically, so a note is never half
// old and half new). It is read back when the app opens, which is also the
// proof that what was typed really reached the flash.

#include "apps/apps.h"

#include <string.h>

#include "gfx/text.h"
#include "os/fs.h"
#include "os/kfmt.h"
#include "os/syslog.h"
#include "board/input.h"
#include "ui/keyboard.h"
#include "ui/ui.h"

#define NOTE_FILE "note"

typedef struct {
    ui_kb_t kb;
    char    msg[48];
    int     saves;
} state_t;

static void init(app_ctx_t *c)
{
    state_t *s = (state_t *)c->user;
    char buf[UI_KB_MAX + 1];
    size_t got = 0;

    const int rc = fs_read(c, NOTE_FILE, buf, UI_KB_MAX, &got);
    if (rc == FS_OK && got) {
        buf[got] = 0;
        ui_kb_init(&s->kb, buf);
        k_snprintf(s->msg, sizeof(s->msg), "%u characters read back from flash", (unsigned)got);
    } else {
        ui_kb_init(&s->kb, nullptr);
        k_snprintf(s->msg, sizeof(s->msg), "%s", rc == FS_ERR_NOENT ? "no note yet - type one"
                                                                    : "could not read the note");
    }
}

static void on_event(app_ctx_t *c, const ev_t *ev)
{
    state_t *s = (state_t *)c->user;
    if (ev->type != EV_INPUT) return;

    if (!ui_kb_event(&s->kb, 1u << ev->b, ev->a)) return;

    if (s->kb.done) {
        s->kb.done = false;
        const int rc = fs_write(c, NOTE_FILE, s->kb.text, strlen(s->kb.text));
        if (rc == FS_OK) {
            s->saves++;
            k_snprintf(s->msg, sizeof(s->msg), "saved %u characters (%d time%s)",
                       (unsigned)strlen(s->kb.text), s->saves, s->saves == 1 ? "" : "s");
            app_log(c, "note saved, %u characters", (unsigned)strlen(s->kb.text));
        } else {
            k_snprintf(s->msg, sizeof(s->msg), "could not save: %d", rc);
        }
    } else if (s->kb.full) {
        k_snprintf(s->msg, sizeof(s->msg), "that is all it holds (%d characters)", UI_KB_MAX);
    }
    app_invalidate(c);
}

static void draw(app_ctx_t *c, fb_t *fb, bool full)
{
    state_t *s = (state_t *)c->user;
    const uint16_t bg = app_bg();

    if (full) {
        ui_title(fb, 4, "Notes");
        fb_text(fb, 210, 14, "BACK deletes", FB_GREY, 1);
    }
    // What OK will do, in words. A grid of keys does not say which one the
    // cursor is on if the screen is read from across a room - and it is the
    // one thing someone about to press OK wants to know.
    {
        char line[32];
        k_snprintf(line, sizeof(line), "OK types: %-6s", ui_kb_current_name(&s->kb));
        fb_text_bg(fb, 120, 4, line, FB_WHITE, bg, 1);
    }
    ui_kb_draw(fb, &s->kb, 40, "the note");

    fb_fill_rect(fb, 6, 66, fb->w - 12, TEXT_CELL_H, bg);
    fb_text(fb, 8, 66, s->msg, s->kb.full ? FB_YELLOW : FB_GREEN, 1);
}

const app_t app_notes = {
    .name      = "notes",
    .about     = "type a line and keep it on the flash",
    .user_size = sizeof(state_t),
    .init      = init,
    .on_event  = on_event,
    .draw      = draw,
};
APP_REGISTER(app_notes, 90);

// For the test script.
const char *app_notes_text(const app_ctx_t *c)
{
    return c && c->user ? ((const state_t *)c->user)->kb.text : "";
}

const char *app_notes_message(const app_ctx_t *c)
{
    return c && c->user ? ((const state_t *)c->user)->msg : "";
}
