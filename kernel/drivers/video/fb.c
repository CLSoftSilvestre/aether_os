/*
 * AetherOS — Framebuffer Drawing Primitives
 * File: kernel/drivers/video/fb.c
 */

#include "drivers/video/fb.h"
#include "aether/types.h"

/* fb_base / fb_width / fb_height / fb_stride are defined in ramfb.c */

void fb_init(void)
{
    /* Nothing to do here — ramfb_init() sets up the globals */
}

void fb_put_pixel(u32 x, u32 y, u32 color)
{
    if (!fb_base || x >= fb_width || y >= fb_height) return;
    fb_base[y * (fb_stride / 4) + x] = color;
}

void fb_fill_rect(u32 x, u32 y, u32 w, u32 h, u32 color)
{
    if (!fb_base) return;
    u32 x1 = x + w;
    u32 y1 = y + h;
    if (x1 > fb_width)  x1 = fb_width;
    if (y1 > fb_height) y1 = fb_height;
    u32 pitch = fb_stride / 4;
    for (u32 row = y; row < y1; row++) {
        volatile u32 *p = fb_base + row * pitch + x;
        for (u32 col = x; col < x1; col++)
            *p++ = color;
    }
}

u32 fb_get_pixel(u32 x, u32 y)
{
    if (!fb_base || x >= fb_width || y >= fb_height) return 0;
    return fb_base[y * (fb_stride / 4) + x];
}

void fb_blit(const u32 *src, u32 x, u32 y, u32 w, u32 h, u32 src_stride)
{
    if (!fb_base) return;
    u32 pitch = fb_stride / 4;
    for (u32 row = 0; row < h && (y + row) < fb_height; row++) {
        const u32 *s = src + row * (src_stride / 4);
        volatile u32 *d = fb_base + (y + row) * pitch + x;
        for (u32 col = 0; col < w && (x + col) < fb_width; col++)
            d[col] = s[col];
    }
}
