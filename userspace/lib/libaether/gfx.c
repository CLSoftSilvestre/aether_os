/*
 * AetherOS — Userspace Graphics Library
 * File: userspace/lib/libaether/gfx.c
 */

#include <gfx.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys.h>

#define FONT_W   8
#define FONT_H  16

static unsigned g_width;
static unsigned g_height;

#define SYS_FB_INFO  607   /* () → (fb_width << 32) | fb_height */

void gfx_init(void)
{
    long dims = _sys0(SYS_FB_INFO);
    if (dims > 0) {
        g_width  = (unsigned)((unsigned long long)dims >> 32);
        g_height = (unsigned)((unsigned long long)dims & 0xFFFFFFFFu);
    } else {
        g_width  = 1280;
        g_height = 720;
    }
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

/* Transparent text — foreground pixels only, no background rectangle.
 * Use over glass or image backgrounds so the glass shows through. */
void gfx_char_transparent(unsigned x, unsigned y, char ch, unsigned fg)
{
    sys_fb_char_nobg(x, y, (unsigned char)ch, fg);
}

void gfx_text_transparent(unsigned x, unsigned y, const char *s, unsigned fg)
{
    unsigned cx = x;
    for (; *s; s++) {
        gfx_char_transparent(cx, y, *s, fg);
        cx += FONT_W;
    }
}

void gfx_text_center_transparent(unsigned cx, unsigned cw, unsigned y,
                                  const char *s, unsigned fg)
{
    unsigned len    = (unsigned)strlen(s);
    unsigned text_w = len * FONT_W;
    unsigned x      = cx + (cw > text_w ? (cw - text_w) / 2u : 0u);
    gfx_text_transparent(x, y, s, fg);
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

void gfx_draw_close_button(unsigned x, unsigned y, int hovered)
{
    unsigned color = hovered ? GFX_RGB(255, 110, 110) : C_RED;
    gfx_fill(x, y, 12, 12, color);
    gfx_fill(x,      y,      2, 2, C_TITLEBAR);
    gfx_fill(x + 10, y,      2, 2, C_TITLEBAR);
    gfx_fill(x,      y + 10, 2, 2, C_TITLEBAR);
    gfx_fill(x + 10, y + 10, 2, 2, C_TITLEBAR);
}

void gfx_icon_term(int x, int y)
{
    gfx_fill(x, y, 48, 48, GFX_RGB(10, 10, 18));
    gfx_fill(x, y, 48, 10, GFX_RGB(35, 35, 55));
    gfx_fill(x + 3, y + 3, 5, 5, C_RED);
    gfx_char(x + 5,  y + 12, '>', C_ACCENT2, GFX_RGB(10, 10, 18));
    gfx_char(x + 14, y + 12, '_', C_TEXT,    GFX_RGB(10, 10, 18));
    gfx_fill(x + 5, y + 24, 28, 2, C_TEXT_DIM);
    gfx_fill(x + 5, y + 28, 22, 2, C_TEXT_DIM);
    gfx_fill(x + 5, y + 32, 26, 2, C_TEXT_DIM);
    gfx_fill(x + 5, y + 36, 16, 2, C_TEXT_DIM);
    gfx_fill(x + 5, y + 40, 20, 2, C_TEXT_DIM);
    gfx_fill(x,      y,      2, 2, GFX_RGB(10, 10, 18));
    gfx_fill(x + 46, y,      2, 2, GFX_RGB(10, 10, 18));
    gfx_fill(x,      y + 46, 2, 2, GFX_RGB(10, 10, 18));
    gfx_fill(x + 46, y + 46, 2, 2, GFX_RGB(10, 10, 18));
}

void gfx_icon_files(int x, int y)
{
    unsigned fg  = GFX_RGB(240, 190,  60);
    unsigned md  = GFX_RGB(210, 165,  45);
    unsigned cbg = GFX_RGB( 30,  28,  22);
    gfx_fill(x, y, 48, 48, cbg);
    gfx_fill(x +  5, y + 14, 18,  5, fg);
    gfx_fill(x +  5, y + 19, 38, 22, fg);
    gfx_fill(x +  5, y + 38,  38,  3, md);
    gfx_fill(x + 10, y + 24, 22,  2, md);
    gfx_fill(x + 10, y + 29, 26,  2, md);
    gfx_fill(x + 10, y + 34, 18,  2, md);
    gfx_fill(x,      y,       2,  2, cbg);
    gfx_fill(x + 46, y,       2,  2, cbg);
    gfx_fill(x,      y + 46,  2,  2, cbg);
    gfx_fill(x + 46, y + 46,  2,  2, cbg);
}

void gfx_icon_editor(int x, int y)
{
    unsigned cbg = GFX_RGB(18, 20, 35);
    gfx_fill(x, y, 48, 48, cbg);
    gfx_fill(x + 5, y +  8, 28, 2, C_ACCENT);
    gfx_fill(x + 5, y + 14, 22, 2, C_TEXT_DIM);
    gfx_fill(x + 10, y + 19, 18, 2, C_TEXT_DIM);
    gfx_fill(x + 5, y + 24, 24, 2, C_TEXT_DIM);
    gfx_fill(x + 10, y + 29, 16, 2, C_TEXT_DIM);
    gfx_fill(x + 5, y + 34, 20, 2, C_TEXT_DIM);
    gfx_fill(x + 5, y + 39,  2,  8, C_ACCENT2);
    gfx_fill(x,      y,      2,  2, cbg);
    gfx_fill(x + 46, y,      2,  2, cbg);
    gfx_fill(x,      y + 46, 2,  2, cbg);
    gfx_fill(x + 46, y + 46, 2,  2, cbg);
}

void gfx_icon_generic(int x, int y, const char *label)
{
    unsigned cbg = GFX_RGB(35, 35, 60);
    gfx_fill(x, y, 48, 48, cbg);
    gfx_rect(x + 1, y + 1, 46, 46, C_ACCENT);
    if (label && label[0])
        gfx_char(x + 20, y + 20, label[0], C_TEXT, cbg);
}

/* ── Scalable icon helpers ──────────────────────────────────────────────── */

/* Scale n (designed for 48-px canvas) to actual sz */
#define S(n)  ((int)(sz) * (n) / 48)

/* ── Drive: FAT32 hard-disk silhouette ─────────────────────────────────── */
void gfx_icon_drive_fat32(int x, int y, int sz)
{
    unsigned bg   = GFX_RGB( 20,  22,  32);
    unsigned body = GFX_RGB(140, 140, 160);
    unsigned top  = GFX_RGB(170, 170, 190);
    unsigned dot  = GFX_RGB( 80, 200, 120);

    gfx_fill(x, y, sz, sz, bg);
    /* Disk body */
    gfx_fill(x + S(4), y + S(10), S(40), S(28), body);
    /* Top plate (lighter) */
    gfx_fill(x + S(4), y + S(10), S(40), S(8),  top);
    /* Platter circle approximation */
    gfx_fill(x + S(18), y + S(20), S(12), S(12), GFX_RGB(100,100,120));
    /* Activity LED */
    if (sz >= 16)
        gfx_fill(x + S(34), y + S(14), S(4), S(4), dot);
    /* Label "FAT" — only at larger sizes */
    if (sz >= 32) {
        gfx_text((unsigned)(x + S(10)), (unsigned)(y + S(32)),
                 "FAT", C_TEXT_DIM, body);
    }
}

/* ── Drive: InitRD RAM chip ────────────────────────────────────────────── */
void gfx_icon_drive_initrd(int x, int y, int sz)
{
    unsigned bg   = GFX_RGB( 12,  24,  28);
    unsigned chip = GFX_RGB(  0, 160, 180);
    unsigned pin  = GFX_RGB(  0, 120, 140);

    gfx_fill(x, y, sz, sz, bg);
    /* Chip body */
    gfx_fill(x + S(8),  y + S(12), S(32), S(24), chip);
    /* Pins left */
    gfx_fill(x + S(2),  y + S(16), S(6),  S(4),  pin);
    gfx_fill(x + S(2),  y + S(24), S(6),  S(4),  pin);
    /* Pins right */
    gfx_fill(x + S(40), y + S(16), S(6),  S(4),  pin);
    gfx_fill(x + S(40), y + S(24), S(6),  S(4),  pin);
    /* Label "RAM" */
    if (sz >= 32) {
        gfx_text((unsigned)(x + S(8)), (unsigned)(y + S(20)),
                 "RAM", C_TEXT, chip);
    }
}

/* ── Drive: AetherFS disc ──────────────────────────────────────────────── */
void gfx_icon_drive_afs(int x, int y, int sz)
{
    unsigned bg   = GFX_RGB( 18,  10,  32);
    unsigned disc = GFX_RGB( 50,  44,  90);
    unsigned ring = GFX_RGB( 80,  68, 140);

    gfx_fill(x, y, sz, sz, bg);
    /* Disc circle (approximated with nested fills) */
    gfx_fill(x + S(6),  y + S(6),  S(36), S(36), disc);
    gfx_fill(x + S(10), y + S(10), S(28), S(28), ring);
    gfx_fill(x + S(16), y + S(16), S(16), S(16), disc);
    gfx_fill(x + S(20), y + S(20), S(8),  S(8),  GFX_RGB( 30, 26, 50));
    /* "A" glyph in accent */
    if (sz >= 16) {
        int ax2 = x + S(20) - (sz >= 32 ? 4 : 2);
        int ay2 = y + S(14);
        gfx_char((unsigned)ax2, (unsigned)ay2, 'A', C_ACCENT, disc);
    }
}

/* ── Folder: closed two-tone yellow shape ─────────────────────────────── */
void gfx_icon_folder(int x, int y, int sz)
{
    unsigned bg   = GFX_RGB( 28,  26,  20);
    unsigned gold = GFX_RGB(230, 175,  50);
    unsigned dark = GFX_RGB(190, 145,  40);

    gfx_fill(x, y, sz, sz, bg);
    /* Tab (top-left notch) */
    gfx_fill(x + S(4),  y + S(12), S(16), S(6),  gold);
    /* Body */
    gfx_fill(x + S(4),  y + S(18), S(40), S(24), gold);
    /* Shadow edge */
    gfx_fill(x + S(4),  y + S(38), S(40), S(4),  dark);
    /* Interior lines at larger sizes */
    if (sz >= 32) {
        gfx_fill(x + S(10), y + S(24), S(26), S(2), dark);
        gfx_fill(x + S(10), y + S(30), S(20), S(2), dark);
    }
}

/* ── Folder: open with lifted front panel ─────────────────────────────── */
void gfx_icon_folder_open(int x, int y, int sz)
{
    unsigned bg   = GFX_RGB( 28,  26,  20);
    unsigned gold = GFX_RGB(230, 175,  50);
    unsigned lite = GFX_RGB(250, 205,  80);
    unsigned dark = GFX_RGB(190, 145,  40);

    gfx_fill(x, y, sz, sz, bg);
    /* Back panel */
    gfx_fill(x + S(4),  y + S(14), S(40), S(28), dark);
    /* Tab */
    gfx_fill(x + S(4),  y + S(10), S(16), S(6),  gold);
    /* Front panel lifted (shifted up 4px) */
    gfx_fill(x + S(2),  y + S(22), S(42), S(20), gold);
    /* Highlight on front lip */
    gfx_fill(x + S(2),  y + S(22), S(42), S(3),  lite);
    /* Interior visible behind front */
    if (sz >= 24) {
        gfx_fill(x + S(10), y + S(16), S(24), S(4), GFX_RGB(210, 190, 100));
    }
}

/* ── File: plain text document with horizontal lines ─────────────────── */
void gfx_icon_file_txt(int x, int y, int sz)
{
    unsigned bg   = GFX_RGB( 20,  20,  32);
    unsigned page = GFX_RGB(220, 220, 230);
    unsigned fold = GFX_RGB(160, 160, 180);
    unsigned line = GFX_RGB(160, 160, 200);

    gfx_fill(x, y, sz, sz, bg);
    /* Page body */
    gfx_fill(x + S(6),  y + S(4),  S(30), S(40), page);
    /* Folded top-right corner */
    gfx_fill(x + S(26), y + S(4),  S(10), S(10), bg);
    gfx_fill(x + S(26), y + S(4),  S(10), S(10), fold);
    gfx_fill(x + S(26), y + S(14), S(10), S(2),  fold);
    gfx_fill(x + S(36), y + S(4),  S(2),  S(10), fold);
    /* Text lines */
    if (sz >= 20) {
        gfx_fill(x + S(10), y + S(18), S(20), S(2), line);
        gfx_fill(x + S(10), y + S(23), S(22), S(2), line);
        gfx_fill(x + S(10), y + S(28), S(18), S(2), line);
        gfx_fill(x + S(10), y + S(33), S(16), S(2), line);
    }
}

/* ── File: AetherScript source with code glyph ────────────────────────── */
void gfx_icon_file_as(int x, int y, int sz)
{
    unsigned bg   = GFX_RGB( 20,  20,  32);
    unsigned page = GFX_RGB(220, 220, 230);
    unsigned fold = GFX_RGB(160, 160, 180);

    gfx_fill(x, y, sz, sz, bg);
    /* Page */
    gfx_fill(x + S(6),  y + S(4),  S(30), S(40), page);
    gfx_fill(x + S(26), y + S(4),  S(10), S(10), bg);
    gfx_fill(x + S(26), y + S(4),  S(10), S(10), fold);
    gfx_fill(x + S(26), y + S(14), S(10), S(2),  fold);
    gfx_fill(x + S(36), y + S(4),  S(2),  S(10), fold);
    /* "{}" glyph in accent */
    if (sz >= 24) {
        gfx_char((unsigned)(x + S(12)), (unsigned)(y + S(20)),
                 '{', C_ACCENT, page);
        gfx_char((unsigned)(x + S(20)), (unsigned)(y + S(20)),
                 '}', C_ACCENT, page);
    } else {
        gfx_fill(x + S(10), y + S(20), S(16), S(2), C_ACCENT);
    }
    /* accent bar at top of page */
    gfx_fill(x + S(6), y + S(4), S(20), S(3), C_ACCENT);
}

/* ── File: executable / gear silhouette ───────────────────────────────── */
void gfx_icon_file_exec(int x, int y, int sz)
{
    unsigned bg   = GFX_RGB( 16,  16,  26);
    unsigned gear = GFX_RGB( 80, 200, 120);
    unsigned hub  = GFX_RGB( 20,  60,  36);

    gfx_fill(x, y, sz, sz, bg);
    /* Outer gear ring (approximated as crossed rectangles + corners) */
    gfx_fill(x + S(14), y + S(4),  S(20), S(40), gear);  /* vertical bar */
    gfx_fill(x + S(4),  y + S(14), S(40), S(20), gear);  /* horizontal bar */
    gfx_fill(x + S(10), y + S(8),  S(28), S(32), gear);  /* fill body */
    /* Teeth stubs */
    gfx_fill(x + S(18), y + S(2),  S(12), S(4),  gear);
    gfx_fill(x + S(18), y + S(42), S(12), S(4),  gear);
    gfx_fill(x + S(2),  y + S(18), S(4),  S(12), gear);
    gfx_fill(x + S(42), y + S(18), S(4),  S(12), gear);
    /* Hub */
    gfx_fill(x + S(18), y + S(18), S(12), S(12), hub);
}

/* ── File: generic page with folded corner ────────────────────────────── */
void gfx_icon_file_generic(int x, int y, int sz)
{
    unsigned bg   = GFX_RGB( 20,  20,  32);
    unsigned page = GFX_RGB(200, 200, 218);
    unsigned fold = GFX_RGB(140, 140, 165);

    gfx_fill(x, y, sz, sz, bg);
    /* Page body (excluding top-right fold area) */
    gfx_fill(x + S(6),  y + S(4),  S(30), S(40), page);
    /* Fold: overwrite corner with bg, draw fold triangle */
    gfx_fill(x + S(26), y + S(4),  S(10), S(10), bg);
    gfx_fill(x + S(26), y + S(4),  S(10), S(10), fold);
    /* Fold crease lines */
    gfx_fill(x + S(26), y + S(14), S(10), S(2),  fold);
    gfx_fill(x + S(36), y + S(4),  S(2),  S(10), fold);
}

#undef S

/* ── 48×48 Telnet desktop icon ─────────────────────────────────────────── */
/*
 * Design: navy background, terminal screen on the left with ">_" prompt,
 * three ascending signal bars on the right representing the network link.
 * Colour scheme is distinctly blue (not purple) to differ from icon_term.
 */
void gfx_icon_telnet(int x, int y)
{
    unsigned nav = GFX_RGB( 8, 12, 30);   /* deep navy background    */
    unsigned bar = GFX_RGB(22, 32, 68);   /* title-bar tint          */
    unsigned scr = GFX_RGB( 4,  8, 22);   /* screen area             */

    gfx_fill(x, y, 48, 48, nav);
    gfx_fill(x, y, 48, 11, bar);
    gfx_fill(x + 3, y + 3, 5, 5, C_RED);

    /* Terminal screen (left side): x+3..x+32, y+13..y+37 */
    gfx_fill(x + 3, y + 13, 30, 25, scr);
    gfx_char(x + 4, y + 15, '>', C_ACCENT2, scr);
    gfx_char(x + 13, y + 15, '_', C_TEXT, scr);
    gfx_fill(x + 4, y + 25, 22, 2, C_TEXT_DIM);
    gfx_fill(x + 4, y + 29, 18, 2, C_TEXT_DIM);
    gfx_fill(x + 4, y + 33, 14, 2, C_TEXT_DIM);

    /* Signal bars (bottom-right): three ascending bars */
    gfx_fill(x + 35, y + 36, 4, 4,  C_ACCENT2);   /* small  */
    gfx_fill(x + 40, y + 31, 4, 9,  C_ACCENT2);   /* medium */
    gfx_fill(x + 45, y + 25, 3, 15, C_ACCENT2);   /* large  */
    /* Base connector */
    gfx_fill(x + 35, y + 40, 13, 2, C_ACCENT2);

    /* Round corners */
    gfx_fill(x,      y,      2, 2, nav);
    gfx_fill(x + 46, y,      2, 2, nav);
    gfx_fill(x,      y + 46, 2, 2, nav);
    gfx_fill(x + 46, y + 46, 2, 2, nav);
}

/* ── 48×48 Tic-Tac-Toe desktop icon ───────────────────────────────────── */
/*
 * Draws a stylised 3×3 board with pre-filled X and O marks so the icon is
 * instantly recognisable at a glance.
 *
 * Board grid lines are drawn in the accent purple.  The three X marks use
 * the game's red and the two O rings use cyan — matching the in-game colors.
 * Cells:   X | O | X
 *          O | X | O   (centre X)
 *            | O | X
 */
void gfx_icon_tictactoe(int x, int y)
{
    unsigned cbg  = GFX_RGB( 22,  20,  38);
    unsigned grid = GFX_RGB( 90,  78, 175);   /* muted accent purple */
    unsigned xcol = GFX_RGB(220,  72,  72);   /* X red               */
    unsigned ocol = GFX_RGB(  0, 185, 205);   /* O cyan              */

    gfx_fill(x, y, 48, 48, cbg);

    /* Grid lines: two vertical + two horizontal, 2 px thick */
    /* Divide 48px into 3 columns of 14px with 2px lines between */
    /* Column dividers at x+14 and x+30 (width 2) */
    gfx_fill(x + 15, y +  1, 2, 46, grid);
    gfx_fill(x + 31, y +  1, 2, 46, grid);
    /* Row dividers at y+15 and y+31 (height 2) */
    gfx_fill(x +  1, y + 15, 46, 2, grid);
    gfx_fill(x +  1, y + 31, 46, 2, grid);

    /* Cell origins (top-left pixel of drawable area inside each cell):
     *   col 0: x+2   col 1: x+18   col 2: x+34
     *   row 0: y+2   row 1: y+18   row 2: y+34
     * Each cell has ~12px drawable width and height.                    */
#define ICON_X(cx,cy,i,j) \
    do { \
        int _ox = (x+2) + (cx)*16; \
        int _oy = (y+2) + (cy)*16; \
        int _len = 10; \
        for (int _t = 0; _t < _len; _t++) { \
            gfx_fill(_ox+1+_t, _oy+1+_t,             2, 2, xcol); \
            gfx_fill(_ox+1+_t, _oy+1+(_len-1-_t),    2, 2, xcol); \
        } \
        (void)(i); (void)(j); \
    } while (0)

#define ICON_O(cx,cy) \
    do { \
        int _ox = (x+2) + (cx)*16; \
        int _oy = (y+2) + (cy)*16; \
        gfx_rect(_ox+1, _oy+1, 11, 11, ocol); \
        gfx_rect(_ox+2, _oy+2,  9,  9, ocol); \
        gfx_rect(_ox+3, _oy+3,  7,  7, ocol); \
    } while (0)

    /* Filled board:  X O X / O X O / _ O X */
    ICON_X(0, 0, 0, 0);
    ICON_O(1, 0);
    ICON_X(2, 0, 0, 0);

    ICON_O(0, 1);
    ICON_X(1, 1, 0, 0);   /* centre */
    ICON_O(2, 1);

    /* cell (0,2) is empty — intentional */
    ICON_O(1, 2);
    ICON_X(2, 2, 0, 0);

#undef ICON_X
#undef ICON_O

    /* Round corners */
    gfx_fill(x,      y,      2, 2, cbg);
    gfx_fill(x + 46, y,      2, 2, cbg);
    gfx_fill(x,      y + 46, 2, 2, cbg);
    gfx_fill(x + 46, y + 46, 2, 2, cbg);
}

/* ── BMP loader ──────────────────────────────────────────────────────────── */

static int bmp_read_exact(long vfd, void *dst, long n)
{
    char *p = (char *)dst;
    while (n > 0) {
        long r = sys_fs_read(vfd, p, n);
        if (r <= 0) return -1;
        p += r;
        n -= r;
    }
    return 0;
}

int gfx_bmp_load(const char *path, unsigned *pixels, unsigned buf_bytes,
                  unsigned *out_w, unsigned *out_h)
{
    long vfd = sys_fs_open(path);
    if (vfd < 0) return -1;

    unsigned char hdr[54];
    if (bmp_read_exact(vfd, hdr, 54) != 0 ||
        hdr[0] != 'B' || hdr[1] != 'M') {
        sys_fs_close(vfd); return -1;
    }

    unsigned pix_off = (unsigned)hdr[10] | ((unsigned)hdr[11] << 8) |
                       ((unsigned)hdr[12] << 16) | ((unsigned)hdr[13] << 24);
    unsigned width   = (unsigned)hdr[18] | ((unsigned)hdr[19] << 8) |
                       ((unsigned)hdr[20] << 16) | ((unsigned)hdr[21] << 24);
    int      h_raw   = (int)((unsigned)hdr[22] | ((unsigned)hdr[23] << 8) |
                             ((unsigned)hdr[24] << 16) | ((unsigned)hdr[25] << 24));
    unsigned bpp     = (unsigned)hdr[28] | ((unsigned)hdr[29] << 8);
    unsigned compr   = (unsigned)hdr[30] | ((unsigned)hdr[31] << 8) |
                       ((unsigned)hdr[32] << 16) | ((unsigned)hdr[33] << 24);

    if (bpp != 32 || compr != 0 || h_raw == 0 || width == 0) {
        sys_fs_close(vfd); return -1;
    }

    int flipped = (h_raw > 0);   /* positive height = rows stored bottom-to-top */
    unsigned height = flipped ? (unsigned)h_raw : (unsigned)(-h_raw);

    if (width * height * 4 > buf_bytes) {
        sys_fs_close(vfd); return -1;
    }

    /* Skip extra header bytes between offset 54 and the pixel data */
    if (pix_off > 54) {
        unsigned char tmp[64];
        unsigned extra = pix_off - 54;
        while (extra > 0) {
            unsigned chunk = extra < 64 ? extra : 64;
            if (bmp_read_exact(vfd, tmp, (long)chunk) != 0) {
                sys_fs_close(vfd); return -1;
            }
            extra -= chunk;
        }
    }

    /*
     * Read pixel rows sequentially from the file.
     * BMP row 0 in the file = bottom row of the image (when flipped=1).
     * Store into pixels[] top-to-bottom: file row i → buffer row (height-1-i).
     * 32-bpp BMP stores [B][G][R][X] per pixel — identical to XRGB8888 u32.
     */
    unsigned row_bytes = width * 4;
    for (unsigned i = 0; i < height; i++) {
        unsigned dst_row = flipped ? (height - 1 - i) : i;
        unsigned char *dst = (unsigned char *)(pixels + dst_row * width);
        if (bmp_read_exact(vfd, dst, (long)row_bytes) != 0) {
            sys_fs_close(vfd); return -1;
        }
    }

    sys_fs_close(vfd);
    if (out_w) *out_w = width;
    if (out_h) *out_h = height;
    return 0;
}

void gfx_bmp_blit_region(const unsigned *pixels, unsigned bmp_w, unsigned bmp_h,
                           unsigned dst_x, unsigned dst_y,
                           unsigned dst_w, unsigned dst_h)
{
    unsigned scr_w = g_width;
    unsigned scr_h = g_height;

    /* Center-crop the BMP onto the screen */
    unsigned crop_x = (bmp_w > scr_w) ? (bmp_w - scr_w) / 2 : 0;
    unsigned crop_y = (bmp_h > scr_h) ? (bmp_h - scr_h) / 2 : 0;

    /* Clamp destination to screen */
    if (dst_x + dst_w > scr_w) dst_w = scr_w - dst_x;
    if (dst_y + dst_h > scr_h) dst_h = scr_h - dst_y;
    if (dst_w == 0 || dst_h == 0) return;

    /* Map destination position back to source position in the BMP */
    unsigned src_x = dst_x + crop_x;
    unsigned src_y = dst_y + crop_y;

    if (src_x >= bmp_w || src_y >= bmp_h) return;
    if (src_x + dst_w > bmp_w) dst_w = bmp_w - src_x;
    if (src_y + dst_h > bmp_h) dst_h = bmp_h - src_y;

    const unsigned *src = pixels + src_y * bmp_w + src_x;
    sys_fb_blit(src, dst_x, dst_y, dst_w, dst_h, bmp_w * 4);
}
