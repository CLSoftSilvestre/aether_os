/*
 * AetherOS — Desktop Manager (PID 1)
 * File: userspace/apps/init/main.c
 *
 * Desktop layout (1024×768):
 *   [0]   Top bar    1024×36
 *   [36]  Accent     1024×2
 *   [38]  Main area  1024×650
 *   [688] Dock       1024×56
 *   [744] Bot bar    1024×24
 */

#include <gfx.h>
#include <manifest.h>
#include <stdio.h>
#include <string.h>
#include <sys.h>
#include <input.h>

/* ── Layout constants ────────────────────────────────────────────────────── */

#define TOPBAR_Y    0
#define TOPBAR_H   36
#define ACCENT_Y   36
#define ACCENT_H    2
#define BOTBAR_Y  744
#define BOTBAR_H   24
#define DOCK_H     56
#define DOCK_Y    688        /* BOTBAR_Y - DOCK_H */
#define FONT_W      8
#define FONT_H      8

/* Title-bar height used by all windows — must match apps */
#define APP_TITLE_H  28

/* Maximum number of WM window slots to query */
#define WM_WIN_MAX   16

/* ── Desktop icon grid ───────────────────────────────────────────────────── */

#define DESKTOP_ICON_MAX   16
#define DESKTOP_CELL_W     80
#define DESKTOP_CELL_H     80
#define DESKTOP_ICON_SIZE  48
#define DESKTOP_ICON_X0    16
#define DESKTOP_ICON_Y0    (ACCENT_Y + ACCENT_H + 16)   /* = 54 */
#define DESKTOP_ICON_COLS  ((1024 - DESKTOP_ICON_X0 * 2) / DESKTOP_CELL_W)
#define DESKTOP_DBLCLICK_TICKS  50   /* 500 ms at 100 Hz */

typedef struct {
    int         cell_x, cell_y;
    manifest_t  manifest;
    int         selected;
    long        last_click_tick;
    int         click_count;
} desktop_icon_t;

static desktop_icon_t g_icons[DESKTOP_ICON_MAX];
static int            g_icon_count = 0;

/* ── Dock layout ─────────────────────────────────────────────────────────── */

#define DOCK_ITEM_COUNT   6
#define DOCK_SLOT_W      80    /* width of each icon slot */
#define DOCK_START_X    272    /* (1024 - 6*80) / 2 */
#define DOCK_ICON_SIZE   40    /* 40x40 icon */

typedef struct {
    const char *path;
    long        pid;   /* 0 = not running */
} dock_item_t;

static dock_item_t g_dock[DOCK_ITEM_COUNT] = {
    { "/aether_term", 0 },
    { "/calculator",  0 },
    { "/tictactoe",   0 },
    { "/widget_demo", 0 },
    { "/files",       0 },
    { "/textviewer",  0 },
};

/* ── Helpers ─────────────────────────────────────────────────────────────── */

static void fmt_uptime(char *buf, long ticks)
{
    long s = ticks / 100, m = s / 60; s %= 60;
    long h = m / 60;                  m %= 60;
    snprintf(buf, 16, "%02ld:%02ld:%02ld", h, m, s);
}

/* ── WM helper: find win_id whose owner is pid ───────────────────────────── */

static long find_win_for_pid(long pid)
{
    for (int i = 0; i < WM_WIN_MAX; i++) {
        if (sys_wm_get_pid(i) == pid) return i;
    }
    return -1;
}

/* ── Dock icon drawing ────────────────────────────────────────────────────── */

/* Clip the 4 corner pixels of a 40x40 icon to match the dock background */
static void icon_round_corners(int ix, int iy)
{
    sys_fb_fill(ix,    iy,    2, 2, C_PANEL);
    sys_fb_fill(ix+38, iy,    2, 2, C_PANEL);
    sys_fb_fill(ix,    iy+38, 2, 2, C_PANEL);
    sys_fb_fill(ix+38, iy+38, 2, 2, C_PANEL);
}

/* Terminal: dark shell window with ">_" prompt */
static void draw_icon_term(int ix, int iy)
{
    gfx_fill(ix, iy, 40, 40, GFX_RGB(10, 10, 18));
    gfx_fill(ix, iy, 40, 8, GFX_RGB(35, 35, 55));          /* title bar */
    gfx_fill(ix+3, iy+2, 4, 4, C_RED);                     /* close dot */
    gfx_char(ix+4, iy+10, '>', C_ACCENT2, GFX_RGB(10, 10, 18));
    gfx_char(ix+12, iy+10, '_', C_TEXT,   GFX_RGB(10, 10, 18));
    gfx_fill(ix+4, iy+22, 24, 2, C_TEXT_DIM);              /* text lines */
    gfx_fill(ix+4, iy+26, 18, 2, C_TEXT_DIM);
    gfx_fill(ix+4, iy+30, 22, 2, C_TEXT_DIM);
    gfx_fill(ix+4, iy+34, 12, 2, C_TEXT_DIM);
    icon_round_corners(ix, iy);
}

/* Calculator: purple body with display + button grid */
static void draw_icon_calc(int ix, int iy)
{
    gfx_fill(ix, iy, 40, 40, GFX_RGB(30, 22, 55));
    gfx_fill(ix+4, iy+4, 32, 9, GFX_RGB(10, 10, 25));      /* display */
    gfx_char(ix+24, iy+5, '0', GFX_RGB(100, 255, 130), GFX_RGB(10, 10, 25));
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 3; c++) {
            unsigned col = (r == 3 && c == 2)
                           ? GFX_RGB(90, 76, 200) : GFX_RGB(50, 38, 85);
            gfx_fill(ix+4+c*12, iy+16+r*6, 10, 4, col);
        }
    }
    icon_round_corners(ix, iy);
}

