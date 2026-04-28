/*
 * AetherOS libwidget — WIDGET_CHECKBOX (Phase 5.3)
 * File: userspace/lib/libwidget/checkbox.c
 *
 * Boolean toggle: draws a 12×12 box (empty or with a filled check square),
 * followed by the label text.
 *
 * Toggle fires on: MOUSE_UP, KEY_DOWN(ENTER), KEY_DOWN(SPACE).
 * Calls on_toggle(w, new_checked) after state change.
 */

#include <widget.h>
#include <gfx.h>
#include <string.h>

#define CB_BOX  12   /* checkbox square size */
#define CB_GAP   6   /* gap between box and label */

#define C_CB_BG    C_WIN_BG
#define C_CB_BDR   C_SEP
#define C_CB_FILL  C_ACCENT
#define C_CB_MARK  GFX_RGB(255, 255, 255)

static void checkbox_draw(widget_t *w, int ax, int ay)
{
    wdata_checkbox_t *d = &w->data.checkbox;
    int focused = (w->state == WS_FOCUSED || w->state == WS_PRESSED);

    /* Background clear */
    gfx_fill(ax, ay, w->bounds.w, w->bounds.h, C_CB_BG);

    int bx = ax;
    int by = ay + (w->bounds.h - CB_BOX) / 2;

    /* Outer box */
    gfx_rect(bx, by, CB_BOX, CB_BOX, focused ? C_ACCENT : C_CB_BDR);

    if (d->checked) {
        /* Fill inner area with accent */
        gfx_fill(bx + 2, by + 2, CB_BOX - 4, CB_BOX - 4, C_CB_FILL);
        /* Draw a simple check mark: two filled rectangles */
        gfx_fill(bx + 2, by + 5, 3, 2, C_CB_MARK);
        gfx_fill(bx + 4, by + 3, 2, 4, C_CB_MARK);
    }

    /* Label text */
    int tx = ax + CB_BOX + CB_GAP;
    int ty = ay + (w->bounds.h - WGT_FONT_H) / 2;
    gfx_text(tx, ty, d->text, focused ? C_TEXT : C_TEXT_DIM, C_CB_BG);
}

static void cb_toggle(widget_t *w)
{
    wdata_checkbox_t *d = &w->data.checkbox;
    d->checked = !d->checked;
    w->dirty = 1;
    if (d->on_toggle) d->on_toggle(w, d->checked);
}

static int checkbox_event(widget_t *w, const widget_event_t *ev)
{
    if (ev->type == WEV_FOCUS_IN || ev->type == WEV_FOCUS_OUT) {
        w->dirty = 1; return 0;
    }
    if (ev->type == WEV_MOUSE_UP)  { cb_toggle(w); return 1; }
    if (ev->type == WEV_KEY_DOWN &&
        (ev->keycode == KEY_ENTER || ev->keycode == KEY_SPACE)) {
        cb_toggle(w);
        return 1;
    }
    return 0;
}

void widget_init_checkbox(widget_t *w, int x, int y, int width, int height,
                          const char *text,
                          void (*on_toggle)(widget_t *w, int checked))
{
    widget_init(w, WIDGET_CHECKBOX, x, y, width, height);
    w->draw_fn   = checkbox_draw;
    w->event_fn  = checkbox_event;
    w->focusable = 1;

    wdata_checkbox_t *d = &w->data.checkbox;
    int i = 0;
    while (text && text[i] && i < 127) { d->text[i] = text[i]; i++; }
    d->text[i]   = '\0';
    d->checked   = 0;
    d->on_toggle = on_toggle;
}

int checkbox_get_checked(widget_t *w)
{
    return w->data.checkbox.checked;
}

void checkbox_set_checked(widget_t *w, int checked)
{
    w->data.checkbox.checked = checked;
    w->dirty = 1;
}
