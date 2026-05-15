/*
 * AetherOS — Top Bar daemon
 * File: userspace/apps/topbar/main.c
 *
 * macOS-style menu bar: full-width strip at the top of the screen.
 * Runs as a standalone process spawned by init after the compositor.
 * Uses a GPU BO so the compositor z-orders it above all app windows.
 *
 * Layout:
 *   Left  : "AetherOS  v0.0.7"
 *   Center: "Lumina Desktop — Phase 6.2"
 *   Right : HH:MM (refreshed every ~100 vsync ticks)
 *   Bottom: 2 px accent line (C_ACCENT)
 *
 * Future: add system-icon slots for network, sound, settings (macOS-style).
 */

#include <gfx.h>
#include <gpu.h>
#include <sys.h>
#include <string.h>
#include <stdio.h>

#define TOPBAR_H    36
#define ACCENT_H     2
#define BAR_H       (TOPBAR_H + ACCENT_H)

static int SCR_W;
static int SCR_H;

static long          g_win_id = -1;
static gpu_bo_t      g_bo     = GPU_BO_INVALID;
static unsigned int *g_bo_ptr = NULL;

static void fmt_time(char *buf, unsigned long ts)
{
    unsigned long s = ts % 86400UL;
    unsigned long h = s / 3600UL;
    unsigned long m = (s % 3600UL) / 60UL;
    snprintf(buf, 6, "%02lu:%02lu", h, m);
}

/* ── Full redraw ─────────────────────────────────────────────────────────── */

static void draw_topbar(void)
{
    if (g_bo_ptr)
        gfx_begin_frame(g_bo_ptr, (unsigned)SCR_W, BAR_H, 0, 0);

    gfx_fill(0, 0, (unsigned)SCR_W, TOPBAR_H, C_PANEL);

    /* Branding — left */
    gfx_text(14, 10, "AetherOS", C_TEXT, C_PANEL);
    gfx_text((unsigned)(14 + gfx_text_width("AetherOS") + 8), 10,
             "v0.0.7", C_TEXT_DIM, C_PANEL);

    /* Center label */
    gfx_text_center(0, (unsigned)SCR_W, 10,
                    "Lumina Desktop  \xe2\x80\x94  Phase 6.2",
                    C_TEXT_DIM, C_PANEL);

    /* Time — right */
    char tbuf[6];
    fmt_time(tbuf, sys_rtc_get());
    gfx_text((unsigned)(SCR_W - gfx_text_width(tbuf) - 14), 10,
             tbuf, C_TEXT_DIM, C_PANEL);

    /* Accent line */
    gfx_fill(0, TOPBAR_H, (unsigned)SCR_W, ACCENT_H, C_ACCENT);

    if (g_bo_ptr)
        gfx_end_frame();
}

/* ── Partial refresh: erase right third and redraw time ─────────────────── */

static void refresh_time(void)
{
    char tbuf[6];
    fmt_time(tbuf, sys_rtc_get());

    if (g_bo_ptr)
        gfx_begin_frame(g_bo_ptr, (unsigned)SCR_W, BAR_H, 0, 0);

    unsigned erase_x = (unsigned)(SCR_W * 2 / 3);
    gfx_fill(erase_x, 0, (unsigned)SCR_W - erase_x, TOPBAR_H, C_PANEL);
    gfx_text((unsigned)(SCR_W - gfx_text_width(tbuf) - 14), 10,
             tbuf, C_TEXT_DIM, C_PANEL);

    if (g_bo_ptr)
        gfx_end_frame();
}

/* ── Main ────────────────────────────────────────────────────────────────── */

int main(void)
{
    gfx_init();
    SCR_W = (int)gfx_width();
    SCR_H = (int)gfx_height();

    g_win_id = sys_wm_register(0, 0, (unsigned)SCR_W, BAR_H, "topbar");
    if (g_win_id >= 0) {
        sys_wm_set_zindex(g_win_id, WM_Z_DOCK);
        sys_wm_set_flags(g_win_id, WM_FLAG_NO_CHROME);
    }

    /* Allocate GPU BO so the compositor can composite the topbar at the
     * correct z-order above all app windows. */
    unsigned bo_bytes = (unsigned)SCR_W * (unsigned)BAR_H * 4u;
    if (g_win_id >= 0) {
        g_bo = gpu_alloc(bo_bytes);
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
    }

    draw_topbar();

    int tick_counter = 0;
    for (;;) {
        sys_vsync_wait();
        if (++tick_counter >= 100) {
            tick_counter = 0;
            refresh_time();
        }
    }
}
