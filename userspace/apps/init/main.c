/*
 * AetherOS — Desktop Manager (PID 1)
 * File: userspace/apps/init/main.c
 *
 * Desktop layout (dynamic — queried from kernel via SYS_FB_INFO):
 *   [0]         Top bar    SCR_W × 36
 *   [36]        Accent     SCR_W × 2
 *   [38]        Main area  SCR_W × (SCR_H - 36 - 2 - 56 - 24)
 *   [SCR_H-80]  Dock       SCR_W × 56
 *   [SCR_H-24]  Bot bar    SCR_W × 24
 */

#include <gfx.h>
#include <gpu.h>
#include <icon_cache.h>
#include <manifest.h>
#include <stdio.h>
#include <string.h>
#include <sys.h>
#include <input.h>
#include <widget.h>

/* ── Wallpaper BMP ───────────────────────────────────────────────────────── */
/* lumina_bg.bmp on the FAT32 disk (/lumina_bg.bmp).  32-bpp, 1376×768.
 * Buffer lives in BSS (~4 MB); loaded once at startup via gfx_bmp_load(). */
#define LUMINA_BG_W  1376
#define LUMINA_BG_H   768

static unsigned g_wp_buf[LUMINA_BG_W * LUMINA_BG_H];
static int      g_wp_ok = 0;   /* 1 once the BMP is loaded successfully */

/* ── Layout constants (size-invariant) ───────────────────────────────────── */

#define TOPBAR_Y    0
#define TOPBAR_H   36
#define ACCENT_Y   36
#define ACCENT_H    2
#define BOTBAR_H   24
#define DOCK_H     56
#define FONT_W       8
#define FONT_H      16

/*
 * Position variables derived from screen size — set in main() after gfx_init().
 * Upper-case names kept for minimal diff against previous hard-coded usage.
 */
static int SCR_W;
static int SCR_H;
static int BOTBAR_Y;   /* SCR_H - BOTBAR_H         */
static int DOCK_Y;     /* SCR_H - BOTBAR_H - DOCK_H */

/* Title-bar height used by all windows — must match apps */
#define APP_TITLE_H  28

/* Maximum number of WM window slots to query */
#define WM_WIN_MAX   16

/* ── Glass panel pixel buffers ───────────────────────────────────────────── */
/* Pre-computed frosted-glass backgrounds for the topbar and dock.
 * Filled once in init_glass_panels() after the wallpaper is drawn.
 * SCR_W is set at runtime; arrays sized for GLASS_W_MAX (max expected width). */
#define GLASS_W_MAX   1280
#define GLASS_BLUR_R    10   /* blur radius in pixels                      */
#define TOPBAR_TINT_A   80   /* topbar tint opacity (0-255)                */
#define DOCK_TINT_A    100  /* dock tint — slightly more opaque           */

static unsigned int g_topbar_glass[GLASS_W_MAX * TOPBAR_H];  /* ~180 KB BSS */
static unsigned int g_dock_glass[GLASS_W_MAX * DOCK_H];      /* ~280 KB BSS */
static int          g_glass_ok = 0;

/* ── Desktop icon grid ───────────────────────────────────────────────────── */

#define DESKTOP_ICON_MAX   16
#define DESKTOP_CELL_W     80
#define DESKTOP_CELL_H     80
#define DESKTOP_ICON_SIZE  48
#define DESKTOP_ICON_X0    16
#define DESKTOP_ICON_Y0    (ACCENT_Y + ACCENT_H + 16)   /* = 54 */
#define DESKTOP_DBLCLICK_TICKS  50   /* 500 ms at 100 Hz */

static int DESKTOP_ICON_COLS;   /* set in main(): (SCR_W - DESKTOP_ICON_X0*2) / DESKTOP_CELL_W */

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

#define DOCK_ITEM_COUNT   8
#define DOCK_SLOT_W      80    /* width of each icon slot */
#define DOCK_ICON_SIZE   40    /* 40x40 icon */

static int DOCK_START_X;      /* set in main(): (SCR_W - DOCK_ITEM_COUNT*DOCK_SLOT_W) / 2 */

/* ── Dock animation ──────────────────────────────────────────────────────────
 * Hover magnification: each icon has a spring-driven scale.  The hovered icon
 * peaks at DOCK_MAG_MAX; neighbours fall off with a cubic-smoothstep curve
 * over DOCK_MAG_INFL slot-widths of radius.  Slot widths expand proportionally
 * so icons physically spread apart (macOS-style).
 * Launch bounce: uses dock_anim_t from libwidget (scale 1.0→1.3→1.0).
 * ─────────────────────────────────────────────────────────────────────────── */
