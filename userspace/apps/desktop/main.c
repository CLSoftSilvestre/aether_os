/*
 * AetherOS — Desktop Process
 * File: userspace/apps/desktop/main.c
 *
 * Owns the desktop layer: wallpaper and application icons.
 * Registers with the WM as a full-screen GPU BO-backed window at
 * WM_Z_DESKTOP (z = 0) with WM_FLAG_NO_CHROME.  The compositor
 * includes it as the lowest layer in every composited frame, keeping
 * icons always visible regardless of compositor flip timing.
 *
 * Mouse events are forwarded here by init (via sys_wm_push_event)
 * whenever the cursor is over the desktop with no app window on top.
 */

#include <gfx.h>
#include <gpu.h>
#include <icon_cache.h>
#include <manifest.h>
#include <sys.h>
#include <input.h>
#include <string.h>
#include <stdio.h>

/* ── Layout constants (must match init) ─────────────────────────────────── */

#define ACCENT_Y   36
#define ACCENT_H    2
#define DOCK_H     56

#define DESKTOP_ICON_MAX        16
#define DESKTOP_CELL_W          80
#define DESKTOP_CELL_H          80
#define DESKTOP_ICON_SIZE       48
#define DESKTOP_ICON_X0         16
#define DESKTOP_ICON_Y0         (ACCENT_Y + ACCENT_H + 16)   /* 54 */
#define DESKTOP_DBLCLICK_TICKS  50   /* 500 ms at 100 Hz */

/* BMP wallpaper source dimensions */
#define LUMINA_BG_W  1376
#define LUMINA_BG_H   768

static unsigned g_wp_buf[LUMINA_BG_W * LUMINA_BG_H];
static int      g_wp_ok = 0;

/* ── Runtime geometry ────────────────────────────────────────────────────── */

static int SCR_W;
static int SCR_H;
static int DOCK_Y;
static int DESKTOP_ICON_COLS;

/* ── GPU BO ──────────────────────────────────────────────────────────────── */

static gpu_bo_t      g_desktop_bo = GPU_BO_INVALID;
static unsigned int *g_bo_ptr     = NULL;
static long          g_win_id     = -1;

/* ── Icon state ──────────────────────────────────────────────────────────── */

typedef struct {
    int        cell_x, cell_y;
    manifest_t manifest;
    int        selected;
    long       last_click_tick;
    int        click_count;
} desktop_icon_t;

static desktop_icon_t g_icons[DESKTOP_ICON_MAX];
static int            g_icon_count = 0;

/* ── Procedural wallpaper helpers ("Lumina Drift") ──────────────────────── */

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
    int dr = (int)((dst >> 16) & 0xff);
    int dg = (int)((dst >>  8) & 0xff);
    int db = (int)( dst        & 0xff);
    int sr = (int)((src >> 16) & 0xff);
    int sg = (int)((src >>  8) & 0xff);
    int sb = (int)( src        & 0xff);
    return GFX_RGB((unsigned)(dr + (sr - dr) * t / 255),
                   (unsigned)(dg + (sg - dg) * t / 255),
                   (unsigned)(db + (sb - db) * t / 255));
}

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

static void wp_draw_gradient(void)
{
    for (int y = 0; y < SCR_H; y++)
        gfx_fill(0, (unsigned)y, (unsigned)SCR_W, 1, wp_bg_at(y));
}

static void wp_draw_orb(int cx, int cy, int rx, int ry,
                        unsigned color, int max_t)
{
    int desk_top = ACCENT_Y + ACCENT_H;
    for (int y = cy - ry; y <= cy + ry; y++) {
        if (y < desk_top || y >= DOCK_Y) continue;
        int dy  = y - cy;
        int ry2 = ry * ry, dy2 = dy * dy;
        if (dy2 > ry2) continue;
        int hw = rx * wp_isqrt(ry2 - dy2) / ry;
        if (hw < 1) continue;

        int ty       = max_t * (ry2 - dy2) / ry2;
        int inner_hw = hw * 6 / 10;
        unsigned bg  = wp_bg_at(y);
        int x0, x1;

        x0 = cx - hw;         x1 = cx - inner_hw;
        if (x0 < 0)          x0 = 0;
        if (x1 > SCR_W - 1)  x1 = SCR_W - 1;
        if (x1 > x0)
            gfx_fill((unsigned)x0, (unsigned)y, (unsigned)(x1 - x0), 1,
                     wp_blend(bg, color, ty / 2));

        x0 = cx - inner_hw;   x1 = cx + inner_hw;
        if (x0 < 0)          x0 = 0;
        if (x1 > SCR_W - 1)  x1 = SCR_W - 1;
        if (x1 > x0)
            gfx_fill((unsigned)x0, (unsigned)y, (unsigned)(x1 - x0), 1,
                     wp_blend(bg, color, ty));

        x0 = cx + inner_hw;   x1 = cx + hw;
        if (x0 < 0)          x0 = 0;
        if (x1 > SCR_W - 1)  x1 = SCR_W - 1;
        if (x1 > x0)
            gfx_fill((unsigned)x0, (unsigned)y, (unsigned)(x1 - x0), 1,
                     wp_blend(bg, color, ty / 2));
    }
}

