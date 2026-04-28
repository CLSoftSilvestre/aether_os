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
    gfx_fill(ax, ay, w->bounds.w, w->bounds.h,
             w->data.panel.bg_color);
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