#define DOCK_MAG_MAX   1.55f   /* peak hover scale                           */
#define DOCK_MAG_INFL  2.5f    /* influence radius in slot-width units       */
#define DOCK_MAG_K   300.0f    /* spring stiffness                           */
#define DOCK_MAG_D    28.0f    /* spring damping                             */

static spring_interp_t g_dock_mag[DOCK_ITEM_COUNT]; /* per-icon hover scale  */
static int             g_dock_hover    = 0;          /* mouse over dock zone  */
static int             g_dock_hover_mx = 0;          /* last mouse X in zone  */
static dock_anim_t     g_dock_bounce[DOCK_ITEM_COUNT]; /* launch bounce       */

typedef struct {
    const char *path;
    const char *icon_name;  /* BMP key for /icons/<icon_name>.bmp */
    long        pid;        /* 0 = not running */
} dock_item_t;

static dock_item_t g_dock[DOCK_ITEM_COUNT] = {
    { "/aether_term", "icon_term",      0 },
    { "/calculator",  "icon_calc",      0 },
    { "/tictactoe",   "icon_tictactoe", 0 },
    { "/widget_demo", "icon_widget",    0 },
    { "/files",       "icon_files",     0 },
    { "/textviewer",  "icon_text",      0 },
    { "/telnet",      "icon_telnet",    0 },
    { "/aether_top",  "icon_top",       0 },
};

/* ── Helpers ─────────────────────────────────────────────────────────────── */

