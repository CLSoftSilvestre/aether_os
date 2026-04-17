#ifndef _GFX_H
#define _GFX_H

/*
 * AetherOS — Userspace Graphics Library
 *
 * Thin wrappers around sys_fb_fill / sys_fb_char that give a convenient
 * drawing API for user-space applications.  All coordinates are in pixels.
 */

#include <stddef.h>

/* Pack RGB into 32-bpp XRGB8888 */
#define GFX_RGB(r,g,b)   (((unsigned)(r) << 16) | ((unsigned)(g) << 8) | (unsigned)(b))

/* ── Lumina color palette ──────────────────────────────────────────── */
#define C_DESKTOP    GFX_RGB( 18,  18,  24)   /* near-black background    */
#define C_PANEL      GFX_RGB( 26,  26,  40)   /* panel / bar backgrounds  */
#define C_WIN_BG     GFX_RGB( 20,  20,  32)   /* window background        */
#define C_TITLEBAR   GFX_RGB( 30,  30,  50)   /* title bar                */
#define C_TERM_BG    GFX_RGB( 12,  12,  20)   /* terminal background      */
#define C_ACCENT     GFX_RGB(124, 106, 247)   /* purple accent            */
#define C_ACCENT2    GFX_RGB(  0, 200, 220)   /* cyan secondary           */
#define C_TEXT       GFX_RGB(216, 216, 232)   /* primary text             */
#define C_TEXT_DIM   GFX_RGB(100, 100, 140)   /* dimmed text              */
#define C_SEP        GFX_RGB( 50,  50,  80)   /* separator lines          */
#define C_RED        GFX_RGB(235,  87,  87)   /* traffic light close      */
#define C_YELLOW     GFX_RGB(247, 201,  72)   /* traffic light minimize   */
#define C_GREEN      GFX_RGB( 80, 200,  75)   /* traffic light maximize   */

/* ── Framebuffer info ───────────────────────────────────────────────── */
typedef struct { unsigned width, height; } gfx_info_t;

void       gfx_init(void);        /* query and cache fb dimensions */
unsigned   gfx_width(void);
unsigned   gfx_height(void);
long       gfx_ticks(void);       /* 100Hz ticks since boot */

/* ── Drawing primitives ─────────────────────────────────────────────── */
void gfx_fill(unsigned x, unsigned y, unsigned w, unsigned h, unsigned color);
void gfx_hline(unsigned x, unsigned y, unsigned w, unsigned color);
void gfx_vline(unsigned x, unsigned y, unsigned h, unsigned color);
void gfx_rect(unsigned x, unsigned y, unsigned w, unsigned h, unsigned color);

/* Draw one character (8×8) at pixel (x, y) */
void gfx_char(unsigned x, unsigned y, char ch, unsigned fg, unsigned bg);

/* Draw a null-terminated string on a single line */
void gfx_text(unsigned x, unsigned y, const char *s, unsigned fg, unsigned bg);

/* Draw string centered horizontally within [cx, cx+cw) */
void gfx_text_center(unsigned cx, unsigned cw, unsigned y,
                     const char *s, unsigned fg, unsigned bg);

/* Draw a formatted string (printf-style) — uses a 256-byte stack buffer */
void gfx_printf(unsigned x, unsigned y, unsigned fg, unsigned bg,
                const char *fmt, ...);

#endif /* _GFX_H */