static void wp_draw_aurora(void)
{
    static const int sine16[16] = {
         0, 11, 21, 28, 30, 28, 21, 11,
         0,-11,-21,-28,-30,-28,-21,-11
    };
    const int segs   = 32;
    const int seg_w  = SCR_W / segs;
    const int band_h = 26;
    int desk_top = ACCENT_Y + ACCENT_H;

    for (int s = 0; s < segs; s++) {
        int bx = s * seg_w;
        int bw = (s == segs - 1) ? SCR_W - bx : seg_w;
        int cy = 550 + sine16[s % 16];

        int tc      = s * 255 / (segs - 1);
        unsigned ac = GFX_RGB((unsigned)(124 - tc * 124 / 255),
                              (unsigned)(106 + tc *  94 / 255),
                              (unsigned)(247 - tc *  27 / 255));

        for (int y = cy - band_h / 2; y <= cy + band_h / 2; y++) {
            if (y < desk_top || y >= DOCK_Y) continue;
            int apos = y - cy; if (apos < 0) apos = -apos;
            int t    = 60 * (band_h / 2 - apos) / (band_h / 2 + 1);
            gfx_fill((unsigned)bx, (unsigned)y, (unsigned)bw, 1,
                     wp_blend(wp_bg_at(y), ac, t));
        }
    }
}

static void wp_draw_horizon(void)
{
    int horizon_start = DOCK_Y - 80;
    for (int y = horizon_start; y < DOCK_Y; y++) {
        int t = 24 * (DOCK_Y - y) / (DOCK_Y - horizon_start);
        gfx_fill(0, (unsigned)y, (unsigned)SCR_W, 1,
                 wp_blend(wp_bg_at(y), GFX_RGB(52, 18, 72), t));
    }
}

static void wp_draw_stars(void)
{
    unsigned st     = 0xCAFEF00Du;
    int      desk_h = DOCK_Y - (ACCENT_Y + ACCENT_H);

    for (int i = 0; i < 130; i++) {
        st = st * 1664525u + 1013904223u;
        int x  = (int)((st >> 1) % (unsigned)SCR_W);
        st = st * 1664525u + 1013904223u;
        int y  = (ACCENT_Y + ACCENT_H) + (int)((st >> 1) % (unsigned)desk_h);
        st = st * 1664525u + 1013904223u;
        int br = 80 + (int)((st >> 1) % 150u);
        st = st * 1664525u + 1013904223u;
        int sz = (int)((st >> 1) % 3u);

        int      b2 = br + 20 < 255 ? br + 20 : 255;
        unsigned sc = GFX_RGB((unsigned)br, (unsigned)br, (unsigned)b2);

        if (sz == 2) {
            gfx_fill((unsigned)x, (unsigned)y, 2, 2, sc);
        } else {
            gfx_fill((unsigned)x, (unsigned)y, 1, 1, sc);
            if (sz == 1 && x + 1 < SCR_W)
                gfx_fill((unsigned)(x + 1), (unsigned)y, 1, 1,
                         GFX_RGB((unsigned)(br / 2), (unsigned)(br / 2),
                                 (unsigned)(b2 / 2)));
        }
    }
}

/* ── Icon management ─────────────────────────────────────────────────────── */

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

/* Draw one icon into the current render target (the desktop BO). */
static void draw_icon_cell(int idx)
{
    desktop_icon_t *ic = &g_icons[idx];
    int cx = ic->cell_x;
    int cy = ic->cell_y;

    unsigned bg;
    if (ic->selected) {
        bg = GFX_RGB(35, 30, 65);
        gfx_fill((unsigned)cx, (unsigned)cy,
                 DESKTOP_CELL_W, DESKTOP_CELL_H, bg);
        gfx_rect((unsigned)cx, (unsigned)cy,
                 DESKTOP_CELL_W, DESKTOP_CELL_H, C_ACCENT);
    } else {
        /* Wallpaper already painted by desktop_draw_full; just pick label bg */
        bg = g_wp_ok ? C_DESKTOP : wp_bg_at(cy + 4 + DESKTOP_ICON_SIZE + 4);
    }

    int ix = cx + (DESKTOP_CELL_W - DESKTOP_ICON_SIZE) / 2;
    int iy = cy + 4;

    const char *key = ic->manifest.icon;
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

    gfx_text_center((unsigned)cx, DESKTOP_CELL_W,
                    (unsigned)(cy + 4 + DESKTOP_ICON_SIZE + 4),
                    ic->manifest.name, C_TEXT, bg);
}

/* ── Full BO repaint ─────────────────────────────────────────────────────── */

