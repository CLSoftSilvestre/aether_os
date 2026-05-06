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

/* Draw one character (8×16 Lumina Mono) at pixel (x, y) */
void gfx_char(unsigned x, unsigned y, char ch, unsigned fg, unsigned bg);

/* Draw a null-terminated string on a single line */
void gfx_text(unsigned x, unsigned y, const char *s, unsigned fg, unsigned bg);

/* Draw string centered horizontally within [cx, cx+cw) */
void gfx_text_center(unsigned cx, unsigned cw, unsigned y,
                     const char *s, unsigned fg, unsigned bg);

/* Transparent variants — draw only foreground pixels; background unchanged.
 * Use over glass / image surfaces to preserve the glass effect. */
void gfx_char_transparent(unsigned x, unsigned y, char ch, unsigned fg);
void gfx_text_transparent(unsigned x, unsigned y, const char *s, unsigned fg);
void gfx_text_center_transparent(unsigned cx, unsigned cw, unsigned y,
                                  const char *s, unsigned fg);

/* Draw a formatted string (printf-style) — uses a 256-byte stack buffer */
void gfx_printf(unsigned x, unsigned y, unsigned fg, unsigned bg,
                const char *fmt, ...);

/*
 * Draw the traffic-light close button (12×12) at pixel (x, y).
 * hovered=1 draws a brighter red for hover feedback.
 * Corners are clipped with C_TITLEBAR to approximate a circle.
 */
void gfx_draw_close_button(unsigned x, unsigned y, int hovered);

/* ── Desktop icon primitives (48×48, Phase 5.4) ────────────────────────── */
void gfx_icon_term(int x, int y);
void gfx_icon_files(int x, int y);
void gfx_icon_editor(int x, int y);
void gfx_icon_generic(int x, int y, const char *label);
void gfx_icon_tictactoe(int x, int y);
void gfx_icon_telnet(int x, int y);

/* ── Scalable file/drive icons (Phase 5.6) ─────────────────────────────── *
 * Each function draws a sz×sz icon at pixel (x, y).
 * sz = 14 for tree rows, sz = 48 for icon grid.
 */
void gfx_icon_drive_fat32(int x, int y, int sz);
void gfx_icon_drive_initrd(int x, int y, int sz);
void gfx_icon_drive_afs(int x, int y, int sz);
void gfx_icon_folder(int x, int y, int sz);
void gfx_icon_folder_open(int x, int y, int sz);
void gfx_icon_file_txt(int x, int y, int sz);
void gfx_icon_file_as(int x, int y, int sz);
void gfx_icon_file_exec(int x, int y, int sz);
void gfx_icon_file_generic(int x, int y, int sz);

/* ── BMP wallpaper support ──────────────────────────────────────────────── */

/*
 * Load a 32-bpp uncompressed BMP from VFS into pixels[] (XRGB8888,
 * row-major, top-to-bottom).  buf_bytes must be >= width*height*4.
 * out_w / out_h receive BMP dimensions (may be NULL).
 * Returns 0 on success, -1 on error (file not found, wrong format, etc.)
 */
int gfx_bmp_load(const char *path, unsigned *pixels, unsigned buf_bytes,
                  unsigned *out_w, unsigned *out_h);

/*
 * Blit a center-cropped sub-region of a loaded BMP pixel buffer to the
 * framebuffer at (dst_x, dst_y) with size (dst_w, dst_h).
 * The BMP is center-cropped when larger than the screen.
 */
void gfx_bmp_blit_region(const unsigned *pixels, unsigned bmp_w, unsigned bmp_h,
                           unsigned dst_x, unsigned dst_y,
                           unsigned dst_w, unsigned dst_h);

#endif /* _GFX_H */