static void fmt_time(char *buf, unsigned long ts)
{
    unsigned long s = ts % 86400UL;
    unsigned long h = s / 3600UL;
    unsigned long m = (s % 3600UL) / 60UL;
    snprintf(buf, 6, "%02lu:%02lu", h, m);
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

/* Clip the 4 corner pixels of a 40×40 icon.
 * When glass is active, restores the glass texture at the corners.
 * When not, fills with the flat C_PANEL color. */
static void icon_round_corners(int ix, int iy)
{
    if (g_glass_ok) {
        unsigned gw  = (unsigned)SCR_W;
        unsigned row0 = (unsigned)(iy - DOCK_Y);
        unsigned row1 = row0 + 38u;
        sys_fb_blit(g_dock_glass + row0 * gw + (unsigned)ix,
                    (unsigned)ix, (unsigned)iy, 2, 2, gw * 4);
        sys_fb_blit(g_dock_glass + row0 * gw + (unsigned)(ix + 38),
                    (unsigned)(ix+38), (unsigned)iy, 2, 2, gw * 4);
        sys_fb_blit(g_dock_glass + row1 * gw + (unsigned)ix,
                    (unsigned)ix, (unsigned)(iy+38), 2, 2, gw * 4);
        sys_fb_blit(g_dock_glass + row1 * gw + (unsigned)(ix + 38),
                    (unsigned)(ix+38), (unsigned)(iy+38), 2, 2, gw * 4);
    } else {
        sys_fb_fill((unsigned)ix,    (unsigned)iy,    2, 2, C_PANEL);
        sys_fb_fill((unsigned)(ix+38), (unsigned)iy,    2, 2, C_PANEL);
        sys_fb_fill((unsigned)ix,    (unsigned)(iy+38), 2, 2, C_PANEL);
        sys_fb_fill((unsigned)(ix+38), (unsigned)(iy+38), 2, 2, C_PANEL);
    }
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

/* Telnet: navy background, terminal screen + three signal bars */
static void draw_icon_telnet(int ix, int iy)
{
    unsigned nav = GFX_RGB( 8, 12, 30);
    unsigned bar = GFX_RGB(22, 32, 68);
    unsigned scr = GFX_RGB( 4,  8, 22);

    gfx_fill(ix, iy, 40, 40, nav);
    gfx_fill(ix, iy, 40, 8, bar);
    gfx_fill(ix+3, iy+2, 4, 4, C_RED);

    /* Terminal screen */
    gfx_fill(ix+2, iy+9, 26, 18, scr);
    gfx_char(ix+3, iy+10, '>', C_ACCENT2, scr);
    gfx_char(ix+11, iy+10, '_', C_TEXT, scr);
    gfx_fill(ix+3, iy+19, 18, 2, C_TEXT_DIM);
    gfx_fill(ix+3, iy+23, 12, 2, C_TEXT_DIM);

    /* Signal bars (bottom-right, three ascending) */
    gfx_fill(ix+28, iy+32, 4,  4, C_ACCENT2);   /* small  */
    gfx_fill(ix+33, iy+28, 4,  8, C_ACCENT2);   /* medium */
    gfx_fill(ix+36, iy+24, 3, 12, C_ACCENT2);   /* large  */
    /* Base connector */
    gfx_fill(ix+28, iy+36, 11, 2, C_ACCENT2);

    icon_round_corners(ix, iy);
}

/* ── Dock drawing ─────────────────────────────────────────────────────────── */

static void draw_dock_item(int idx)
{
    int sx = DOCK_START_X + idx * DOCK_SLOT_W;
    int ix = sx + (DOCK_SLOT_W - DOCK_ICON_SIZE) / 2;
    int iy = DOCK_Y + 4;

    if (!g_glass_ok)
        gfx_fill((unsigned)sx, (unsigned)DOCK_Y,
                 (unsigned)DOCK_SLOT_W, DOCK_H, C_PANEL);

    /* Try BMP icon from /icons/ first; fall back to procedural vector icon */
    const icon_entry_t *bicon = icon_cache_get(g_dock[idx].icon_name);
    if (bicon) {
        gfx_icon_blit(bicon->pixels, bicon->width, bicon->height,
                      ix, iy, DOCK_ICON_SIZE, DOCK_ICON_SIZE);
    } else {
        switch (idx) {
        case 0: draw_icon_term(ix, iy);       break;
        case 1: draw_icon_calc(ix, iy);       break;
        case 2: draw_icon_tictactoe(ix, iy);  break;
        case 3: draw_icon_widget(ix, iy);     break;
        case 4: draw_icon_files(ix, iy);      break;
        case 5: draw_icon_text(ix, iy);       break;
        case 6: draw_icon_telnet(ix, iy);     break;
        case 7: draw_icon_widget(ix, iy);     break;
        }
    }

    /* Running indicator: accent bar when running, glass restored when not */
    int running = g_dock[idx].pid && (find_win_for_pid(g_dock[idx].pid) >= 0);
    int dx = sx + (DOCK_SLOT_W - 16) / 2;
    int dy = DOCK_Y + DOCK_H - 8;
    if (running) {
        gfx_fill((unsigned)dx, (unsigned)dy, 16, 4, C_ACCENT2);
    } else if (g_glass_ok) {
        unsigned gw  = (unsigned)SCR_W;
        unsigned row = (unsigned)(dy - DOCK_Y);
        sys_fb_blit(g_dock_glass + row * gw + (unsigned)dx,
                    (unsigned)dx, (unsigned)dy, 16, 4, gw * 4u);
    } else {
        gfx_fill((unsigned)dx, (unsigned)dy, 16, 4, C_PANEL);
    }
}

static void wp_repaint_region(int rx, int ry, int rw, int rh);  /* forward decl */

/* draw_dock_icon_at — render icon idx at arbitrary position and size.
 * BMP icons are scaled via gfx_icon_blit; procedural fallbacks always draw
 * at the natural 40×40 size (centred horizontally, bottom-anchored) to avoid
 * glass-offset out-of-bounds when the icon protrudes above DOCK_Y. */
static void draw_dock_icon_at(int idx, int ix, int iy, int isize)
{
    const icon_entry_t *bicon = icon_cache_get(g_dock[idx].icon_name);
    if (bicon) {
        gfx_icon_blit(bicon->pixels, bicon->width, bicon->height,
                      ix, iy, isize, isize);
        return;
    }
    /* Procedural fallback: fixed 40×40, centred in the slot, at natural Y */
    int nat_x = ix + (isize - DOCK_ICON_SIZE) / 2;
    int nat_y = DOCK_Y + 4;
    switch (idx) {
    case 0: draw_icon_term(nat_x, nat_y);       break;
    case 1: draw_icon_calc(nat_x, nat_y);       break;
    case 2: draw_icon_tictactoe(nat_x, nat_y);  break;
    case 3: draw_icon_widget(nat_x, nat_y);     break;
    case 4: draw_icon_files(nat_x, nat_y);      break;
    case 5: draw_icon_text(nat_x, nat_y);       break;
    case 6: draw_icon_telnet(nat_x, nat_y);     break;
    case 7: draw_icon_widget(nat_x, nat_y);     break;
    }
}

/* draw_dock_magnified — full dock redraw honouring per-icon scale springs.
 *
 * Slot widths scale proportionally (total dock width expands/contracts) and
 * the whole dock stays horizontally centred on screen — this is what gives
 * the "icons spread apart" look.  Icons are bottom-anchored so they grow
 * upward out of the dock.  The wallpaper strip above the dock is repainted
 * whenever any icon protrudes, with a one-frame trailing cleanup on the way
 * back to scale 1.0.
 *
 * This function replaces draw_dock() for all rendering purposes. */
static void draw_dock_magnified(void)
{
    static int s_strip_h = 0;   /* protrusion height repainted last call */

    /* Step 1: effective scale per icon = max(hover, bounce) */
    float eff[DOCK_ITEM_COUNT];
    float max_scale = 1.0f;
    for (int i = 0; i < DOCK_ITEM_COUNT; i++) {
        float hs = g_dock_mag[i].pos;
        float bs = g_dock_bounce[i].active ? g_dock_bounce[i].scale_sp.pos : 1.0f;
        eff[i] = hs > bs ? hs : bs;
        if (eff[i] > max_scale) max_scale = eff[i];
    }

    /* Step 2: magnified slot widths and centred start X */
    float slot_w[DOCK_ITEM_COUNT];
    float total_w = 0.0f;
    for (int i = 0; i < DOCK_ITEM_COUNT; i++) {
        slot_w[i] = (float)DOCK_SLOT_W * eff[i];
        total_w  += slot_w[i];
    }
    float start_x = ((float)SCR_W - total_w) * 0.5f;

    /* Step 3: repaint wallpaper strip above dock (icons may protrude upward).
     * Use max(current, previous) height so the previous frame's pixels are
     * always erased, giving a clean trailing edge as icons shrink back. */
    int cur_prot = max_scale > 1.001f
                   ? (int)((max_scale - 1.0f) * (float)DOCK_ICON_SIZE) + 2 : 0;
    int prot = cur_prot > s_strip_h ? cur_prot : s_strip_h;
    if (prot > 0 && DOCK_Y - prot >= 0)
        wp_repaint_region(0, DOCK_Y - prot, SCR_W, prot);
    s_strip_h = cur_prot;

    /* Step 4: dock glass / panel background */
    if (g_glass_ok) {
        sys_fb_blit((const unsigned *)g_dock_glass,
                    0, (unsigned)DOCK_Y,
                    (unsigned)SCR_W, DOCK_H,
                    (unsigned)SCR_W * 4u);
    } else {
        gfx_fill(0, (unsigned)DOCK_Y, (unsigned)SCR_W, DOCK_H, C_PANEL);
    }
    gfx_hline(0, (unsigned)DOCK_Y, (unsigned)SCR_W, C_SEP);

    /* Step 5: icons at magnified positions */
    float cx = start_x;
    for (int i = 0; i < DOCK_ITEM_COUNT; i++) {
        float center = cx + slot_w[i] * 0.5f;
        int   isz    = (int)((float)DOCK_ICON_SIZE * eff[i]);
        if (isz < 4) isz = 4;
        int ix = (int)(center - (float)isz * 0.5f);
        /* bottom-anchored: icon bottom fixed at DOCK_Y + 4 + DOCK_ICON_SIZE */
        int iy = DOCK_Y + 4 + DOCK_ICON_SIZE - isz;

        draw_dock_icon_at(i, ix, iy, isz);

        /* Running indicator centred below icon within dock bottom area */
        int running = g_dock[i].pid && (find_win_for_pid(g_dock[i].pid) >= 0);
        int dx = (int)(center - 8.0f);
        int dy = DOCK_Y + DOCK_H - 8;
        if (dx < 0) dx = 0;
        if (dx + 16 > SCR_W) dx = SCR_W - 16;
        if (running) {
            gfx_fill((unsigned)dx, (unsigned)dy, 16, 4, C_ACCENT2);
        } else if (g_glass_ok) {
            unsigned gw  = (unsigned)SCR_W;
            unsigned row = (unsigned)(dy - DOCK_Y);
            sys_fb_blit(g_dock_glass + row * gw + (unsigned)dx,
                        (unsigned)dx, (unsigned)dy, 16, 4, gw * 4u);
        } else {
            gfx_fill((unsigned)dx, (unsigned)dy, 16, 4, C_PANEL);
        }
        cx += slot_w[i];
    }
}

static void draw_dock(void)
{
    draw_dock_magnified();
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

/* Active clip rect for partial repaints; reset to full screen after each repaint */
static int s_wc_x0 = 0, s_wc_y0 = 0, s_wc_x1 = 0, s_wc_y1 = 0;

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
    for (int y = 0; y < SCR_H; y++)
        wp_fill(0, y, SCR_W, 1, wp_bg_at(y));
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
        if (x1 > SCR_W - 1) x1 = SCR_W - 1;
        if (x1 > x0)   wp_fill(x0, y, x1 - x0, 1, wp_blend(bg, color, ty / 2));

        x0 = cx - inner_hw;
        x1 = cx + inner_hw;
        if (x0 < 0)    x0 = 0;
        if (x1 > SCR_W - 1) x1 = SCR_W - 1;
        if (x1 > x0)   wp_fill(x0, y, x1 - x0, 1, wp_blend(bg, color, ty));

        x0 = cx + inner_hw;
        x1 = cx + hw;
        if (x0 < 0)    x0 = 0;
        if (x1 > SCR_W - 1) x1 = SCR_W - 1;
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
    const int segs  = 32;
    const int seg_w = SCR_W / segs;
    const int band_h = 26;

    for (int s = 0; s < segs; s++) {
        int bx = s * seg_w;
        int bw = (s == segs - 1) ? SCR_W - bx : seg_w;
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
    int horizon_start = DOCK_Y - 80;
    for (int y = horizon_start; y < DOCK_Y; y++) {
        int t = 24 * (DOCK_Y - y) / (DOCK_Y - horizon_start);
        wp_fill(0, y, SCR_W, 1, wp_blend(wp_bg_at(y), GFX_RGB(52, 18, 72), t));
    }
}

/* Stage 6 — deterministic star field (fixed LCG seed → same stars every boot) */
static void wp_draw_stars(void)
{
    unsigned st     = 0xCAFEF00Du;
    int      desk_h = DOCK_Y - (ACCENT_Y + ACCENT_H);   /* 650 px */

    for (int i = 0; i < 130; i++) {
        st = st * 1664525u + 1013904223u;
        int x  = (int)((st >> 1) % (unsigned)SCR_W);
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
            if (sz == 1 && x + 1 < SCR_W)
                wp_fill(x + 1, y, 1, 1,
                        GFX_RGB((unsigned)(br / 2), (unsigned)(br / 2),
                                (unsigned)(b2 / 2)));
        }
    }
}

/* Repaint the wallpaper for any sub-region (window close, drag ghost erase, icon cell). */
static void wp_repaint_region(int rx, int ry, int rw, int rh)
{
    if (g_wp_ok) {
        gfx_bmp_blit_region(g_wp_buf, LUMINA_BG_W, LUMINA_BG_H,
                             (unsigned)rx, (unsigned)ry,
                             (unsigned)rw, (unsigned)rh);
        return;
    }
    s_wc_x0 = rx;       s_wc_y0 = ry;
    s_wc_x1 = rx + rw;  s_wc_y1 = ry + rh;
    wp_draw_gradient();
    wp_draw_orb(SCR_W * 37 / 100, SCR_H * 42 / 100, 295, 250, C_ACCENT,  170);
    wp_draw_orb(SCR_W * 74 / 100, SCR_H * 60 / 100, 220, 178, C_ACCENT2, 148);
    wp_draw_aurora();
    wp_draw_horizon();
    wp_draw_stars();
    s_wc_x0 = 0;  s_wc_y0 = 0;  s_wc_x1 = SCR_W;  s_wc_y1 = SCR_H;
}

/* ── Chrome drawing ──────────────────────────────────────────────────────── */

static void draw_desktop(void)
{
    if (g_wp_ok) {
        gfx_bmp_blit_region(g_wp_buf, LUMINA_BG_W, LUMINA_BG_H,
                             0, 0, (unsigned)SCR_W, (unsigned)SCR_H);
        return;
    }
    wp_draw_gradient();
    wp_draw_orb(SCR_W * 37 / 100, SCR_H * 42 / 100, 295, 250, C_ACCENT,  170);
    wp_draw_orb(SCR_W * 74 / 100, SCR_H * 60 / 100, 220, 178, C_ACCENT2, 148);
    wp_draw_aurora();
    wp_draw_horizon();
    wp_draw_stars();
}

/*
 * init_glass_panels — compute frosted-glass backgrounds for topbar and dock.
 * Called once after the wallpaper is drawn.  Uses the BMP buffer when
 * available (scaled sample), or the procedural gradient as fallback.
 * Results stored in g_topbar_glass / g_dock_glass and blitted each frame.
 */
static void init_glass_panels(void)
{
    gpu_init(NULL);

    if (g_wp_ok) {
        /* BMP path: sample BMP rows that map to topbar and dock screen rows.
         * The BMP (LUMINA_BG_W × LUMINA_BG_H) is stretch-blitted to the
         * screen, so BMP row for screen row sy = sy * LUMINA_BG_H / SCR_H. */
        int topbar_bmp_y = 0;
        int dock_bmp_y   = DOCK_Y * LUMINA_BG_H / SCR_H;

        gpu_glass_panel(
            (const unsigned int *)(g_wp_buf
                + (unsigned)topbar_bmp_y * LUMINA_BG_W),
            LUMINA_BG_W,
            g_topbar_glass, (unsigned)SCR_W, TOPBAR_H,
            C_PANEL, TOPBAR_TINT_A, GLASS_BLUR_R);

        gpu_glass_panel(
            (const unsigned int *)(g_wp_buf
                + (unsigned)dock_bmp_y * LUMINA_BG_W),
            LUMINA_BG_W,
            g_dock_glass, (unsigned)SCR_W, DOCK_H,
            C_PANEL, DOCK_TINT_A, GLASS_BLUR_R);
    } else {
        /* Procedural path: fill a scratch row-buffer from the gradient. */
        static unsigned int scratch[GLASS_W_MAX];

        /* Topbar */
        for (int y = 0; y < TOPBAR_H; y++) {
            unsigned col = wp_bg_at(TOPBAR_Y + y);
            for (int x = 0; x < SCR_W; x++)
                scratch[x] = col;
            for (int x = 0; x < SCR_W; x++)
                g_topbar_glass[y * GLASS_W_MAX + x] = scratch[x];
        }
        gpu_glass_panel(
            g_topbar_glass, (unsigned)GLASS_W_MAX,
            g_topbar_glass, (unsigned)SCR_W, TOPBAR_H,
            C_PANEL, TOPBAR_TINT_A, GLASS_BLUR_R);

        /* Dock */
        for (int y = 0; y < DOCK_H; y++) {
            unsigned col = wp_bg_at(DOCK_Y + y);
            for (int x = 0; x < SCR_W; x++)
                g_dock_glass[y * GLASS_W_MAX + x] = col;
        }
        gpu_glass_panel(
            g_dock_glass, (unsigned)GLASS_W_MAX,
            g_dock_glass, (unsigned)SCR_W, DOCK_H,
            C_PANEL, DOCK_TINT_A, GLASS_BLUR_R);
    }

    g_glass_ok = 1;
}

static void draw_top_bar(void)
{
    char tbuf[6];
    fmt_time(tbuf, sys_rtc_get());
    int len = (int)strlen(tbuf);

    if (g_glass_ok) {
        sys_fb_blit((const unsigned *)g_topbar_glass,
                    0, (unsigned)TOPBAR_Y,
                    (unsigned)SCR_W, TOPBAR_H,
                    (unsigned)SCR_W * 4u);
        gfx_text_transparent(14, TOPBAR_Y + 10, "AetherOS", C_TEXT);
        gfx_text_transparent(14 + 8 * FONT_W + 8, TOPBAR_Y + 10,
                             "v0.0.7", C_TEXT_DIM);
        gfx_text_center_transparent(0, (unsigned)SCR_W, TOPBAR_Y + 10,
                                    "Lumina Desktop  \xe2\x80\x94  Phase 6.2",
                                    C_TEXT_DIM);
        gfx_text_transparent((unsigned)(SCR_W - len * FONT_W - 14),
                              TOPBAR_Y + 10, tbuf, C_TEXT_DIM);
    } else {
        gfx_fill(0, TOPBAR_Y, (unsigned)SCR_W, TOPBAR_H, C_PANEL);
        gfx_text(14, TOPBAR_Y + 10, "AetherOS", C_TEXT, C_PANEL);
        gfx_text(14 + 8 * FONT_W + 8, TOPBAR_Y + 10,
                 "v0.0.7", C_TEXT_DIM, C_PANEL);
        gfx_text_center(0, (unsigned)SCR_W, TOPBAR_Y + 10,
                        "Lumina Desktop  --  Phase 6.2",
                        C_TEXT_DIM, C_PANEL);
        gfx_text((unsigned)(SCR_W - len * FONT_W - 14), TOPBAR_Y + 10,
                 tbuf, C_TEXT_DIM, C_PANEL);
    }
    gfx_fill(0, ACCENT_Y, (unsigned)SCR_W, ACCENT_H, C_ACCENT);
}

static void draw_bot_bar(void)
{
    gfx_fill(0, BOTBAR_Y, SCR_W, BOTBAR_H, C_PANEL);
    gfx_hline(0, BOTBAR_Y, SCR_W, C_SEP);
    char resbuf[32];
    snprintf(resbuf, sizeof(resbuf), "AetherOS  |  QEMU  |  %dx%d", SCR_W, SCR_H);
    gfx_text(14, BOTBAR_Y + 6, resbuf, C_TEXT_DIM, C_PANEL);
    long v = sys_pmm_stats();
    unsigned long free_pages = (unsigned long)((unsigned long long)v >> 32);
    unsigned long free_mb    = free_pages * 4 / 1024;
    char mbuf[24];
    snprintf(mbuf, sizeof(mbuf), "Free: %lu MB", free_mb);
    int mlen = (int)strlen(mbuf);
    gfx_text(SCR_W - mlen * FONT_W - 14, BOTBAR_Y + 6,
             mbuf, C_TEXT_DIM, C_PANEL);
}

static void refresh_top_bar(void)
{
    char tbuf[6];
    fmt_time(tbuf, sys_rtc_get());
    int len = (int)strlen(tbuf);
    int x = SCR_W - len * FONT_W - 14;

    /* Restore the right third of the topbar, then redraw time text */
    unsigned erase_x = (unsigned)(SCR_W * 2 / 3);
    if (g_glass_ok) {
        sys_fb_blit((const unsigned *)(g_topbar_glass + erase_x),
                    erase_x, (unsigned)TOPBAR_Y,
                    (unsigned)SCR_W - erase_x, TOPBAR_H,
                    (unsigned)SCR_W * 4u);
        gfx_text_transparent((unsigned)x, TOPBAR_Y + 10, tbuf, C_TEXT_DIM);
    } else {
        gfx_fill(erase_x, TOPBAR_Y,
                 (unsigned)SCR_W - erase_x, TOPBAR_H, C_PANEL);
        gfx_text((unsigned)x, TOPBAR_Y + 10, tbuf, C_TEXT_DIM, C_PANEL);
    }
}

static void __attribute__((unused)) refresh_bot_bar(void)
{
    long v = sys_pmm_stats();
    unsigned long free_pages = (unsigned long)((unsigned long long)v >> 32);
    unsigned long free_mb    = free_pages * 4 / 1024;
    char mbuf[24];
    snprintf(mbuf, sizeof(mbuf), "Free: %lu MB", free_mb);
    int mlen = (int)strlen(mbuf);
    int x = SCR_W - mlen * FONT_W - 14;
    gfx_fill(x - 2, BOTBAR_Y, SCR_W - (x - 2), BOTBAR_H, C_PANEL);
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

    /* Rounded 2-px border — matches GFX_WINDOW_R corner radius of the window */
    gfx_rect_rounded((unsigned)x,       (unsigned)y,
                     (unsigned)w,       (unsigned)h,
                     GFX_WINDOW_R, color);
    gfx_rect_rounded((unsigned)(x + 1), (unsigned)(y + 1),
                     (unsigned)(w - 2), (unsigned)(h - 2),
                     (unsigned)(GFX_WINDOW_R > 1 ? GFX_WINDOW_R - 1 : 0), color);
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
        bg = g_wp_ok ? C_DESKTOP : wp_bg_at(cy + 4 + DESKTOP_ICON_SIZE + 4);
    }

    int ix = cx + (DESKTOP_CELL_W - DESKTOP_ICON_SIZE) / 2;
    int iy = cy + 4;

    const char *key = ic->manifest.icon;

    /* Try BMP icon from /icons/ first; fall back to procedural vector icon */
    const icon_entry_t *bicon = icon_cache_get(key);
    if (bicon) {
        gfx_icon_blit(bicon->pixels, bicon->width, bicon->height,
                      ix, iy, DESKTOP_ICON_SIZE, DESKTOP_ICON_SIZE);
    } else if (strcmp(key, "icon_term")      == 0) gfx_icon_term(ix, iy);
    else if   (strcmp(key, "icon_files")     == 0) gfx_icon_files(ix, iy);
    else if   (strcmp(key, "icon_editor")    == 0) gfx_icon_editor(ix, iy);
    else if   (strcmp(key, "icon_tictactoe") == 0) gfx_icon_tictactoe(ix, iy);
    else if   (strcmp(key, "icon_telnet")    == 0) gfx_icon_telnet(ix, iy);
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
    if (cx + clear_w > SCR_W) clear_w = SCR_W - cx;
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
        /* App not running — launch it and start bounce animation */
        long pid = sys_spawn(g_dock[idx].path);
        if (pid > 0) {
            g_dock[idx].pid = pid;
            int sx = DOCK_START_X + idx * DOCK_SLOT_W;
            int ix = sx + (DOCK_SLOT_W - DOCK_ICON_SIZE) / 2;
            dock_anim_start(&g_dock_bounce[idx], ix, DOCK_Y + 4, DOCK_ICON_SIZE);
            draw_dock_magnified();
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
    icon_cache_init();

    /* Compute layout from actual screen dimensions */
    SCR_W          = (int)gfx_width();
    SCR_H          = (int)gfx_height();
    BOTBAR_Y       = SCR_H - BOTBAR_H;
    DOCK_Y         = SCR_H - BOTBAR_H - DOCK_H;
    DOCK_START_X   = (SCR_W - DOCK_ITEM_COUNT * DOCK_SLOT_W) / 2;
    DESKTOP_ICON_COLS = (SCR_W - DESKTOP_ICON_X0 * 2) / DESKTOP_CELL_W;

    /* Initialise full-screen clip rect now that SCR_W/H are known */
    s_wc_x1 = SCR_W;
    s_wc_y1 = SCR_H;

    /* Load BMP wallpaper from FAT32 — falls back to procedural on failure */
    g_wp_ok = (gfx_bmp_load("/lumina_bg.bmp",
                              g_wp_buf, sizeof(g_wp_buf),
                              (void *)0, (void *)0) == 0);
    if (g_wp_ok)
        sys_wp_register(g_wp_buf, LUMINA_BG_W, LUMINA_BG_H);

    /* Initialise dock animation springs (must happen before first draw_dock) */
    for (int i = 0; i < DOCK_ITEM_COUNT; i++) {
        spring_init(&g_dock_mag[i], 1.0f, 1.0f, DOCK_MAG_K, DOCK_MAG_D);
        g_dock_bounce[i].active = 0;
    }

    draw_desktop();
    init_glass_panels();   /* must run after draw_desktop so wallpaper is ready */
    draw_top_bar();
    draw_bot_bar();
    draw_dock();
    desktop_icons_load();
    desktop_icons_draw();

    /*sys_spawn("/statusbar");*/
    // g_dock[0].pid = sys_spawn("/aether_term");

    /* Pre-focus the terminal so keyboard works immediately */
    // sys_wm_focus_set(g_dock[0].pid);

    sys_cursor_show(1);

    /* Dock running state — track per-item to avoid full redraws */
    static int s_dock_running[DOCK_ITEM_COUNT];
    for (int i = 0; i < DOCK_ITEM_COUNT; i++) s_dock_running[i] = 0;

    int  bar_counter  = 0;
    int  dock_counter = 0;
    int  prev_buttons = 0;

    for (;;) {
        if (++bar_counter >= 60) {   /* 60 vsync frames ≈ 1 second */
            bar_counter = 0;
            refresh_top_bar();
            draw_bot_bar();
        }

        /*
         * Refresh dock running indicators every ~30 frames (~500 ms).
         * Only redraw individual items whose running state changed — avoids
         * the full glass-blit → icon-redraw cycle that caused flicker.
         */
        if (++dock_counter >= 30) {
            dock_counter = 0;
            for (int i = 0; i < DOCK_ITEM_COUNT; i++) {
                int running = g_dock[i].pid &&
                              (find_win_for_pid(g_dock[i].pid) >= 0);
                if (running != s_dock_running[i]) {
                    s_dock_running[i] = running;
                    /* When hover magnification is active the animation step
                     * redraws the whole dock; only do individual redraws when idle */
                    if (!g_dock_hover)
                        draw_dock_item(i);
                }
            }
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

            /* Track dock hover for magnification effect */
            if (my >= DOCK_Y && my < BOTBAR_Y) {
                g_dock_hover    = 1;
                g_dock_hover_mx = mx;
            } else {
                g_dock_hover = 0;
            }

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
                if (nx + g_drag_ghost_w > SCR_W) nx = SCR_W - g_drag_ghost_w;
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
                    gfx_hline(0, DOCK_Y, SCR_W, C_SEP);

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

        /* ── Dock animation step ─────────────────────────────────────── */

        /* Update hover magnification spring targets.
         * Distance to cursor normalised to slot-width units; cubic smoothstep
         * gives a smooth Gaussian-like falloff without needing expf(). */
        if (g_dock_hover) {
            for (int i = 0; i < DOCK_ITEM_COUNT; i++) {
                float cx = (float)(DOCK_START_X + i * DOCK_SLOT_W + DOCK_SLOT_W / 2);
                float adist = (float)g_dock_hover_mx - cx;
                if (adist < 0.0f) adist = -adist;
                adist /= (float)DOCK_SLOT_W;   /* normalise to slot units */
                float t = 1.0f - adist / DOCK_MAG_INFL;
                if (t < 0.0f) t = 0.0f;
                float falloff = t * t * (3.0f - 2.0f * t);  /* smoothstep */
                spring_set_target(&g_dock_mag[i],
                                  1.0f + (DOCK_MAG_MAX - 1.0f) * falloff);
            }
        } else {
            for (int i = 0; i < DOCK_ITEM_COUNT; i++)
                spring_set_target(&g_dock_mag[i], 1.0f);
        }

        /* Advance springs and bounce animations; redraw if anything moved */
        int dock_dirty = 0;
        for (int i = 0; i < DOCK_ITEM_COUNT; i++) {
            float prev = g_dock_mag[i].pos;
            spring_step(&g_dock_mag[i], 1.0f / 60.0f);
            float d = g_dock_mag[i].pos - prev;
            if (d < 0.0f) d = -d;
            if (d > 0.003f) dock_dirty = 1;
        }
        for (int i = 0; i < DOCK_ITEM_COUNT; i++) {
            if (g_dock_bounce[i].active) {
                dock_anim_tick(&g_dock_bounce[i]);
                dock_dirty = 1;
            }
        }
        if (dock_dirty)
            draw_dock_magnified();

        sys_vsync_wait();   /* pace the event loop to ~60 fps */
    }

    return 0;
}