static void desktop_draw_full(void)
{
    gfx_begin_frame(g_bo_ptr, (unsigned)SCR_W, (unsigned)SCR_H, 0, 0);

    if (g_wp_ok) {
        /* Center-crop BMP into BO using gfx_raw_blit (BO-aware) */
        unsigned crop_x = LUMINA_BG_W > (unsigned)SCR_W
                          ? (LUMINA_BG_W - (unsigned)SCR_W) / 2 : 0;
        unsigned crop_y = LUMINA_BG_H > (unsigned)SCR_H
                          ? (LUMINA_BG_H - (unsigned)SCR_H) / 2 : 0;
        unsigned copy_w = (unsigned)SCR_W < LUMINA_BG_W
                          ? (unsigned)SCR_W : LUMINA_BG_W;
        unsigned copy_h = (unsigned)SCR_H < LUMINA_BG_H
                          ? (unsigned)SCR_H : LUMINA_BG_H;
        gfx_raw_blit(g_wp_buf + crop_y * LUMINA_BG_W + crop_x,
                     LUMINA_BG_W, 0, 0, copy_w, copy_h);
    } else {
        wp_draw_gradient();
        wp_draw_orb(SCR_W * 37 / 100, SCR_H * 42 / 100, 295, 250, C_ACCENT,  170);
        wp_draw_orb(SCR_W * 74 / 100, SCR_H * 60 / 100, 220, 178, C_ACCENT2, 148);
        wp_draw_aurora();
        wp_draw_horizon();
        wp_draw_stars();
    }

    for (int i = 0; i < g_icon_count; i++)
        draw_icon_cell(i);

    gfx_end_frame();   /* → sys_wm_damage() → compositor composites on next vsync */
}

/* ── Click handling ──────────────────────────────────────────────────────── */

static int desktop_hit_test(int mx, int my)
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
    int hit = desktop_hit_test(mx, my);

    if (hit < 0) {
        int changed = 0;
        for (int i = 0; i < g_icon_count; i++) {
            if (g_icons[i].selected) { g_icons[i].selected = 0; changed = 1; }
        }
        if (changed) desktop_draw_full();
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
        if (i != hit && g_icons[i].selected)
            g_icons[i].selected = 0;
    }
    ic->selected = 1;
    desktop_draw_full();

    if (ic->click_count >= 2) {
        ic->click_count = 0;
        ic->selected    = 0;
        desktop_draw_full();
        sys_spawn(ic->manifest.exec);
    }
}

/* ── Main ────────────────────────────────────────────────────────────────── */

int main(void)
{
    gfx_init();
    icon_cache_init();

    SCR_W             = (int)gfx_width();
    SCR_H             = (int)gfx_height();
    DOCK_Y            = SCR_H - DOCK_H;
    DESKTOP_ICON_COLS = (SCR_W - DESKTOP_ICON_X0 * 2) / DESKTOP_CELL_W;

    /* Load BMP wallpaper from FAT32; fall back to procedural if unavailable */
    g_wp_ok = (gfx_bmp_load("/lumina_bg.bmp", g_wp_buf, sizeof(g_wp_buf),
                             (void *)0, (void *)0) == 0);

    /* Allocate full-screen GPU BO */
    unsigned bo_bytes = (unsigned)SCR_W * (unsigned)SCR_H * 4u;
    g_desktop_bo = gpu_alloc(bo_bytes);
    if (g_desktop_bo == GPU_BO_INVALID) return 1;
    g_bo_ptr = (unsigned int *)gpu_map(g_desktop_bo);
    if (!g_bo_ptr) { gpu_free(g_desktop_bo); return 1; }

    /* Register with WM as the desktop base layer */
    g_win_id = sys_wm_register(0, 0, SCR_W, SCR_H, "Desktop");
    sys_wm_set_buffer(g_win_id, g_desktop_bo);
    sys_wm_set_zindex(g_win_id, WM_Z_DESKTOP);
    sys_wm_set_flags(g_win_id, WM_FLAG_NO_CHROME);
    gfx_set_damage_target((int)g_win_id);

    /* Load icon manifests */
    desktop_icons_load();

    /* Event loop: receive forwarded mouse events from init.
     * Draw on the first two iterations so the compositor (which may still be
     * initialising at spawn time) is guaranteed to receive a damage event
     * once it is ready. */
    int prev_buttons  = 0;
    int startup_draws = 2;
    for (;;) {
        unsigned long long ev;
        while ((ev = sys_wm_event_poll()) != 0) {
            if (wm_event_is_mouse(ev)) {
                mouse_event_t me = wm_event_mouse_unpack(ev);
                int btn = (int)(me.buttons & 1u);
                if (btn && !prev_buttons)
                    desktop_handle_click((int)me.x, (int)me.y);
                prev_buttons = btn;
            }
        }
        if (startup_draws > 0) {
            desktop_draw_full();
            startup_draws--;
        }
        sys_vsync_wait();
    }

    return 0;
}