/* Widget demo: button, slider, checkbox on dark blue */
static void draw_icon_widget(int ix, int iy)
{
    gfx_fill(ix, iy, 40, 40, GFX_RGB(20, 35, 55));
    gfx_fill(ix+4, iy+6, 32, 10, GFX_RGB(55, 70, 120));    /* button body */
    gfx_rect(ix+4, iy+6, 32, 10, GFX_RGB(80, 100, 180));   /* button border */
    gfx_char(ix+16, iy+7, 'A', C_TEXT, GFX_RGB(55, 70, 120));
    gfx_fill(ix+4, iy+21, 32, 3, GFX_RGB(40, 50, 90));     /* slider track */
    gfx_fill(ix+4, iy+20, 15, 5, C_ACCENT);                /* slider thumb */
    gfx_rect(ix+4, iy+30, 8, 8, C_TEXT_DIM);               /* checkbox */
    gfx_fill(ix+6, iy+32, 4, 4, C_ACCENT2);                /* checkbox tick */
    gfx_fill(ix+16, iy+32, 16, 2, C_TEXT_DIM);             /* label lines */
    gfx_fill(ix+16, iy+35, 12, 2, C_TEXT_DIM);
    icon_round_corners(ix, iy);
}

/* Files: yellow folder with file lines */
static void draw_icon_files(int ix, int iy)
{
    unsigned fg = GFX_RGB(240, 190, 60);
    unsigned md = GFX_RGB(210, 165, 45);
    gfx_fill(ix, iy, 40, 40, GFX_RGB(30, 28, 22));
    gfx_fill(ix+4, iy+12, 16, 4, fg);                      /* folder tab */
    gfx_fill(ix+4, iy+16, 32, 20, fg);                     /* folder body */
    gfx_fill(ix+4, iy+34, 32, 2, md);                      /* bottom shade */
    gfx_fill(ix+8, iy+20, 18, 2, md);                      /* file lines */
    gfx_fill(ix+8, iy+24, 22, 2, md);
    gfx_fill(ix+8, iy+28, 14, 2, md);
    icon_round_corners(ix, iy);
}

/* Tic-Tac-Toe: purple 3x3 grid with X and O marks */
static void draw_icon_tictactoe(int ix, int iy)
{
    unsigned cbg  = GFX_RGB( 22,  20,  38);
    unsigned grid = GFX_RGB( 90,  78, 175);
    unsigned xcol = GFX_RGB(220,  72,  72);
    unsigned ocol = GFX_RGB(  0, 185, 205);

    gfx_fill(ix, iy, 40, 40, cbg);

    /* Grid lines — two vertical, two horizontal, 2 px thick */
    gfx_fill(ix + 12, iy +  1, 2, 38, grid);
    gfx_fill(ix + 26, iy +  1, 2, 38, grid);
    gfx_fill(ix +  1, iy + 12, 38, 2, grid);
    gfx_fill(ix +  1, iy + 26, 38, 2, grid);

    /* Cells: X O X / O X O / _ O X  (same pattern as 48px icon) */
    /* Col origins: 2, 15, 28  Row origins: 2, 15, 28  Cell size ~11px */
#define DI_X(cx, cy) \
    do { \
        int _ox = ix + 2 + (cx)*13; \
        int _oy = iy + 2 + (cy)*13; \
        for (int _t = 0; _t < 9; _t++) { \
            gfx_fill(_ox+_t,       _oy+_t,     2, 2, xcol); \
            gfx_fill(_ox+_t,       _oy+8-_t,   2, 2, xcol); \
        } \
    } while (0)
#define DI_O(cx, cy) \
    do { \
        int _ox = ix + 2 + (cx)*13; \
        int _oy = iy + 2 + (cy)*13; \
        gfx_rect(_ox,   _oy,   10, 10, ocol); \
        gfx_rect(_ox+1, _oy+1,  8,  8, ocol); \
    } while (0)

    DI_X(0, 0); DI_O(1, 0); DI_X(2, 0);
    DI_O(0, 1); DI_X(1, 1); DI_O(2, 1);
                DI_O(1, 2); DI_X(2, 2);

#undef DI_X
#undef DI_O

    icon_round_corners(ix, iy);
}

/* Text viewer: white paper with text lines and page fold */
static void draw_icon_text(int ix, int iy)
{
    unsigned paper = GFX_RGB(230, 228, 240);
    unsigned line  = GFX_RGB(80, 80, 110);
    unsigned strip = GFX_RGB(200, 198, 218);
    gfx_fill(ix, iy, 40, 40, paper);
    gfx_fill(ix+28, iy, 12, 40, strip);                    /* right margin */
    gfx_fill(ix+28, iy, 12, 12, paper);                    /* fold corner */
    gfx_fill(ix+28, iy+12, 1, 1, GFX_RGB(150, 148, 170)); /* fold crease */
    gfx_fill(ix+4, iy+8,  20, 2, line);
    gfx_fill(ix+4, iy+13, 22, 2, line);
    gfx_fill(ix+4, iy+18, 18, 2, line);
    gfx_fill(ix+4, iy+23, 22, 2, line);
    gfx_fill(ix+4, iy+28, 16, 2, line);
    gfx_fill(ix+4, iy+33, 20, 2, line);
}

