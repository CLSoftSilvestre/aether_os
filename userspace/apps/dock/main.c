/*
 * AetherOS — Dock (WM8)
 * File: userspace/apps/dock/main.c
 *
 * Standalone dock process spawned by init after the compositor.
 * Renders directly to the framebuffer via gfx_begin_frame/gfx_end_frame.
 * Receives all mouse events forwarded from init via the WM event ring.
 * Handles icon magnification, launch bounce, app launching, and window restore.
 */

#include <gfx.h>
#include <gpu.h>
#include <icon_cache.h>
#include <stdlib.h>
#include <sys.h>
#include <input.h>
#include <widget.h>
#include <string.h>

/* ── Layout — must match init/main.c ────────────────────────────────────── */

#define DOCK_H      56
#define TOPBAR_H    36
#define ACCENT_Y    36
#define ACCENT_H     2

static int SCR_W;
static int SCR_H;
static int DOCK_Y;

/* ── Dock layout ─────────────────────────────────────────────────────────── */

#define DOCK_ITEM_COUNT   8
#define DOCK_SLOT_W      80
#define DOCK_ICON_SIZE   40

#define DOCK_MAG_MAX   1.55f
#define DOCK_MAG_INFL  2.5f
#define DOCK_MAG_K   300.0f
#define DOCK_MAG_D    28.0f

static int DOCK_START_X;

/* Back buffer: DOCK_H rows + DOCK_BB_STRIP rows above for icon protrusion */
#define GLASS_W_MAX   1280
#define DOCK_BB_STRIP  40
static unsigned int *g_dock_bb = (unsigned int *)0;   /* GPU BO or malloc fallback */

/* ── Dock item data ──────────────────────────────────────────────────────── */

