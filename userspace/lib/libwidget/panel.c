/*
 * AetherOS libwidget — WIDGET_PANEL (Phase 5.3)
 * File: userspace/lib/libwidget/panel.c
 *
 * Panel is a transparent container that clips and groups children.
 * It fills its rect with bg_color and has no event handling itself.
 */

#include <widget.h>
#include <gfx.h>
#include <string.h>

static void panel_draw(widget_t *w, int ax, int ay)
{
    unsigned bg = w->data.panel.bg_color;

    /* Rounded glass panel — r=6 for inner widgets, stays small enough for
     * narrow panels while giving a clearly rounded glass look. */
    gfx_fill_rounded((unsigned)ax, (unsigned)ay,
                     (unsigned)w->bounds.w, (unsigned)w->bounds.h,
                     GFX_WIDGET_R, bg);

    /* Subtle glass border — dim accent so the panel edge is softly visible */
    gfx_rect_rounded((unsigned)ax, (unsigned)ay,
                     (unsigned)w->bounds.w, (unsigned)w->bounds.h,
                     GFX_WIDGET_R, C_SEP);

    /* 1-px inner highlight on top edge — simulates glass catching light */
    gfx_hline((unsigned)(ax + GFX_WIDGET_R), (unsigned)ay,
              (unsigned)(w->bounds.w - 2 * GFX_WIDGET_R),
              GFX_RGB(55, 52, 85));
}

void widget_init_panel(widget_t *w, int x, int y, int width, int height,
                       unsigned int bg_color)
{
    widget_init(w, WIDGET_PANEL, x, y, width, height);
    w->draw_fn               = panel_draw;
    w->event_fn              = NULL;
    w->focusable             = 0;
    w->data.panel.bg_color   = bg_color;
}
