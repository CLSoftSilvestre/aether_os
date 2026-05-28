/*
 * AetherOS — Power Menu
 * File: userspace/apps/power_menu/main.c
 *
 * Spawned by the topbar when the power icon is clicked.
 * Opens a centered dialog with "Shut Down" and "Reboot" buttons.
 * Closes itself on Cancel or the title-bar close button.
 *
 * Window: 300 × 170 px, centered on screen, WM chrome (close button).
 */

#include <gfx.h>
#include <gpu.h>
#include <input.h>
#include <stdio.h>
#include <string.h>
#include <sys.h>

/* ── Window geometry ─────────────────────────────────────────────────────── */

#define PM_W         300
#define PM_H         170
#define PM_TH          28   /* title bar height */
#define TOPBAR_STRIP   38   /* TOPBAR_H + ACCENT_H */

/* Content area (below title bar + separator) */
#define CONTENT_Y    (PM_TH + 2)

/* Action buttons (side by side) */
#define BTN_W        120
#define BTN_H         40
#define BTN_SD_X       22   /* Shut Down — left button */
#define BTN_RB_X      158   /* Reboot    — right button (22+120+16) */
#define BTN_Y          58   /* relative to window top */

/* Cancel button */
#define CANCEL_W       80
#define CANCEL_H       26
#define CANCEL_X      ((PM_W - CANCEL_W) / 2)   /* = 110, centered */
#define CANCEL_Y      120

/* Close-button hit box (matches gfx_glass_window_frame placement) */
#define CLOSE_BTN_X   10
#define CLOSE_BTN_W   12

/* ── Globals ─────────────────────────────────────────────────────────────── */

static int            SCR_W, SCR_H;
static int            PM_X,  PM_Y;

static long           g_win_id = -1;
static gpu_bo_t       g_bo     = GPU_BO_INVALID;
static unsigned int  *g_bo_ptr = NULL;

/* Hover state */
static int g_hov_close    = 0;
static int g_hov_shutdown = 0;
static int g_hov_reboot   = 0;
static int g_hov_cancel   = 0;
static int g_needs_draw   = 1;

/* ── Hit testing ─────────────────────────────────────────────────────────── */

static int hit_close(int mx, int my)
{
    return (mx >= PM_X + CLOSE_BTN_X &&
            mx <  PM_X + CLOSE_BTN_X + CLOSE_BTN_W + 4 &&
            my >= PM_Y && my < PM_Y + PM_TH);
}

static int hit_shutdown(int mx, int my)
{
    return (mx >= PM_X + BTN_SD_X && mx < PM_X + BTN_SD_X + BTN_W &&
            my >= PM_Y + BTN_Y   && my < PM_Y + BTN_Y + BTN_H);
}

static int hit_reboot(int mx, int my)
{
    return (mx >= PM_X + BTN_RB_X && mx < PM_X + BTN_RB_X + BTN_W &&
            my >= PM_Y + BTN_Y   && my < PM_Y + BTN_Y + BTN_H);
}

static int hit_cancel(int mx, int my)
{
    return (mx >= PM_X + CANCEL_X && mx < PM_X + CANCEL_X + CANCEL_W &&
            my >= PM_Y + CANCEL_Y && my < PM_Y + CANCEL_Y + CANCEL_H);
}

/* ── Drawing ─────────────────────────────────────────────────────────────── */

/* 14×14 power symbol (ring with top gap + center stem) */
static void draw_power_glyph(int x, int y, unsigned c)
{
    /* Stem — passes through ring gap to center */
    gfx_fill(x+6, y+0, 2, 8, c);

    /* Ring top arc flanking the stem gap */
    gfx_fill(x+2,  y+3, 2, 1, c);
    gfx_fill(x+10, y+3, 2, 1, c);

    /* Ring sides taper outward */
    gfx_fill(x+1,  y+4, 2, 1, c);
    gfx_fill(x+11, y+4, 2, 1, c);

    /* Ring widest section */
    gfx_fill(x+0,  y+5, 2, 4, c);
    gfx_fill(x+12, y+5, 2, 4, c);

    /* Ring sides taper inward */
    gfx_fill(x+1,  y+9, 2, 1, c);
    gfx_fill(x+11, y+9, 2, 1, c);

    /* Ring bottom corners */
    gfx_fill(x+2,  y+10, 2, 1, c);
    gfx_fill(x+10, y+10, 2, 1, c);

    /* Ring bottom arc (2 px tall) */
    gfx_fill(x+3, y+11, 8, 2, c);
}

