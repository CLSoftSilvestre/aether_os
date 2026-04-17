#ifndef DRIVERS_VIDEO_FB_H
#define DRIVERS_VIDEO_FB_H

/*
 * AetherOS — Framebuffer Abstraction
 *
 * A 32-bpp linear framebuffer at a physical address provided by ramfb_init().
 * All coordinates are in pixels; (0,0) is top-left.
 */

#include "aether/types.h"

/* Pack RGB into 32-bpp XRGB8888 (X bits ignored by display) */
#define FB_RGB(r, g, b)  ((u32)(((r) << 16) | ((g) << 8) | (b)))

/* Common colors */
#define FB_BLACK    FB_RGB(0,   0,   0)
#define FB_WHITE    FB_RGB(255, 255, 255)
#define FB_GRAY     FB_RGB(128, 128, 128)
#define FB_RED      FB_RGB(220,  50,  50)
#define FB_GREEN    FB_RGB( 50, 220,  50)
#define FB_BLUE     FB_RGB( 50,  50, 220)
#define FB_CYAN     FB_RGB(  0, 200, 220)
#define FB_YELLOW   FB_RGB(255, 220,   0)

/* ── Public API ──────────────────────────────────────────────────────── */

/* Call after ramfb_init() sets fb_base / fb_width / fb_height / fb_stride */
void fb_init(void);

void fb_put_pixel(u32 x, u32 y, u32 color);
void fb_fill_rect(u32 x, u32 y, u32 w, u32 h, u32 color);

/* Copy a w×h block from src (row-major, src_stride bytes/row) to (x,y) */
void fb_blit(const u32 *src, u32 x, u32 y, u32 w, u32 h, u32 src_stride);

/* Framebuffer geometry (set by ramfb_init) */
extern volatile u32  *fb_base;
extern u32            fb_width;
extern u32            fb_height;
extern u32            fb_stride;   /* bytes per row */

#endif /* DRIVERS_VIDEO_FB_H */
