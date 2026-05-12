/*
 * AetherOS libwidget — WIDGET_LABEL (Phase 5.3)
 * File: userspace/lib/libwidget/label.c
 *
 * Static text label — no interaction.  Supports LEFT/CENTER/RIGHT alignment.
 * Text is rendered on a single line; truncated at widget width.
 */

#include <widget.h>
#include <gfx.h>
#include <string.h>

static void label_draw(widget_t *w, int ax, int ay)
{
    /* Clear background (inherit parent's bg — panel redraws behind us) */
    gfx_fill(ax, ay, w->bounds.w, w->bounds.h, C_WIN_BG);

    int y = ay + (w->bounds.h - WGT_FONT_H) / 2;
    const char *s = w->data.label.text;

    switch (w->data.label.align) {
    case WGT_ALIGN_CENTER:
        gfx_text_center(ax, w->bounds.w, y, s, C_TEXT, C_WIN_BG);
        break;
    case WGT_ALIGN_RIGHT: {
        int tx = ax + w->bounds.w - gfx_text_width(s);
        if (tx < ax) tx = ax;
        gfx_text(tx, y, s, C_TEXT, C_WIN_BG);
        break;
    }
    default: /* LEFT */
        gfx_text(ax, y, s, C_TEXT, C_WIN_BG);
        break;
    }
}

void widget_init_label(widget_t *w, int x, int y, int width, int height,
                       const char *text, int align)
{
    widget_init(w, WIDGET_LABEL, x, y, width, height);
    w->draw_fn   = label_draw;
    w->event_fn  = NULL;
    w->focusable = 0;
    w->data.label.align = align;

    int i = 0;
    while (text && text[i] && i < WGT_LABEL_MAX - 1) {
        w->data.label.text[i] = text[i];
        i++;
    }
    w->data.label.text[i] = '\0';
}

void label_set_text(widget_t *w, const char *text)
{
    int i = 0;
    while (text && text[i] && i < WGT_LABEL_MAX - 1) {
        w->data.label.text[i] = text[i];
        i++;
    }
    w->data.label.text[i] = '\0';
    w->dirty = 1;
}