/* ── Dock drawing ─────────────────────────────────────────────────────────── */

static void draw_dock_item(int idx)
{
    int sx = DOCK_START_X + idx * DOCK_SLOT_W;
    int ix = sx + (DOCK_SLOT_W - DOCK_ICON_SIZE) / 2;
    int iy = DOCK_Y + 4;

    gfx_fill(sx, DOCK_Y, DOCK_SLOT_W, DOCK_H, C_PANEL);

    switch (idx) {
    case 0: draw_icon_term(ix, iy);       break;
    case 1: draw_icon_calc(ix, iy);       break;
    case 2: draw_icon_tictactoe(ix, iy);  break;
    case 3: draw_icon_widget(ix, iy);     break;
    case 4: draw_icon_files(ix, iy);      break;
    case 5: draw_icon_text(ix, iy);       break;
    }

    /* Running indicator: cyan bar below icon when app has an active window */
    int running = g_dock[idx].pid && (find_win_for_pid(g_dock[idx].pid) >= 0);
    int dx = sx + (DOCK_SLOT_W - 16) / 2;
    int dy = DOCK_Y + DOCK_H - 8;
    gfx_fill(dx, dy, 16, 4, running ? C_ACCENT2 : C_PANEL);
}

static void draw_dock(void)
{
    gfx_fill(0, DOCK_Y, 1024, DOCK_H, C_PANEL);
    gfx_hline(0, DOCK_Y, 1024, C_SEP);
    for (int i = 0; i < DOCK_ITEM_COUNT; i++)
        draw_dock_item(i);
}

/* ── Wallpaper: "Lumina Drift" ──────────────────────────────────────────── */
/*
 * Procedurally generated — no image assets required.
 * Composition (drawn in order, each layer blends into the previous):
 *   1. Deep-space vertical gradient            (768 fills)
 *   2. Purple nebula orb — left-centre         (~1 500 fills)
 *   3. Cyan accent orb  — right-centre         (~1 050 fills)
 *   4. Wavy aurora band, purple→cyan sweep     (~850 fills)
 *   5. Warm horizon glow near the dock         (~80 fills)
 *   6. 130 deterministic stars                 (~200 fills)
 */

/* Active clip rect for partial repaints; default = full screen */
static int s_wc_x0 = 0, s_wc_y0 = 0, s_wc_x1 = 1024, s_wc_y1 = 768;

/* Internal fill that is silently clipped to s_wc_* — used by all wp_draw_* stages. */
static void wp_fill(int x, int y, int w, int h, unsigned color)
{
    int ax = x < s_wc_x0 ? s_wc_x0 : x;
    int ay = y < s_wc_y0 ? s_wc_y0 : y;
    int bx = x + w > s_wc_x1 ? s_wc_x1 : x + w;
    int by = y + h > s_wc_y1 ? s_wc_y1 : y + h;
    if (bx > ax && by > ay)
        gfx_fill((unsigned)ax, (unsigned)ay,
                 (unsigned)(bx - ax), (unsigned)(by - ay), color);
}

static int wp_isqrt(int n)
{
    if (n <= 0) return 0;
    int x = n, y = (x + 1) / 2;
    while (y < x) { x = y; y = (x + n / x) / 2; }
    return x;
}

static unsigned wp_blend(unsigned dst, unsigned src, int t)
{
    if (t <= 0)   return dst;
    if (t >= 255) return src;
    int dr = (int)((dst >> 16) & 0xff), dg = (int)((dst >> 8) & 0xff), db = (int)(dst & 0xff);
    int sr = (int)((src >> 16) & 0xff), sg = (int)((src >> 8) & 0xff), sb = (int)(src & 0xff);
    return GFX_RGB((unsigned)(dr + (sr - dr) * t / 255),
                   (unsigned)(dg + (sg - dg) * t / 255),
                   (unsigned)(db + (sb - db) * t / 255));
}

/* Gradient formula — must match wp_draw_gradient exactly so orb blending is correct. */
static unsigned wp_bg_at(int y)
{
    int r, g, b;
    if (y < 280) {
        r =  4 + y *  8 / 280;
        g =  4 + y *  4 / 280;
        b = 14 + y * 14 / 280;
    } else {
        int dy = y - 280, span = 488;
        r = 12 - dy * 6  / span;
        g =  8 - dy * 3  / span;
        b = 28 - dy * 10 / span;
    }
    return GFX_RGB((unsigned)r, (unsigned)g, (unsigned)b);
}

/* Stage 1 — deep-space two-stop gradient: near-black → deep navy → near-black */
static void wp_draw_gradient(void)
{
    for (int y = 0; y < 768; y++)
        wp_fill(0, y, 1024, 1, wp_bg_at(y));
}

/*
 * Stage 2/3 — soft elliptical nebula orb.
 * Each scanline is split into three horizontal zones for radial falloff:
 *   outer strips (40 % of half-width each) — half intensity
 *   centre strip (60 % of half-width)      — full intensity
 * This approximates a smooth radial gradient with only 3 fills per row.
 */