static void draw_window(void)
{
    if (g_bo_ptr)
        gfx_begin_frame(g_bo_ptr, PM_W, PM_H, PM_X, PM_Y);

    /* Window chrome */
    gfx_glass_window_frame(PM_X, PM_Y, PM_W, PM_H, PM_TH, "Power", g_hov_close);

    /* Content background */
    gfx_fill(PM_X, PM_Y + CONTENT_Y, PM_W, PM_H - CONTENT_Y, C_WIN_BG);

    /* Subtitle */
    int lbl_y = PM_Y + 40;
    gfx_text_center(PM_X, PM_W, lbl_y, "Choose an action:", C_TEXT_DIM, C_WIN_BG);

    /* Power glyph centered above subtitle — small decorative element */
    int glyph_x = PM_X + (PM_W - 14) / 2;
    draw_power_glyph(glyph_x, PM_Y + CONTENT_Y + 4, C_TEXT_DIM);

    /* Shut Down button */
    unsigned sd_bg = g_hov_shutdown ? C_RED : GFX_RGB(90, 30, 30);
    gfx_fill_rounded(PM_X + BTN_SD_X, PM_Y + BTN_Y, BTN_W, BTN_H, 5, sd_bg);
    gfx_rect_rounded(PM_X + BTN_SD_X, PM_Y + BTN_Y, BTN_W, BTN_H, 5,
                     g_hov_shutdown ? GFX_RGB(255, 120, 120) : GFX_RGB(180, 60, 60));
    gfx_text_center(PM_X + BTN_SD_X, BTN_W,
                    PM_Y + BTN_Y + (BTN_H - gfx_font_height()) / 2,
                    "Shut Down", C_TEXT, sd_bg);

    /* Reboot button */
    unsigned rb_bg = g_hov_reboot ? C_ACCENT : GFX_RGB(50, 46, 84);
    gfx_fill_rounded(PM_X + BTN_RB_X, PM_Y + BTN_Y, BTN_W, BTN_H, 5, rb_bg);
    gfx_rect_rounded(PM_X + BTN_RB_X, PM_Y + BTN_Y, BTN_W, BTN_H, 5,
                     g_hov_reboot ? C_ACCENT : C_SEP);
    gfx_text_center(PM_X + BTN_RB_X, BTN_W,
                    PM_Y + BTN_Y + (BTN_H - gfx_font_height()) / 2,
                    "Reboot", C_TEXT, rb_bg);

    /* Cancel button */
    unsigned ca_bg = g_hov_cancel ? GFX_RGB(50, 46, 84) : GFX_RGB(28, 26, 46);
    gfx_fill_rounded(PM_X + CANCEL_X, PM_Y + CANCEL_Y, CANCEL_W, CANCEL_H, 4, ca_bg);
    gfx_rect_rounded(PM_X + CANCEL_X, PM_Y + CANCEL_Y, CANCEL_W, CANCEL_H, 4, C_SEP);
    gfx_text_center(PM_X + CANCEL_X, CANCEL_W,
                    PM_Y + CANCEL_Y + (CANCEL_H - gfx_font_height()) / 2,
                    "Cancel", C_TEXT_DIM, ca_bg);

    if (g_bo_ptr)
        gfx_end_frame();
}

/* ── Main ────────────────────────────────────────────────────────────────── */

int main(void)
{
    gfx_init();
    SCR_W = (int)gfx_width();
    SCR_H = (int)gfx_height();

    PM_X = (SCR_W - PM_W) / 2;
    PM_Y = (SCR_H - PM_H) / 2;

    g_win_id = sys_wm_register(PM_X, PM_Y, PM_W, PM_H, "Power");
    if (g_win_id < 0)
        sys_exit(1);

    g_bo = gpu_alloc((unsigned)(PM_W * PM_H) * 4u);
    if (g_bo != GPU_BO_INVALID) {
        g_bo_ptr = (unsigned int *)gpu_map(g_bo);
        if (g_bo_ptr) {
            sys_wm_set_buffer(g_win_id, g_bo);
            gfx_set_damage_target((int)g_win_id);
        } else {
            gpu_free(g_bo);
            g_bo = GPU_BO_INVALID;
        }
    }

    draw_window();

    int prev_lbtn = 0;

    for (;;) {
        sys_vsync_wait();

        unsigned long long wev;
        while ((wev = sys_wm_event_poll()) != 0u) {

            unsigned int etype = wm_event_type(wev);

            if (etype == WM_EV_CLOSE_REQUEST) {
                sys_wm_request_close(g_win_id);
                sys_exit(0);
            }

            if (wm_event_is_mouse(wev)) {
                mouse_event_t me = wm_event_mouse_unpack(wev);
                int mx = (int)me.x, my = (int)me.y;
                int lbtn = (int)(me.buttons & 1u);

                /* Update hover state */
                int hc = hit_close(mx, my);
                int hs = hit_shutdown(mx, my);
                int hr = hit_reboot(mx, my);
                int ha = hit_cancel(mx, my);
                if (hc != g_hov_close   || hs != g_hov_shutdown ||
                    hr != g_hov_reboot  || ha != g_hov_cancel) {
                    g_hov_close    = hc;
                    g_hov_shutdown = hs;
                    g_hov_reboot   = hr;
                    g_hov_cancel   = ha;
                    g_needs_draw   = 1;
                }

                /* Click actions (on button release) */
                if (!lbtn && prev_lbtn) {
                    if (hit_close(mx, my) || hit_cancel(mx, my)) {
                        sys_wm_request_close(g_win_id);
                        sys_exit(0);
                    }
                    if (hit_shutdown(mx, my))
                        sys_power_shutdown();
                    if (hit_reboot(mx, my))
                        sys_power_reboot();
                }

                prev_lbtn = lbtn;
            }

            if (etype == WM_EV_REDRAW)
                g_needs_draw = 1;
        }

        if (g_needs_draw) {
            g_needs_draw = 0;
            draw_window();
        }
    }
}
