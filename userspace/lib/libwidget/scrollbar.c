/*
 * AetherOS libwidget — WIDGET_SCROLLBAR (Phase 5.3)
 * File: userspace/lib/libwidget/scrollbar.c
 *
 * Standalone scrollbar widget.  Typically used as a companion to a textarea
 * or custom list; listview has its own built-in scrollbar indicator.
 *
 * Orientation 0 = vertical, 1 = horizontal.
 * Value range: 0..max.  Thumb size = (page / (max + page)) * track.
 */

#include <widget.h>
#include <gfx.h>

#define C_SB_BG    GFX_RGB( 30,  30,  48)
#define C_SB_THUMB GFX_RGB( 70,  70, 110)
#define C_SB_THUMBF GFX_RGB(100, 100, 160)  /* focused */

static void scrollbar_draw(widget_t *w, int ax, int ay)
{
    wdata_scrollbar_t *d = &w->data.scrollbar;
    int focused = (w->state == WS_FOCUSED);

    gfx_fill(ax, ay, w->bounds.w, w->bounds.h, C_SB_BG);

    if (d->max <= 0) return;

    int total = d->max + d->page;
    if (total <= 0) return;

    if (d->orientation == 0) {
        /* Vertical */
        int track = w->bounds.h;
        int thumb_h = track * d->page / total;
        if (thumb_h < 6) thumb_h = 6;
        int thumb_y = ay + (track - thumb_h) * d->value / d->max;
        gfx_fill(ax + 1, thumb_y, w->bounds.w - 2, thumb_h,
                 focused ? C_SB_THUMBF : C_SB_THUMB);
    } else {
        /* Horizontal */
        int track = w->bounds.w;
        int thumb_w = track * d->page / total;
        if (thumb_w < 6) thumb_w = 6;
        int thumb_x = ax + (track - thumb_w) * d->value / d->max;
        gfx_fill(thumb_x, ay + 1, thumb_w, w->bounds.h - 2,
                 focused ? C_SB_THUMBF : C_SB_THUMB);
    }
}

static int scrollbar_event(widget_t *w, const widget_event_t *ev)
{
    (void)w; (void)ev;
    return 0;
}

void widget_init_scrollbar_v(widget_t *w, int x, int y, int width, int height,
                             int max, int page)
{
    widget_init(w, WIDGET_SCROLLBAR, x, y, width, height);
    w->draw_fn   = scrollbar_draw;
    w->event_fn  = scrollbar_event;
    w->focusable = 0;

    w->data.scrollbar.orientation = 0;
    w->data.scrollbar.value       = 0;
    w->data.scrollbar.max         = max;
    w->data.scrollbar.page        = page;
}

void widget_init_scrollbar_h(widget_t *w, int x, int y, int width, int height,
                             int max, int page)
{
    widget_init_scrollbar_v(w, x, y, width, height, max, page);
    w->data.scrollbar.orientation = 1;
}
