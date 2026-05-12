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

/* ── Off-screen render target (eliminates widget flicker) ───────────────── */
/*
 * gfx_begin_frame / gfx_end_frame implement double-buffering for widget
 * windows.  While a frame is active, gfx_fill() and gfx_text() write into
 * buf instead of the live framebuffer.  gfx_end_frame() blits buf to the
 * screen in a single sys_fb_blit(), so the user only ever sees completed
 * frames — no intermediate blank-background / text-appearing flicker.
 *
 *   buf     : XRGB8888 pixel array, w×h pixels, caller-owned
 *   w, h    : dimensions of buf (= content area of the window)
 *   off_x/y : screen position of buf's top-left corner
 *
 * Only gfx_fill() and gfx_text() (and anything that calls them) are
 * redirected.  gfx_char() always writes directly to the framebuffer so
 * the terminal renderer is unaffected.
 */
void gfx_begin_frame(unsigned *buf, unsigned w, unsigned h, int off_x, int off_y);
void gfx_end_frame(void);

/* ── Drawing primitives ─────────────────────────────────────────────── */
void gfx_fill(unsigned x, unsigned y, unsigned w, unsigned h, unsigned color);
void gfx_hline(unsigned x, unsigned y, unsigned w, unsigned color);
void gfx_vline(unsigned x, unsigned y, unsigned h, unsigned color);
void gfx_rect(unsigned x, unsigned y, unsigned w, unsigned h, unsigned color);

/* ── Rounded rectangle primitives ──────────────────────────────────────────── */

/* Standard window corner radius — used by gfx_glass_window_frame and widgets */
#define GFX_WINDOW_R   10
#define GFX_WIDGET_R    5
#define GFX_INPUT_R     4

/* Fill a rounded rectangle.  Corner pixels outside the arc are left untouched,
 * revealing whatever was drawn behind the window (wallpaper / desktop). */
void gfx_fill_rounded(unsigned x, unsigned y, unsigned w, unsigned h,
                       unsigned r, unsigned color);

/* Draw a 1-pixel rounded rectangle outline. */
void gfx_rect_rounded(unsigned x, unsigned y, unsigned w, unsigned h,
                       unsigned r, unsigned color);

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

/* ── FreeType text metrics (Phase 7.2.4) ────────────────────────────────── */
/*
 * Pixel width of s at the current UI font size.
 * Returns strlen(s) * 8 when FreeType is not available.
 */
int gfx_text_width(const char *s);

/*
 * Pixel width of the first n bytes of s.
 * Useful for cursor positioning and clipped string rendering.
 */
int gfx_text_prefix_width(const char *s, int n);

/*
 * Line height in pixels (ascender + |descender|) at the current UI font size.
 * Returns 16 when FreeType is not available.
 */
int gfx_font_height(void);

/*
 * Draw the traffic-light close button (12×12) at pixel (x, y).
 * hovered=1 draws a brighter red for hover feedback.
 * Corners are clipped with C_TITLEBAR to approximate a circle.
 */
void gfx_draw_close_button(unsigned x, unsigned y, int hovered);

/* ── Glass window chrome ────────────────────────────────────────────────────── */

/*
 * gfx_glass_window_frame — draw a complete Lumina glassmorphism window frame.
 *
 *   Renders (in order):
 *     1. Soft drop shadow  (4 px right / 6 px down, near-black, rounded)
 *     2. Window body fill  (C_WIN_BG, corner radius = GFX_WINDOW_R)
 *     3. Titlebar glass    (C_TITLEBAR with specular top-edge highlight,
 *                           top corners rounded, straight bottom edge)
 *     4. Outer glass rim   (1 px rounded rect in C_ACCENT — the visible "edge")
 *     5. Inner border line (1 px rounded rect, darker — adds depth)
 *     6. Accent separator  (C_ACCENT, 1 px, under titlebar)
 *     7. Traffic-light close button
 *     8. Title text        (centered, transparent over glass)
 *
 *   hovered_close: pass 1 when the cursor is over the close button.
 *
 *   The caller is responsible for filling the content area below the titlebar
 *   (e.g. with C_TERM_BG for a terminal, or letting the widget system draw it).
 */
void gfx_glass_window_frame(int wx, int wy, int ww, int wh,
                              int title_h, const char *title,
                              int hovered_close);

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

/* ── BMP icon system ────────────────────────────────────────────────────── */

/*
 * Pure magenta as the chroma-key transparency color.
 * Pixels exactly matching this value are not drawn to the framebuffer.
 * Never appears in the Lumina glassmorphism palette (dark purples + cyans).
 * Artists: fill the icon background with RGB(255, 0, 255) in their editor.
 */
#define GFX_ICON_TRANSPARENT  GFX_RGB(255, 0, 255)

/*
 * Load a 24-bpp or 32-bpp uncompressed BMP from VFS for use as an icon.
 * Output: pixels[] in XRGB8888 format, top-to-bottom, row-major.
 * buf_pixels must be >= width × height (pixel count, not bytes).
 * Returns 0 on success, -1 on error.
 */
int gfx_bmp_load_icon(const char *path, unsigned *pixels, unsigned buf_pixels,
                       unsigned *out_w, unsigned *out_h);

/*
 * Blit a loaded icon pixel buffer to the framebuffer at (dst_x, dst_y)
 * with target size (dst_w × dst_h).  Nearest-neighbor scaling is applied
 * when src and dst sizes differ.  Pixels equal to GFX_ICON_TRANSPARENT
 * are skipped, leaving the background intact.
 */
void gfx_icon_blit(const unsigned *pixels, unsigned src_w, unsigned src_h,
                    int dst_x, int dst_y, int dst_w, int dst_h);

/* Blit a strided pixel buffer into the active render target (or live fb). */
void gfx_raw_blit(const unsigned *src, unsigned src_stride_px,
                  int dst_x, int dst_y, unsigned w, unsigned h);

#endif /* _GFX_H */
