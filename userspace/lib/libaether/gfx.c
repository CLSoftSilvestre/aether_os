/*
 * AetherOS — Userspace Graphics Library
 * File: userspace/lib/libaether/gfx.c
 */

#include <gfx.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys.h>

#define FONT_W  8
#define FONT_H  8

static unsigned g_width;
static unsigned g_height;

void gfx_init(void)
{
    /* QEMU ramfb is fixed 1024×768 — hardcode for now.
     * Phase 4.2 will expose sys_fb_info for dynamic query. */
    g_width  = 1024;
    g_height = 768;
}

unsigned gfx_width(void)  { return g_width;  }
unsigned gfx_height(void) { return g_height; }
long     gfx_ticks(void)  { return sys_get_ticks(); }

void gfx_fill(unsigned x, unsigned y, unsigned w, unsigned h, unsigned color)
{
    sys_fb_fill(x, y, w, h, color);
}

void gfx_hline(unsigned x, unsigned y, unsigned w, unsigned color)
{
    sys_fb_fill(x, y, w, 1, color);
}

void gfx_vline(unsigned x, unsigned y, unsigned h, unsigned color)
{
    sys_fb_fill(x, y, 1, h, color);
}

void gfx_rect(unsigned x, unsigned y, unsigned w, unsigned h, unsigned color)
{
    gfx_hline(x,         y,         w, color);   /* top    */
    gfx_hline(x,         y + h - 1, w, color);   /* bottom */
    gfx_vline(x,         y,         h, color);   /* left   */
    gfx_vline(x + w - 1, y,         h, color);   /* right  */
}

void gfx_char(unsigned x, unsigned y, char ch, unsigned fg, unsigned bg)
{
    sys_fb_char(x, y, (unsigned char)ch, fg, bg);
}

void gfx_text(unsigned x, unsigned y, const char *s, unsigned fg, unsigned bg)
{
    unsigned cx = x;
    for (; *s; s++) {
        gfx_char(cx, y, *s, fg, bg);
        cx += FONT_W;
    }
}

void gfx_text_center(unsigned cx, unsigned cw, unsigned y,
                     const char *s, unsigned fg, unsigned bg)
{
    unsigned len = (unsigned)strlen(s);
    unsigned text_w = len * FONT_W;
    unsigned x = cx + (cw > text_w ? (cw - text_w) / 2 : 0);
    gfx_text(x, y, s, fg, bg);
}

void gfx_printf(unsigned x, unsigned y, unsigned fg, unsigned bg,
                const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    gfx_text(x, y, buf, fg, bg);
}
