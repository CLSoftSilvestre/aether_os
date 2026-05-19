/*
 * plot_aether.h — Phase 7.7 NetSurf plotter for AetherOS
 *
 * The plotter renders HTML/CSS content into an off-screen XRGB8888 pixel
 * buffer owned by the caller.  Wire it into a browser redraw like this:
 *
 *   aether_plot_ctx_t ctx;
 *   aether_plot_ctx_init(&ctx, pixels, width, height);
 *
 *   struct redraw_context rctx = {
 *       .interactive      = true,
 *       .background_images = true,
 *       .plot             = &aether_plotter_table,
 *       .priv             = &ctx,
 *   };
 *   struct rect r = { 0, 0, width, height };
 *   browser_window_redraw(bw, 0, 0, &r, &rctx);
 *
 *   gfx_raw_blit(pixels, width, screen_x, screen_y, width, height);
 */

#ifndef PLOT_AETHER_H
#define PLOT_AETHER_H

#include <stdint.h>
#include <stdbool.h>

/* forward — defined in netsurf/types.h */
struct rect;
struct plotter_table;

/*
 * Off-screen render context.  Passed as redraw_context.priv.
 * clip is updated by the plot_clip callback on each repaint.
 */
typedef struct aether_plot_ctx {
    uint32_t   *pixels;  /* XRGB8888 buffer, width×height uint32_t words */
    int         stride;  /* row stride in pixels (usually == width)        */
    int         width;
    int         height;
    struct rect  clip;   /* current clip rectangle — set by plot_clip      */
} aether_plot_ctx_t;

/*
 * The plotter_table — set redraw_context.plot to this before calling
 * browser_window_redraw().
 */
extern const struct plotter_table aether_plotter_table;

/*
 * Initialise *ctx for a full-buffer repaint.
 * Fills the clip rectangle to cover [0,0]–[width,height].
 * Does NOT clear the pixel buffer — caller fills or browser does it.
 */
void aether_plot_ctx_init(aether_plot_ctx_t *ctx,
                           uint32_t *pixels, int width, int height);

#endif /* PLOT_AETHER_H */