static void wp_draw_orb(int cx, int cy, int rx, int ry,
                        unsigned color, int max_t)
{
    for (int y = cy - ry; y <= cy + ry; y++) {
        if (y < ACCENT_Y + ACCENT_H || y >= DOCK_Y) continue;
        int dy  = y - cy;
        int ry2 = ry * ry, dy2 = dy * dy;
        if (dy2 > ry2) continue;
        int hw = rx * wp_isqrt(ry2 - dy2) / ry;
        if (hw < 1) continue;

        int ty       = max_t * (ry2 - dy2) / ry2;  /* quadratic Y intensity */
        int inner_hw = hw * 6 / 10;
        unsigned bg  = wp_bg_at(y);
        int x0, x1;

        x0 = cx - hw;
        x1 = cx - inner_hw;
        if (x0 < 0)    x0 = 0;
        if (x1 > 1023) x1 = 1023;
        if (x1 > x0)   wp_fill(x0, y, x1 - x0, 1, wp_blend(bg, color, ty / 2));

        x0 = cx - inner_hw;
        x1 = cx + inner_hw;
        if (x0 < 0)    x0 = 0;
        if (x1 > 1023) x1 = 1023;
        if (x1 > x0)   wp_fill(x0, y, x1 - x0, 1, wp_blend(bg, color, ty));

        x0 = cx + inner_hw;
        x1 = cx + hw;
        if (x0 < 0)    x0 = 0;
        if (x1 > 1023) x1 = 1023;
        if (x1 > x0)   wp_fill(x0, y, x1 - x0, 1, wp_blend(bg, color, ty / 2));
    }
}

/* Stage 4 — wavy aurora band sweeping purple→cyan from left to right */
static void wp_draw_aurora(void)
{
    /* One-period sine approximation (amplitude ±30 px), 16 samples */
    static const int sine16[16] = {
         0, 11, 21, 28, 30, 28, 21, 11,
         0,-11,-21,-28,-30,-28,-21,-11
    };
    const int segs  = 32;           /* 32-px segments across 1024 px */
    const int seg_w = 1024 / segs;
    const int band_h = 26;

    for (int s = 0; s < segs; s++) {
        int bx = s * seg_w;
        int bw = (s == segs - 1) ? 1024 - bx : seg_w;
        int cy = 550 + sine16[s % 16];              /* oscillating centre  */

        int tc       = s * 255 / (segs - 1);        /* purple→cyan colour  */
        unsigned ac  = GFX_RGB((unsigned)(124 - tc * 124 / 255),
                               (unsigned)(106 + tc *  94 / 255),
                               (unsigned)(247 - tc *  27 / 255));

        for (int y = cy - band_h / 2; y <= cy + band_h / 2; y++) {
            if (y < ACCENT_Y + ACCENT_H || y >= DOCK_Y) continue;
            int apos = y - cy; if (apos < 0) apos = -apos;
            int t    = 60 * (band_h / 2 - apos) / (band_h / 2 + 1);
            wp_fill(bx, y, bw, 1, wp_blend(wp_bg_at(y), ac, t));
        }
    }
}

/* Stage 5 — warm-purple horizon glow that grounds the scene near the dock */
static void wp_draw_horizon(void)
{
    for (int y = 608; y < DOCK_Y; y++) {
        int t = 24 * (DOCK_Y - y) / (DOCK_Y - 608);
        wp_fill(0, y, 1024, 1, wp_blend(wp_bg_at(y), GFX_RGB(52, 18, 72), t));
    }
}

/* Stage 6 — deterministic star field (fixed LCG seed → same stars every boot) */
static void wp_draw_stars(void)
{
    unsigned st     = 0xCAFEF00Du;
    int      desk_h = DOCK_Y - (ACCENT_Y + ACCENT_H);   /* 650 px */

    for (int i = 0; i < 130; i++) {
        st = st * 1664525u + 1013904223u;
        int x  = (int)((st >> 1) % 1024u);
        st = st * 1664525u + 1013904223u;
        int y  = (ACCENT_Y + ACCENT_H) + (int)((st >> 1) % (unsigned)desk_h);
        st = st * 1664525u + 1013904223u;
        int br = 80 + (int)((st >> 1) % 150u);           /* brightness 80-229 */
        st = st * 1664525u + 1013904223u;
        int sz = (int)((st >> 1) % 3u);                  /* 0=tiny 1=small 2=bright */

        int      b2 = br + 20 < 255 ? br + 20 : 255;
        unsigned sc = GFX_RGB((unsigned)br, (unsigned)br, (unsigned)b2);

        if (sz == 2) {
            wp_fill(x, y, 2, 2, sc);
        } else {
            wp_fill(x, y, 1, 1, sc);
            if (sz == 1 && x + 1 < 1024)
                wp_fill(x + 1, y, 1, 1,
                        GFX_RGB((unsigned)(br / 2), (unsigned)(br / 2),
                                (unsigned)(b2 / 2)));
        }
    }
}

/* Repaint the wallpaper for any sub-region (window close, drag ghost erase, icon cell). */
static void wp_repaint_region(int rx, int ry, int rw, int rh)
{
    s_wc_x0 = rx;       s_wc_y0 = ry;
    s_wc_x1 = rx + rw;  s_wc_y1 = ry + rh;
    wp_draw_gradient();
    wp_draw_orb(375, 325, 295, 250, C_ACCENT,  170);
    wp_draw_orb(765, 468, 220, 178, C_ACCENT2, 148);
    wp_draw_aurora();
    wp_draw_horizon();
    wp_draw_stars();
    s_wc_x0 = 0;  s_wc_y0 = 0;  s_wc_x1 = 1024;  s_wc_y1 = 768;
}