typedef struct {
    const char *path;
    const char *icon_name;
    long        pid;
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

/* ── Dock animation ──────────────────────────────────────────────────────── */

static spring_interp_t g_dock_mag[DOCK_ITEM_COUNT];
static int             g_dock_hover    = 0;
static int             g_dock_hover_mx = 0;
static dock_anim_t     g_dock_bounce[DOCK_ITEM_COUNT];

/* ── Minimized window tracking ───────────────────────────────────────────── */

#define DOCK_THUMB_W    56
#define DOCK_THUMB_MAX   8
#define THUMB_ICON_SIZE 28   /* icon size inside a minimized thumbnail */

typedef struct {
    int  win_id;
    int  dock_idx;       /* index into g_dock[], or -1 if not a pinned app */
    char title[32];
    char icon_name[32];  /* icon_cache key, set when dock_idx >= 0 */
} dock_thumb_t;

static dock_thumb_t    g_minimized[DOCK_THUMB_MAX];
static int             g_minimized_n = 0;

static spring_interp_t g_thumb_mag[DOCK_THUMB_MAX];
static int             g_thumb_actual_x0 = 0;

/* ── WM state ────────────────────────────────────────────────────────────── */

#define WM_WIN_MAX  16

static long     g_win_id = -1;
static gpu_bo_t g_dock_bo = GPU_BO_INVALID;

/* ── WM helpers ──────────────────────────────────────────────────────────── */

static long find_win_for_pid(long pid)
{
    for (int i = 0; i < WM_WIN_MAX; i++) {
        if (sys_wm_get_pid(i) == pid) return i;
    }
    return -1;
}

static void raise_to_front(long win_id)
{
    int max_z = 0;
    for (int i = 0; i < WM_WIN_MAX; i++) {
        if (!sys_wm_get_pid(i)) continue;
        int z = sys_wm_get_zindex(i);
        if (z >= WM_Z_DOCK) continue;
        if (z > max_z) max_z = z;
    }
    int new_z = max_z + 1;
    if (new_z >= WM_Z_DOCK) new_z = WM_Z_DOCK - 1;
    if (sys_wm_get_zindex(win_id) < new_z) {
        sys_wm_set_zindex(win_id, new_z);
        long pid = sys_wm_get_pid(win_id);
        long pos = sys_wm_get_pos(win_id);
        if (pid && pos != -1) {
            int wx = (int)((unsigned long long)pos >> 32);
            int wy = (int)((unsigned long long)pos & 0xFFFFFFFFu);
            sys_wm_push_event(pid, wm_pack_redraw(wx, wy));
        }
    }
}

/* ── Procedural gradient for protrusion strip ────────────────────────────── */
/* Matches init's wp_bg_at — gives a plausible background color when icons
 * protrude above the dock. Not pixel-perfect with the BMP wallpaper but
 * close enough in the near-dock region that is mostly dark. */
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

/* ── Procedural icon drawing ─────────────────────────────────────────────── */

static void icon_round_corners(int ix, int iy)
{
    gfx_fill((unsigned)ix,      (unsigned)iy,      2, 2, C_PANEL);
    gfx_fill((unsigned)(ix+38), (unsigned)iy,      2, 2, C_PANEL);
    gfx_fill((unsigned)ix,      (unsigned)(iy+38), 2, 2, C_PANEL);
    gfx_fill((unsigned)(ix+38), (unsigned)(iy+38), 2, 2, C_PANEL);
}

static void draw_icon_term(int ix, int iy)
{
    gfx_fill(ix, iy, 40, 40, GFX_RGB(10, 10, 18));
    gfx_fill(ix, iy, 40, 8, GFX_RGB(35, 35, 55));
    gfx_fill(ix+3, iy+2, 4, 4, C_RED);
    gfx_char(ix+4, iy+10, '>', C_ACCENT2, GFX_RGB(10, 10, 18));
    gfx_char(ix+12, iy+10, '_', C_TEXT,   GFX_RGB(10, 10, 18));
    gfx_fill(ix+4, iy+22, 24, 2, C_TEXT_DIM);
    gfx_fill(ix+4, iy+26, 18, 2, C_TEXT_DIM);
    gfx_fill(ix+4, iy+30, 22, 2, C_TEXT_DIM);
    gfx_fill(ix+4, iy+34, 12, 2, C_TEXT_DIM);
    icon_round_corners(ix, iy);
}

static void draw_icon_calc(int ix, int iy)
{
    gfx_fill(ix, iy, 40, 40, GFX_RGB(30, 22, 55));
    gfx_fill(ix+4, iy+4, 32, 9, GFX_RGB(10, 10, 25));
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

static void draw_icon_widget(int ix, int iy)
{
    gfx_fill(ix, iy, 40, 40, GFX_RGB(20, 35, 55));
    gfx_fill(ix+4, iy+6, 32, 10, GFX_RGB(55, 70, 120));
    gfx_rect(ix+4, iy+6, 32, 10, GFX_RGB(80, 100, 180));
    gfx_char(ix+16, iy+7, 'A', C_TEXT, GFX_RGB(55, 70, 120));
    gfx_fill(ix+4, iy+21, 32, 3, GFX_RGB(40, 50, 90));
    gfx_fill(ix+4, iy+20, 15, 5, C_ACCENT);
    gfx_rect(ix+4, iy+30, 8, 8, C_TEXT_DIM);
    gfx_fill(ix+6, iy+32, 4, 4, C_ACCENT2);
    gfx_fill(ix+16, iy+32, 16, 2, C_TEXT_DIM);
    gfx_fill(ix+16, iy+35, 12, 2, C_TEXT_DIM);
    icon_round_corners(ix, iy);
}

static void draw_icon_files(int ix, int iy)
{
    unsigned fg = GFX_RGB(240, 190, 60);
    unsigned md = GFX_RGB(210, 165, 45);
    gfx_fill(ix, iy, 40, 40, GFX_RGB(30, 28, 22));
    gfx_fill(ix+4, iy+12, 16, 4, fg);
    gfx_fill(ix+4, iy+16, 32, 20, fg);
    gfx_fill(ix+4, iy+34, 32, 2, md);
    gfx_fill(ix+8, iy+20, 18, 2, md);
    gfx_fill(ix+8, iy+24, 22, 2, md);
    gfx_fill(ix+8, iy+28, 14, 2, md);
    icon_round_corners(ix, iy);
}

static void draw_icon_tictactoe(int ix, int iy)
{
    unsigned cbg  = GFX_RGB( 22,  20,  38);
    unsigned grid = GFX_RGB( 90,  78, 175);
    unsigned xcol = GFX_RGB(220,  72,  72);
    unsigned ocol = GFX_RGB(  0, 185, 205);
    gfx_fill(ix, iy, 40, 40, cbg);
    gfx_fill(ix + 12, iy +  1, 2, 38, grid);
    gfx_fill(ix + 26, iy +  1, 2, 38, grid);
    gfx_fill(ix +  1, iy + 12, 38, 2, grid);
    gfx_fill(ix +  1, iy + 26, 38, 2, grid);
#define DI_X(cx, cy) \
    do { \
        int _ox = ix + 2 + (cx)*13; \
        int _oy = iy + 2 + (cy)*13; \
        for (int _t = 0; _t < 9; _t++) { \
            gfx_fill(_ox+_t,   _oy+_t,   2, 2, xcol); \
            gfx_fill(_ox+_t,   _oy+8-_t, 2, 2, xcol); \
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

static void draw_icon_text(int ix, int iy)
{
    unsigned paper = GFX_RGB(230, 228, 240);
    unsigned line  = GFX_RGB(80, 80, 110);
    unsigned strip = GFX_RGB(200, 198, 218);
    gfx_fill(ix, iy, 40, 40, paper);
    gfx_fill(ix+28, iy, 12, 40, strip);
    gfx_fill(ix+28, iy, 12, 12, paper);
    gfx_fill(ix+28, iy+12, 1, 1, GFX_RGB(150, 148, 170));
    gfx_fill(ix+4, iy+8,  20, 2, line);
    gfx_fill(ix+4, iy+13, 22, 2, line);
    gfx_fill(ix+4, iy+18, 18, 2, line);
    gfx_fill(ix+4, iy+23, 22, 2, line);
    gfx_fill(ix+4, iy+28, 16, 2, line);
    gfx_fill(ix+4, iy+33, 20, 2, line);
}

static void draw_icon_telnet(int ix, int iy)
{
    unsigned nav = GFX_RGB( 8, 12, 30);
    unsigned bar = GFX_RGB(22, 32, 68);
    unsigned scr = GFX_RGB( 4,  8, 22);
    gfx_fill(ix, iy, 40, 40, nav);
    gfx_fill(ix, iy, 40, 8, bar);
    gfx_fill(ix+3, iy+2, 4, 4, C_RED);
    gfx_fill(ix+2, iy+9, 26, 18, scr);
    gfx_char(ix+3, iy+10, '>', C_ACCENT2, scr);
    gfx_char(ix+11, iy+10, '_', C_TEXT, scr);
    gfx_fill(ix+3, iy+19, 18, 2, C_TEXT_DIM);
    gfx_fill(ix+3, iy+23, 12, 2, C_TEXT_DIM);
    gfx_fill(ix+28, iy+32, 4,  4, C_ACCENT2);
    gfx_fill(ix+33, iy+28, 4,  8, C_ACCENT2);
    gfx_fill(ix+36, iy+24, 3, 12, C_ACCENT2);
    gfx_fill(ix+28, iy+36, 11, 2, C_ACCENT2);
    icon_round_corners(ix, iy);
}

/* Draw icon at a given position and size; BMP first, procedural fallback. */
static void draw_dock_icon_at(int idx, int ix, int iy, int isize)
{
    const icon_entry_t *bicon = icon_cache_get(g_dock[idx].icon_name);
    if (bicon) {
        gfx_icon_blit(bicon->pixels, bicon->width, bicon->height,
                      ix, iy, isize, isize);
        return;
    }
    int nat_x = ix + (isize - DOCK_ICON_SIZE) / 2;
    int nat_y = iy + (isize - DOCK_ICON_SIZE) / 2;
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

/* ── Minimized window tracking ───────────────────────────────────────────── */

static void dock_update_minimized(void)
{
    wm_entry_t entries[WM_WIN_MAX];
    int n = sys_wm_enum(entries, WM_WIN_MAX);
    g_minimized_n = 0;
    for (int i = 0; i < n && g_minimized_n < DOCK_THUMB_MAX; i++) {
        if (!entries[i].minimized) continue;
        g_minimized[g_minimized_n].win_id = entries[i].win_id;

        /* Match against a pinned dock item by PID to get the icon */
        int didx = -1;
        for (int d = 0; d < DOCK_ITEM_COUNT; d++) {
            if (g_dock[d].pid && g_dock[d].pid == (long)entries[i].pid) {
                didx = d;
                break;
            }
        }
        g_minimized[g_minimized_n].dock_idx = didx;

        /* Copy icon_name so the bitmap can be fetched even if dock_idx is lost */
        int k = 0;
        if (didx >= 0) {
            while (k < 31 && g_dock[didx].icon_name[k]) {
                g_minimized[g_minimized_n].icon_name[k] = g_dock[didx].icon_name[k];
                k++;
            }
        }
        g_minimized[g_minimized_n].icon_name[k] = '\0';

        int j = 0;
        while (j < 31 && entries[i].title[j]) {
            g_minimized[g_minimized_n].title[j] = entries[i].title[j];
            j++;
        }
        g_minimized[g_minimized_n].title[j] = '\0';
        g_minimized_n++;
    }
}

static int dock_thumb_x0(void)
{
    return DOCK_START_X + DOCK_ITEM_COUNT * DOCK_SLOT_W + 12;
}

static void draw_minimized_thumbs(int x0)
{
    if (g_minimized_n == 0) return;

    int div_x = x0 - 8;
    gfx_fill((unsigned)div_x, (unsigned)(DOCK_Y + 8), 1, DOCK_H - 16, C_SEP);

    /* Natural thumb bottom edge — thumbs are bottom-anchored to this */
    int thumb_bottom = DOCK_Y + DOCK_H - 4;

    float cx = (float)x0;
    for (int i = 0; i < g_minimized_n; i++) {
        float scale = g_thumb_mag[i].pos;
        float slot  = (float)DOCK_THUMB_W * scale;

        int tw = (int)((float)(DOCK_THUMB_W - 4) * scale);
        if (tw < 8) tw = 8;
        int th = (int)((float)(DOCK_H - 8) * scale);
        if (th < 8) th = 8;

        float center = cx + slot * 0.5f;
        int   tx = (int)(center - (float)tw * 0.5f);
        int   ty = thumb_bottom - th;   /* bottom-anchored, grows upward */

        gfx_fill_rounded((unsigned)tx, (unsigned)ty,
                         (unsigned)tw, (unsigned)th,
                         6, GFX_RGB(40, 35, 70));
        gfx_rect_rounded((unsigned)tx, (unsigned)ty,
                         (unsigned)tw, (unsigned)th,
                         6, C_ACCENT);

        {
            int isz = (int)((float)THUMB_ICON_SIZE * scale);
            if (isz < 4) isz = 4;
            int ix = tx + (tw - isz) / 2;
            int iy = ty + 3;
            const icon_entry_t *ico = icon_cache_get(g_minimized[i].icon_name);
            if (ico) {
                gfx_icon_blit(ico->pixels, ico->width, ico->height,
                              ix, iy, isz, isz);
            } else if (g_minimized[i].dock_idx >= 0) {
                draw_dock_icon_at(g_minimized[i].dock_idx, ix, iy, isz);
            } else {
                char ch = g_minimized[i].title[0] ? g_minimized[i].title[0] : '?';
                int lcx = tx + tw / 2 - 4;
                int lcy = ty + th / 2 - 8;
                gfx_char((unsigned)lcx, (unsigned)lcy, (unsigned char)ch,
                         C_TEXT, GFX_RGB(40, 35, 70));
            }
        }

        char lbl[6];
        int li = 0;
        while (li < 5 && g_minimized[i].title[li]) {
            lbl[li] = g_minimized[i].title[li];
            li++;
        }
        lbl[li] = '\0';
        int ly = ty + th - 10;
        gfx_text_center((unsigned)tx, (unsigned)tw,
                        (unsigned)ly, lbl, C_TEXT_DIM, GFX_RGB(40, 35, 70));

        cx += slot;
    }
}

static int hit_minimized_thumb(int mx, int my)
{
    if (g_minimized_n == 0) return -1;
    if (my < DOCK_Y || my >= DOCK_Y + DOCK_H) return -1;
    float cx = (float)g_thumb_actual_x0;
    for (int i = 0; i < g_minimized_n; i++) {
        float slot = (float)DOCK_THUMB_W * g_thumb_mag[i].pos;
        if ((float)mx >= cx && (float)mx < cx + slot)
            return i;
        cx += slot;
    }
    return -1;
}

/* ── Main dock draw ──────────────────────────────────────────────────────── */

static void draw_dock_magnified(void)
{
    static int s_strip_h = 0;

    /* Effective scale = max(hover spring, bounce spring) */
    float eff[DOCK_ITEM_COUNT];
    float max_scale = 1.0f;
    for (int i = 0; i < DOCK_ITEM_COUNT; i++) {
        float hs = g_dock_mag[i].pos;
        float bs = g_dock_bounce[i].active ? g_dock_bounce[i].scale_sp.pos : 1.0f;
        eff[i] = hs > bs ? hs : bs;
        if (eff[i] > max_scale) max_scale = eff[i];
    }
    /* Include thumb scales — thumbs protrude more (DOCK_H-8 tall vs DOCK_ICON_SIZE) */
    for (int i = 0; i < g_minimized_n; i++) {
        if (g_thumb_mag[i].pos > max_scale) max_scale = g_thumb_mag[i].pos;
    }

    /* Magnified slot widths and centred start X */
    float slot_w[DOCK_ITEM_COUNT];
    float total_w = 0.0f;
    for (int i = 0; i < DOCK_ITEM_COUNT; i++) {
        slot_w[i] = (float)DOCK_SLOT_W * eff[i];
        total_w  += slot_w[i];
    }
    float start_x = ((float)SCR_W - total_w) * 0.5f;

    /* Render into back buffer, blit once to avoid flicker */
    gfx_begin_frame(g_dock_bb, (unsigned)SCR_W,
                    (unsigned)(DOCK_BB_STRIP + DOCK_H),
                    0, DOCK_Y - DOCK_BB_STRIP);

    /* Repaint protrusion strip — use DOCK_H-8 as height basis since thumbs
     * are taller than DOCK_ICON_SIZE and protrude further when magnified */
    int cur_prot = max_scale > 1.001f
                   ? (int)((max_scale - 1.0f) * (float)(DOCK_H - 8)) + 2 : 0;
    int prot = cur_prot > s_strip_h ? cur_prot : s_strip_h;
    if (prot > 0 && DOCK_Y - prot >= 0) {
        for (int y = DOCK_Y - prot; y < DOCK_Y; y++)
            gfx_fill(0, y, SCR_W, 1, wp_bg_at(y));
    }
    s_strip_h = cur_prot;

    /* Dock background — solid panel (glass added in a later phase) */
    gfx_fill(0, (unsigned)DOCK_Y, (unsigned)SCR_W, DOCK_H, C_PANEL);
    gfx_hline(0, (unsigned)DOCK_Y, (unsigned)SCR_W, C_SEP);

    /* Icons at magnified positions */
    float cx = start_x;
    for (int i = 0; i < DOCK_ITEM_COUNT; i++) {
        float center = cx + slot_w[i] * 0.5f;
        int   isz    = (int)((float)DOCK_ICON_SIZE * eff[i]);
        if (isz < 4) isz = 4;
        int ix = (int)(center - (float)isz * 0.5f);
        int iy = DOCK_Y + 4 + DOCK_ICON_SIZE - isz;   /* bottom-anchored */

        draw_dock_icon_at(i, ix, iy, isz);

        int running = g_dock[i].pid && (find_win_for_pid(g_dock[i].pid) >= 0);
        int dx = (int)(center - 8.0f);
        int dy = DOCK_Y + DOCK_H - 8;
        if (dx < 0) dx = 0;
        if (dx + 16 > SCR_W) dx = SCR_W - 16;
        if (running)
            gfx_fill((unsigned)dx, (unsigned)dy, 16, 4, C_ACCENT2);
        else
            gfx_fill((unsigned)dx, (unsigned)dy, 16, 4, C_PANEL);

        cx += slot_w[i];
    }

    /* Thumbs start right after the regular icon block (magnified end) */
    g_thumb_actual_x0 = (int)cx + 12;
    draw_minimized_thumbs(g_thumb_actual_x0);
    gfx_end_frame();
}

static void draw_dock(void)
{
    dock_update_minimized();
    draw_dock_magnified();
}

/* ── Click handling ──────────────────────────────────────────────────────── */

static void dock_click(int mx, int my)
{
    /* Minimized window thumbnails (right section) */
    int tidx = hit_minimized_thumb(mx, my);
    if (tidx >= 0 && tidx < g_minimized_n) {
        sys_wm_restore(g_minimized[tidx].win_id);
        dock_update_minimized();
        draw_dock_magnified();
        return;
    }

    if (mx < DOCK_START_X || mx >= DOCK_START_X + DOCK_ITEM_COUNT * DOCK_SLOT_W)
        return;

    int idx = (mx - DOCK_START_X) / DOCK_SLOT_W;
    if (idx < 0 || idx >= DOCK_ITEM_COUNT) return;

    long wid = g_dock[idx].pid ? find_win_for_pid(g_dock[idx].pid) : -1;

    if (wid >= 0) {
        raise_to_front(wid);
        sys_wm_focus_set(g_dock[idx].pid);
    } else {
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

/* ── Main ────────────────────────────────────────────────────────────────── */

int main(void)
{
    gfx_init();
    icon_cache_init();

    SCR_W        = (int)gfx_width();
    SCR_H        = (int)gfx_height();
    DOCK_Y       = SCR_H - DOCK_H;
    DOCK_START_X = (SCR_W - DOCK_ITEM_COUNT * DOCK_SLOT_W) / 2;

    for (int i = 0; i < DOCK_ITEM_COUNT; i++) {
        spring_init(&g_dock_mag[i], 1.0f, 1.0f, DOCK_MAG_K, DOCK_MAG_D);
        g_dock_bounce[i].active = 0;
    }
    for (int i = 0; i < DOCK_THUMB_MAX; i++)
        spring_init(&g_thumb_mag[i], 1.0f, 1.0f, DOCK_MAG_K, DOCK_MAG_D);
    g_thumb_actual_x0 = dock_thumb_x0();

    /* Register as a WM window covering the dock bar + icon-protrusion strip.
     * The extra DOCK_BB_STRIP rows above DOCK_Y let the compositor composite
     * magnified icons that protrude above the dock bar correctly. */
    g_win_id = sys_wm_register(0, DOCK_Y - DOCK_BB_STRIP,
                                SCR_W, DOCK_BB_STRIP + DOCK_H, "dock");
    if (g_win_id >= 0) {
        sys_wm_set_zindex(g_win_id, WM_Z_DOCK);
        sys_wm_set_flags(g_win_id, WM_FLAG_NO_CHROME);
    }

    /* Allocate GPU BO for the dock back buffer so the compositor can render
     * the dock with correct z-order, opacity, and minimize animations. */
    unsigned bb_bytes = (unsigned)(SCR_W) * (DOCK_BB_STRIP + DOCK_H) * 4u;
    if (g_win_id >= 0) {
        g_dock_bo = gpu_alloc(bb_bytes);
        if (g_dock_bo != GPU_BO_INVALID) {
            g_dock_bb = (unsigned int *)gpu_map(g_dock_bo);
            if (g_dock_bb) {
                sys_wm_set_buffer(g_win_id, g_dock_bo);
                gfx_set_damage_target((int)g_win_id);
            } else {
                gpu_free(g_dock_bo);
                g_dock_bo = GPU_BO_INVALID;
            }
        }
    }
    if (!g_dock_bb)
        g_dock_bb = (unsigned int *)malloc(bb_bytes);

    draw_dock();

    int prev_buttons = 0;
    int dock_counter = 0;

    for (;;) {
        /* Refresh running indicators every ~30 frames.
         * Use sys_wm_enum so minimized windows (still registered in the WM
         * but not found by find_win_for_pid on some implementations) don't
         * cause their dock pid to be incorrectly cleared. */
        if (++dock_counter >= 30) {
            dock_counter = 0;
            wm_entry_t snap[WM_WIN_MAX];
            int ns = sys_wm_enum(snap, WM_WIN_MAX);
            for (int i = 0; i < DOCK_ITEM_COUNT; i++) {
                if (!g_dock[i].pid) continue;
                int alive = 0;
                for (int j = 0; j < ns; j++) {
                    if ((long)snap[j].pid == g_dock[i].pid) { alive = 1; break; }
                }
                if (!alive) g_dock[i].pid = 0;
            }
        }

        /* Drain WM events: mouse (forwarded from init) and window-closed */
        unsigned long long wev;
        while ((wev = sys_wm_event_poll()) != 0) {
            if (wm_event_is_mouse(wev)) {
                mouse_event_t ev = wm_event_mouse_unpack(wev);
                int mx  = (int)ev.x;
                int my  = (int)ev.y;
                int btn = (int)(ev.buttons & 1);
                int pressed = btn && !prev_buttons;
                prev_buttons = btn;

                /* Update hover state */
                if (my >= DOCK_Y) {
                    g_dock_hover    = 1;
                    g_dock_hover_mx = mx;
                } else {
                    g_dock_hover = 0;
                }

                if (pressed && my >= DOCK_Y)
                    dock_click(mx, my);

            } else if (wm_is_window_closed(wev)) {
                /* A window closed — refresh running state and minimized list */
                for (int i = 0; i < DOCK_ITEM_COUNT; i++) {
                    if (g_dock[i].pid && find_win_for_pid(g_dock[i].pid) < 0)
                        g_dock[i].pid = 0;
                }
                dock_update_minimized();
                draw_dock_magnified();
            } else if (wm_event_type(wev) == WM_EV_MINIMIZE) {
                /* Init forwarded a minimize notification — show thumbnail */
                dock_update_minimized();
                draw_dock_magnified();
            }
        }

        /* Hover magnification spring targets */
        if (g_dock_hover) {
            for (int i = 0; i < DOCK_ITEM_COUNT; i++) {
                float center = (float)(DOCK_START_X + i * DOCK_SLOT_W + DOCK_SLOT_W / 2);
                float adist  = (float)g_dock_hover_mx - center;
                if (adist < 0.0f) adist = -adist;
                adist /= (float)DOCK_SLOT_W;
                float t = 1.0f - adist / DOCK_MAG_INFL;
                if (t < 0.0f) t = 0.0f;
                float falloff = t * t * (3.0f - 2.0f * t);
                spring_set_target(&g_dock_mag[i],
                                  1.0f + (DOCK_MAG_MAX - 1.0f) * falloff);
            }
            /* Thumb magnification: same formula, using natural thumb positions */
            for (int i = 0; i < DOCK_THUMB_MAX; i++) {
                if (i < g_minimized_n) {
                    float tcenter = (float)(g_thumb_actual_x0
                                    + i * DOCK_THUMB_W + DOCK_THUMB_W / 2);
                    float adist = (float)g_dock_hover_mx - tcenter;
                    if (adist < 0.0f) adist = -adist;
                    adist /= (float)DOCK_THUMB_W;
                    float t = 1.0f - adist / DOCK_MAG_INFL;
                    if (t < 0.0f) t = 0.0f;
                    float falloff = t * t * (3.0f - 2.0f * t);
                    spring_set_target(&g_thumb_mag[i],
                                      1.0f + (DOCK_MAG_MAX - 1.0f) * falloff);
                } else {
                    spring_set_target(&g_thumb_mag[i], 1.0f);
                }
            }
        } else {
            for (int i = 0; i < DOCK_ITEM_COUNT; i++)
                spring_set_target(&g_dock_mag[i], 1.0f);
            for (int i = 0; i < DOCK_THUMB_MAX; i++)
                spring_set_target(&g_thumb_mag[i], 1.0f);
        }

        /* Advance springs and bounce animations; redraw when anything moved */
        int dock_dirty = 0;
        for (int i = 0; i < DOCK_ITEM_COUNT; i++) {
            float prev = g_dock_mag[i].pos;
            spring_step(&g_dock_mag[i], 1.0f / 60.0f);
            float d = g_dock_mag[i].pos - prev;
            if (d < 0.0f) d = -d;
            if (d > 0.003f) dock_dirty = 1;
        }
        for (int i = 0; i < DOCK_THUMB_MAX; i++) {
            float prev = g_thumb_mag[i].pos;
            spring_step(&g_thumb_mag[i], 1.0f / 60.0f);
            float d = g_thumb_mag[i].pos - prev;
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

        sys_vsync_wait();
    }

    return 0;
}
