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
