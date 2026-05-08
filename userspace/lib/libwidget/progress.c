/*
 * AetherOS libwidget - WIDGET_PROGRESS
 * File: userspace/lib/libwidget/progress.c
 */

 #include <widget.h>
 #include <gfx.h>
 #include <string.h>

 #define PROGRESS_BAR_HEIGHT 4
 #define LOADING_CHUNK_W     30

 static void progress_draw(widget_t *w, int ax, int ay) {
    // 1. Draw the background/Track
    gfx_fill(ax, ay, w->bounds.w, w->bounds.h, C_WIN_BG); //Clear
    gfx_rect(ax, ay, w->bounds.w, w->bounds.h, C_SEP); //Border

    int inner_x = ax + 2;
    int inner_y = ay + 2;
    int inner_w = w->bounds.w - 4;
    int inner_h = w->bounds.h - 4;

    if (w->data.progress.mode == WGT_PROGRESS_DETERMINATE) {
        // Mode 1: Actual Progress
        int fill_w = (w->data.progress.value * inner_w) / 100;
        if (fill_w > inner_w) fill_w = inner_w;

        gfx_fill(inner_x, inner_y, fill_w, inner_h, C_ACCENT);
    }
    else {
        // Mode 2: Loading Indicator (Marquee/Scrooling)
        // we use the 'value' field as the offset for the animation
        int offset = w->data.progress.value % inner_w;

        // Draw the moving chunk
        gfx_fill(inner_x + offset, inner_y, LOADING_CHUNK_W, inner_h, C_ACCENT);

        // If chunck is wrapping around the right edge, draw the remaining on the left
        if (offset + LOADING_CHUNK_W > inner_w) {
            int wrapped_w = (offset + LOADING_CHUNK_W) - inner_w;
            gfx_fill(inner_x, inner_y, wrapped_w, inner_h, C_ACCENT);
        }
    }
 }

 static int progress_event(widget_t *w, const widget_event_t *ev) {
    if (ev->type == WEV_TICK && w->data.progress.mode == WGT_PROGRESS_INDETERMINATE) {
        // Advance the animation offset
        w->data.progress.value = (w->data.progress.value + 2) % w->bounds.w;
        w->dirty = 1;
        return 1; // Event handled
    }
    return 0; // Event not handled
 }

 void widget_init_progress(widget_t *w, int x, int y, int width, int height, int mode) {
    widget_init(w, WIDGET_PROGRESS, x, y, width, height);
    w->draw_fn = progress_draw;
    w->event_fn = progress_event;
    w->focusable = 1; // Needs focus/tick events for animation
    w->data.progress.mode = mode;
    w->data.progress.value = 0;
 }

 void progress_set_value(widget_t *w, int value) {
    if (w->data.progress.mode == WGT_PROGRESS_DETERMINATE) {
        if (value <0) value = 0;
        if (value > 100) value = 100;
    }
    w->data.progress.value = value;
    w->dirty = 1;
 }