/* ── Chrome drawing ──────────────────────────────────────────────────────── */

static void draw_desktop(void)
{
    wp_draw_gradient();
    wp_draw_orb(375, 325, 295, 250, C_ACCENT,  170);   /* purple nebula  */
    wp_draw_orb(765, 468, 220, 178, C_ACCENT2, 148);   /* cyan highlight */
    wp_draw_aurora();
    wp_draw_horizon();
    wp_draw_stars();
}

static void draw_top_bar(long ticks)
{
    gfx_fill(0, TOPBAR_Y, 1024, TOPBAR_H, C_PANEL);
    gfx_text(14, TOPBAR_Y + 10, "AetherOS", C_TEXT, C_PANEL);
    gfx_text(14 + 8 * FONT_W + 8, TOPBAR_Y + 10, "v0.0.7", C_TEXT_DIM, C_PANEL);
    gfx_text_center(0, 1024, TOPBAR_Y + 10,
                    "Phase 4.7  --  Lumina Desktop", C_TEXT_DIM, C_PANEL);
    char ubuf[20], tbuf[16];
    fmt_uptime(tbuf, ticks);
    snprintf(ubuf, sizeof(ubuf), "up %s", tbuf);
    int len = (int)strlen(ubuf);
    gfx_text(1024 - len * FONT_W - 14, TOPBAR_Y + 10,
             ubuf, C_TEXT_DIM, C_PANEL);
    gfx_fill(0, ACCENT_Y, 1024, ACCENT_H, C_ACCENT);
}

static void draw_bot_bar(void)
{
    gfx_fill(0, BOTBAR_Y, 1024, BOTBAR_H, C_PANEL);
    gfx_hline(0, BOTBAR_Y, 1024, C_SEP);
    gfx_text(14, BOTBAR_Y + 6,
             "AetherOS 0.0.7  |  QEMU virt  |  Cortex-A76  |  1024x768",
             C_TEXT_DIM, C_PANEL);
    long v = sys_pmm_stats();
    unsigned long free_pages = (unsigned long)((unsigned long long)v >> 32);
    unsigned long free_mb    = free_pages * 4 / 1024;
    char mbuf[24];
    snprintf(mbuf, sizeof(mbuf), "Free: %lu MB", free_mb);
    int mlen = (int)strlen(mbuf);
    gfx_text(1024 - mlen * FONT_W - 14, BOTBAR_Y + 6,
             mbuf, C_TEXT_DIM, C_PANEL);
}

static void refresh_top_bar(long ticks)
{
    char ubuf[20], tbuf[16];
    fmt_uptime(tbuf, ticks);
    snprintf(ubuf, sizeof(ubuf), "up %s", tbuf);
    int len = (int)strlen(ubuf);
    int x = 1024 - len * FONT_W - 14;
    gfx_fill(x - 2, TOPBAR_Y, 1024 - (x - 2), TOPBAR_H, C_PANEL);
    gfx_text(x, TOPBAR_Y + 10, ubuf, C_TEXT_DIM, C_PANEL);
}

static void refresh_bot_bar(void)
{
    long v = sys_pmm_stats();
    unsigned long free_pages = (unsigned long)((unsigned long long)v >> 32);
    unsigned long free_mb    = free_pages * 4 / 1024;
    char mbuf[24];
    snprintf(mbuf, sizeof(mbuf), "Free: %lu MB", free_mb);
    int mlen = (int)strlen(mbuf);
    int x = 1024 - mlen * FONT_W - 14;
    gfx_fill(x - 2, BOTBAR_Y, 1024 - (x - 2), BOTBAR_H, C_PANEL);
    gfx_text(x, BOTBAR_Y + 6, mbuf, C_TEXT_DIM, C_PANEL);
}

/* ── WM helper: draw 2px focus border around a window ───────────────────── */

static void draw_focus_border(long win_id, int focused)
{
    long pos  = sys_wm_get_pos(win_id);
    if (pos == -1) return;
    long size = sys_wm_get_size(win_id);
    if (size == 0) return;

    int x = (int)((unsigned long long)pos  >> 32);
    int y = (int)((unsigned long long)pos  & 0xFFFFFFFFu);
    int w = (int)((unsigned long long)size >> 32);
    int h = (int)((unsigned long long)size & 0xFFFFFFFFu);

    unsigned color = focused ? C_ACCENT : C_SEP;

    sys_fb_fill(x,         y,         w, 2, color);
    sys_fb_fill(x,         y + h - 2, w, 2, color);
    sys_fb_fill(x,         y,         2, h, color);
    sys_fb_fill(x + w - 2, y,         2, h, color);
}

/* ── WM helper: close button hit-test ───────────────────────────────────── */

static int hit_close_button(int wx, int wy, int mx, int my)
{
    return mx >= wx + 10 && mx < wx + 22 &&
           my >= wy +  8 && my < wy + 20;
}

/* ── Desktop icon subsystem (Phase 5.4) ─────────────────────────────────── */

static void desktop_icon_assign_cell(int idx)
{
    int col = idx % DESKTOP_ICON_COLS;
    int row = idx / DESKTOP_ICON_COLS;
    g_icons[idx].cell_x = DESKTOP_ICON_X0 + col * DESKTOP_CELL_W;
    g_icons[idx].cell_y = DESKTOP_ICON_Y0 + row * DESKTOP_CELL_H;
}

static void on_manifest_found(const manifest_t *m, void *ud)
{
    (void)ud;
    if (g_icon_count >= DESKTOP_ICON_MAX) return;
    desktop_icon_t *ic = &g_icons[g_icon_count];
    ic->manifest        = *m;
    ic->selected        = 0;
    ic->last_click_tick = 0;
    ic->click_count     = 0;
    desktop_icon_assign_cell(g_icon_count);
    g_icon_count++;
}

static void desktop_icons_load(void)
{
    g_icon_count = 0;
    manifest_scan_dir(on_manifest_found, (void *)0);

    if (g_icon_count == 0) {
        static const manifest_t fallback[] = {
            { "Terminal", "icon_term",  "/aether_term", "Terminal emulator" },
            { "Files",    "icon_files", "/files",       "File browser"      },
        };
        for (int i = 0; i < 2; i++)
            on_manifest_found(&fallback[i], (void *)0);
    }
}

static void desktop_icons_draw_one(int idx)
{
    desktop_icon_t *ic = &g_icons[idx];
    int cx = ic->cell_x;
    int cy = ic->cell_y;

    unsigned bg;
    if (ic->selected) {
        bg = GFX_RGB(35, 30, 65);
        gfx_fill(cx, cy, DESKTOP_CELL_W, DESKTOP_CELL_H, bg);
        gfx_rect(cx, cy, DESKTOP_CELL_W, DESKTOP_CELL_H, C_ACCENT);
    } else {
        wp_repaint_region(cx, cy, DESKTOP_CELL_W, DESKTOP_CELL_H);
        /* Approximate bg for the text row — gradient color at label y */
        bg = wp_bg_at(cy + 4 + DESKTOP_ICON_SIZE + 4);
    }

    int ix = cx + (DESKTOP_CELL_W - DESKTOP_ICON_SIZE) / 2;
    int iy = cy + 4;

    const char *key = ic->manifest.icon;
    if      (strcmp(key, "icon_term")       == 0) gfx_icon_term(ix, iy);
    else if (strcmp(key, "icon_files")      == 0) gfx_icon_files(ix, iy);
    else if (strcmp(key, "icon_editor")     == 0) gfx_icon_editor(ix, iy);
    else if (strcmp(key, "icon_tictactoe")  == 0) gfx_icon_tictactoe(ix, iy);
    else                                           gfx_icon_generic(ix, iy, ic->manifest.name);

    gfx_text_center(cx, DESKTOP_CELL_W, cy + 4 + DESKTOP_ICON_SIZE + 4,
                    ic->manifest.name, C_TEXT, bg);
}

static void desktop_icons_draw(void)
{
    for (int i = 0; i < g_icon_count; i++)
        desktop_icons_draw_one(i);
}

static void desktop_icons_draw_region(int rx, int ry, int rw, int rh)
{
    for (int i = 0; i < g_icon_count; i++) {
        desktop_icon_t *ic = &g_icons[i];
        if (ic->cell_x + DESKTOP_CELL_W > rx && ic->cell_x < rx + rw &&
            ic->cell_y + DESKTOP_CELL_H > ry && ic->cell_y < ry + rh)
            desktop_icons_draw_one(i);
    }
}

static int desktop_icons_hit_test(int mx, int my)
{
    for (int i = 0; i < g_icon_count; i++) {
        desktop_icon_t *ic = &g_icons[i];
        if (mx >= ic->cell_x && mx < ic->cell_x + DESKTOP_CELL_W &&
            my >= ic->cell_y && my < ic->cell_y + DESKTOP_CELL_H)
            return i;
    }
    return -1;
}

static void desktop_handle_click(int mx, int my)
{
    int hit = desktop_icons_hit_test(mx, my);

    if (hit < 0) {
        int changed = 0;
        for (int i = 0; i < g_icon_count; i++) {
            if (g_icons[i].selected) {
                g_icons[i].selected = 0;
                changed = 1;
            }
        }
        if (changed) desktop_icons_draw();
        return;
    }

    desktop_icon_t *ic = &g_icons[hit];
    long now = sys_get_ticks();

    if (now - ic->last_click_tick < DESKTOP_DBLCLICK_TICKS)
        ic->click_count++;
    else
        ic->click_count = 1;
    ic->last_click_tick = now;

    for (int i = 0; i < g_icon_count; i++) {
        if (i != hit && g_icons[i].selected) {
            g_icons[i].selected = 0;
            desktop_icons_draw_one(i);
        }
    }
    ic->selected = 1;
    desktop_icons_draw_one(hit);

    if (ic->click_count >= 2) {
        ic->click_count = 0;
        ic->selected    = 0;
        desktop_icons_draw_one(hit);
        sys_spawn(ic->manifest.exec);
    }
}

/* ── WM helper: repaint desktop after a window closes ───────────────────── */

static void on_window_closed(unsigned long long ev)
{
    int cx, cy, cw, ch;
    wm_decode_closed(ev, &cx, &cy, &cw, &ch);

    /*
     * Expand clear area by 4px right/bottom to erase the drop-shadow that
     * apps draw just outside their registered rect.
     */
    int clear_w = cw + 4;
    int clear_h = ch + 4;
    if (cx + clear_w > 1024) clear_w = 1024 - cx;
    if (cy + clear_h > DOCK_Y) clear_h = DOCK_Y - cy;
    wp_repaint_region(cx, cy, clear_w, clear_h);

    desktop_icons_draw_region(cx, cy, clear_w, clear_h);

    /* Update dock running state (window is already unregistered at this point) */
    for (int i = 0; i < DOCK_ITEM_COUNT; i++) {
        if (g_dock[i].pid && find_win_for_pid(g_dock[i].pid) < 0)
            g_dock[i].pid = 0;
    }

    /* Redraw dock — clears any overlap with closed window and refreshes indicators */
    draw_dock();

    /*
     * Trigger a repaint for every surviving window that overlaps the vacated
     * region.  Without this, those windows' pixels were overwritten by the
     * closed app and they never know they need to redraw.
     */
    int overlap_w = cw + 4;
    int overlap_h = ch + 4;
    for (int i = 0; i < WM_WIN_MAX; i++) {
        long pid = sys_wm_get_pid(i);
        if (!pid) continue;
        long pos  = sys_wm_get_pos(i);
        long size = sys_wm_get_size(i);
        if (pos == -1 || size == 0) continue;
        int wx = (int)((unsigned long long)pos  >> 32);
        int wy = (int)((unsigned long long)pos  & 0xFFFFFFFFu);
        int ww = (int)((unsigned long long)size >> 32);
        int wh = (int)((unsigned long long)size & 0xFFFFFFFFu);
        if (cx < wx+ww && cx+overlap_w > wx && cy < wy+wh && cy+overlap_h > wy)
            sys_wm_push_event(pid, wm_pack_redraw(wx, wy));
    }

    /* If the closed window held focus, assign it to the first remaining window */
    if (sys_wm_focus_get() == 0) {
        for (int i = 0; i < WM_WIN_MAX; i++) {
            long pid = sys_wm_get_pid(i);
            if (pid) {
                sys_wm_focus_set(pid);
                draw_focus_border(i, 1);
                break;
            }
        }
    }

    /* Redraw focus borders for all surviving windows */
    long focused_pid = sys_wm_focus_get();
    for (int i = 0; i < WM_WIN_MAX; i++) {
        long pid = sys_wm_get_pid(i);
        if (pid) draw_focus_border(i, (pid == focused_pid));
    }
}

/* ── WM helper: hit-test click (x, y) against registered windows ─────────── */

static long hit_test(int mx, int my)
{
    /* Scan in reverse registration order to respect Z-order (last = front) */
    for (int i = WM_WIN_MAX - 1; i >= 0; i--) {
        long pid = sys_wm_get_pid(i);
        if (pid == 0) continue;

        long pos  = sys_wm_get_pos(i);
        long size = sys_wm_get_size(i);
        if (pos == -1 || size == 0) continue;

        int x = (int)((unsigned long long)pos  >> 32);
        int y = (int)((unsigned long long)pos  & 0xFFFFFFFFu);
        int w = (int)((unsigned long long)size >> 32);
        int h = (int)((unsigned long long)size & 0xFFFFFFFFu);

        if (mx >= x && mx < x + w && my >= y && my < y + h)
            return (long)i;
    }
    return -1;
}

/* ── Dock click handler ──────────────────────────────────────────────────── */

static void dock_click(int mx)
{
    if (mx < DOCK_START_X || mx >= DOCK_START_X + DOCK_ITEM_COUNT * DOCK_SLOT_W)
        return;

    int idx = (mx - DOCK_START_X) / DOCK_SLOT_W;
    if (idx < 0 || idx >= DOCK_ITEM_COUNT) return;

    long wid = g_dock[idx].pid ? find_win_for_pid(g_dock[idx].pid) : -1;

    if (wid >= 0) {
        /* App running — focus its window */
        long old_pid = sys_wm_focus_get();
        if (old_pid && old_pid != g_dock[idx].pid) {
            long old_win = find_win_for_pid(old_pid);
            if (old_win >= 0) draw_focus_border(old_win, 0);
        }
        draw_focus_border(wid, 1);
        sys_wm_focus_set(g_dock[idx].pid);
    } else {
        /* App not running — launch it */
        long pid = sys_spawn(g_dock[idx].path);
        if (pid > 0) {
            g_dock[idx].pid = pid;
            draw_dock_item(idx);
        }
    }
}

/* ── Drag state ──────────────────────────────────────────────────────────── */

static int  g_drag_active    = 0;
static long g_drag_win_id    = -1;
static int  g_drag_off_x     = 0;
static int  g_drag_off_y     = 0;
static int  g_drag_ghost_x   = 0;
static int  g_drag_ghost_y   = 0;
static int  g_drag_ghost_w   = 0;

static void draw_drag_ghost(int x, int y, int w)
{
    sys_fb_fill(x, y,               w, APP_TITLE_H, C_TITLEBAR);
    sys_fb_fill(x, y + APP_TITLE_H, w, 2,           C_ACCENT);
    sys_fb_fill(x,     y,     w, 1, C_ACCENT);
    sys_fb_fill(x,     y,     1, APP_TITLE_H + 2, C_ACCENT);
    sys_fb_fill(x+w-1, y,     1, APP_TITLE_H + 2, C_ACCENT);
}

static void erase_drag_ghost(int x, int y, int w)
{
    wp_repaint_region(x, y, w, APP_TITLE_H + 2);
}

/* ── Main ────────────────────────────────────────────────────────────────── */

int main(void)
{
    sys_fb_claim();
    gfx_init();

    draw_desktop();
    draw_top_bar(gfx_ticks());
    draw_bot_bar();
    draw_dock();
    desktop_icons_load();
    desktop_icons_draw();

    /*sys_spawn("/statusbar");*/
    g_dock[0].pid = sys_spawn("/aether_term");

    /* Pre-focus the terminal so keyboard works immediately */
    sys_wm_focus_set(g_dock[0].pid);

    sys_cursor_show(1);

    int  bar_counter  = 0;
    int  dock_counter = 0;
    int  prev_buttons = 0;

    for (;;) {
        if (++bar_counter >= 100) {
            bar_counter = 0;
            refresh_top_bar(gfx_ticks());
            //refresh_bot_bar();
            draw_bot_bar();
        }

        /* Refresh dock running indicators every ~500 ms */
        if (++dock_counter >= 50) {
            dock_counter = 0;
            draw_dock();
        }

        /* ── Drain WM events (window-closed notifications from kernel) ── */
        unsigned long long wev;
        while ((wev = sys_wm_event_poll()) != 0) {
            if (wm_is_window_closed(wev))
                on_window_closed(wev);
        }

        /* ── Process all pending mouse events ────────────────────────── */
        unsigned long long me;
        while ((me = sys_mouse_poll()) != 0) {
            mouse_event_t ev = mouse_event_unpack(me);
            sys_cursor_move(ev.x, ev.y);

            int mx = (int)ev.x;
            int my = (int)ev.y;
            int btn      = (int)(ev.buttons & 1);
            int pressed  = btn && !prev_buttons;
            int released = !btn && prev_buttons;
            prev_buttons = btn;

            /* ── Left button pressed ─────────────────────────────────── */
            if (pressed) {
                /* Dock area — launch or focus app */
                if (my >= DOCK_Y && my < BOTBAR_Y) {
                    dock_click(mx);
                    continue;
                }

                long win_id = hit_test(mx, my);
                if (win_id >= 0) {
                    long new_pid  = sys_wm_get_pid(win_id);
                    long prev_pid = sys_wm_focus_get();

                    if (new_pid != (long)prev_pid) {
                        if (prev_pid) {
                            long old_win = find_win_for_pid(prev_pid);
                            if (old_win >= 0)
                                draw_focus_border(old_win, 0);
                        }
                        draw_focus_border(win_id, 1);
                        sys_wm_focus_set(new_pid);
                    }

                    long pos  = sys_wm_get_pos(win_id);
                    long size = sys_wm_get_size(win_id);
                    if (pos != -1 && size != 0) {
                        int wx = (int)((unsigned long long)pos  >> 32);
                        int wy = (int)((unsigned long long)pos  & 0xFFFFFFFFu);
                        int ww = (int)((unsigned long long)size >> 32);

                        if (my >= wy && my < wy + APP_TITLE_H) {
                            if (hit_close_button(wx, wy, mx, my)) {
                                sys_wm_close(win_id);
                            } else {
                                g_drag_active  = 1;
                                g_drag_win_id  = win_id;
                                g_drag_off_x   = mx - wx;
                                g_drag_off_y   = my - wy;
                                g_drag_ghost_x = wx;
                                g_drag_ghost_y = wy;
                                g_drag_ghost_w = ww;
                                draw_drag_ghost(wx, wy, ww);
                            }
                        }
                    }
                } else {
                    /* Click on desktop background */
                    if (my > ACCENT_Y + ACCENT_H && my < DOCK_Y)
                        desktop_handle_click(mx, my);
                }
            }

            /* ── Button held during drag: move ghost ─────────────────── */
            if (g_drag_active && btn) {
                int nx = mx - g_drag_off_x;
                int ny = my - g_drag_off_y;

                if (nx < 0) nx = 0;
                if (ny < TOPBAR_H + ACCENT_H) ny = TOPBAR_H + ACCENT_H;
                if (nx + g_drag_ghost_w > 1024) nx = 1024 - g_drag_ghost_w;
                if (ny + APP_TITLE_H > DOCK_Y) ny = DOCK_Y - APP_TITLE_H;

                if (nx != g_drag_ghost_x || ny != g_drag_ghost_y) {
                    erase_drag_ghost(g_drag_ghost_x, g_drag_ghost_y,
                                     g_drag_ghost_w);
                    g_drag_ghost_x = nx;
                    g_drag_ghost_y = ny;
                    draw_drag_ghost(nx, ny, g_drag_ghost_w);
                }
            }

            /* ── Button released: commit drag ────────────────────────── */
            if (g_drag_active && released) {
                erase_drag_ghost(g_drag_ghost_x, g_drag_ghost_y,
                                 g_drag_ghost_w);

                /* Restore dock separator if ghost grazed it */
                if (g_drag_ghost_y + APP_TITLE_H + 2 >= DOCK_Y)
                    gfx_hline(0, DOCK_Y, 1024, C_SEP);

                sys_wm_move(g_drag_win_id,
                            g_drag_ghost_x, g_drag_ghost_y);
                draw_focus_border(g_drag_win_id, 1);

                g_drag_active = 0;
                g_drag_win_id = -1;
            }

            /* ── Forward mouse event to the window under the cursor ──── */
            if (!g_drag_active) {
                long fw_id = hit_test(mx, my);
                if (fw_id >= 0) {
                    long fw_pid = sys_wm_get_pid(fw_id);
                    if (fw_pid > 1)
                        sys_wm_push_event(fw_pid,
                                          wm_pack_mouse((unsigned)mx,
                                                        (unsigned)my,
                                                        ev.buttons));
                }
            }
        }

        sys_sleep(1);
    }

    return 0;
}
