/*
 * AetherOS — Desktop Manager (PID 1)
 * File: userspace/apps/init/main.c
 *
 * Phase 4.6: init acts as the window manager.
 *
 * WM responsibilities added in this phase:
 *   - Mouse click hit-test against the WM window registry
 *   - Focus change: update focused PID, redraw focus borders
 *   - Title-bar drag: show ghost preview, finalize position on release
 *     (which notifies the app via WM_EV_REDRAW to repaint at new coords)
 *
 * Desktop layout (1024×768):
 *   [0]   Top bar    1024×36
 *   [36]  Accent     1024×2
 *   [38]  Main area  1024×706
 *   [744] Bot bar    1024×24
 */

#include <gfx.h>
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
#define FONT_W      8
#define FONT_H      8

/* Title-bar height used by all windows — must match apps */
#define APP_TITLE_H  28

/* Maximum number of WM window slots to query */
#define WM_WIN_MAX   16

/* ── Helpers ─────────────────────────────────────────────────────────────── */

static void fmt_uptime(char *buf, long ticks)
{
    long s = ticks / 100, m = s / 60; s %= 60;
    long h = m / 60;                  m %= 60;
    snprintf(buf, 16, "%02ld:%02ld:%02ld", h, m, s);
}

/* ── Chrome drawing ──────────────────────────────────────────────────────── */

static void draw_desktop(void)
{
    gfx_fill(0, 0, 1024, 768, C_DESKTOP);
}

static void draw_top_bar(long ticks)
{
    gfx_fill(0, TOPBAR_Y, 1024, TOPBAR_H, C_PANEL);
    gfx_text(14, TOPBAR_Y + 10, "AetherOS", C_TEXT, C_PANEL);
    gfx_text(14 + 8 * FONT_W + 8, TOPBAR_Y + 10, "v0.0.6", C_TEXT_DIM, C_PANEL);
    gfx_text_center(0, 1024, TOPBAR_Y + 10,
                    "Phase 4.6  --  Lumina Desktop", C_TEXT_DIM, C_PANEL);
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
             "AetherOS 0.0.6  |  QEMU virt  |  Cortex-A76  |  1024x768",
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

    /* 2px border on all four sides, drawn as thin filled rects */
    sys_fb_fill(x,         y,         w, 2, color);   /* top    */
    sys_fb_fill(x,         y + h - 2, w, 2, color);   /* bottom */
    sys_fb_fill(x,         y,         2, h, color);   /* left   */
    sys_fb_fill(x + w - 2, y,         2, h, color);   /* right  */
}

/* ── WM helper: find win_id whose owner is pid ───────────────────────────── */

static long find_win_for_pid(long pid)
{
    for (int i = 0; i < WM_WIN_MAX; i++) {
        if (sys_wm_get_pid(i) == pid) return i;
    }
    return -1;
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

/* ── Drag state ──────────────────────────────────────────────────────────── */

static int  g_drag_active    = 0;
static long g_drag_win_id    = -1;
static int  g_drag_off_x     = 0;    /* click offset within title bar */
static int  g_drag_off_y     = 0;
static int  g_drag_ghost_x   = 0;    /* last-drawn ghost position */
static int  g_drag_ghost_y   = 0;
static int  g_drag_ghost_w   = 0;

/* Draw / erase the drag ghost (title-bar + accent strip at current position) */
static void draw_drag_ghost(int x, int y, int w)
{
    sys_fb_fill(x, y,              w, APP_TITLE_H, C_TITLEBAR);
    sys_fb_fill(x, y + APP_TITLE_H, w, 2,          C_ACCENT);
    /* 1px outline so the ghost is visible over the desktop */
    sys_fb_fill(x,     y,     w, 1, C_ACCENT);
    sys_fb_fill(x,     y,     1, APP_TITLE_H + 2, C_ACCENT);
    sys_fb_fill(x+w-1, y,     1, APP_TITLE_H + 2, C_ACCENT);
}

static void erase_drag_ghost(int x, int y, int w)
{
    sys_fb_fill(x, y, w, APP_TITLE_H + 2, C_DESKTOP);
}

/* ── Main ────────────────────────────────────────────────────────────────── */

int main(void)
{
    sys_fb_claim();
    gfx_init();

    draw_desktop();
    draw_top_bar(gfx_ticks());
    draw_bot_bar();

    sys_spawn("/statusbar");
    long term_pid = sys_spawn("/aether_term");

    /* Pre-focus the terminal so keyboard works as soon as the QEMU window
     * is frontmost — without this, g_focused_pid stays 0 and all key events
     * are silently dropped until the user clicks on a window. */
    sys_wm_focus_set(term_pid);

    sys_cursor_show(1);

    int  bar_counter  = 0;
    int  prev_buttons = 0;

    for (;;) {
        if (++bar_counter >= 100) {
            bar_counter = 0;
            refresh_top_bar(gfx_ticks());
            refresh_bot_bar();
        }

        /* ── Process all pending mouse events ────────────────────────── */
        unsigned long long me;
        while ((me = sys_mouse_poll()) != 0) {
            mouse_event_t ev = mouse_event_unpack(me);
            sys_cursor_move(ev.x, ev.y);

            int mx = (int)ev.x;
            int my = (int)ev.y;
            int btn     = (int)(ev.buttons & 1);
            int pressed  = btn && !prev_buttons;   /* rising edge  */
            int released = !btn && prev_buttons;   /* falling edge */
            prev_buttons = btn;

            /* ── Left button pressed: hit-test + focus + drag start ── */
            if (pressed) {
                long win_id = hit_test(mx, my);
                if (win_id >= 0) {
                    long new_pid  = sys_wm_get_pid(win_id);
                    long prev_pid = sys_wm_focus_get();

                    /* Update focus if it changed */
                    if (new_pid != (long)prev_pid) {
                        /* Dim old border */
                        if (prev_pid) {
                            long old_win = find_win_for_pid(prev_pid);
                            if (old_win >= 0)
                                draw_focus_border(old_win, 0);
                        }
                        /* Highlight new border */
                        draw_focus_border(win_id, 1);
                        sys_wm_focus_set(new_pid);
                    }

                    /* Start drag if click is in title bar */
                    long pos  = sys_wm_get_pos(win_id);
                    long size = sys_wm_get_size(win_id);
                    if (pos != -1 && size != 0) {
                        int wx = (int)((unsigned long long)pos  >> 32);
                        int wy = (int)((unsigned long long)pos  & 0xFFFFFFFFu);
                        int ww = (int)((unsigned long long)size >> 32);

                        if (my >= wy && my < wy + APP_TITLE_H) {
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
            }

            /* ── Button held during drag: move ghost ─────────────────── */
            if (g_drag_active && btn) {
                int nx = mx - g_drag_off_x;
                int ny = my - g_drag_off_y;

                /* Clamp so window stays on-screen */
                if (nx < 0) nx = 0;
                if (ny < TOPBAR_H + ACCENT_H) ny = TOPBAR_H + ACCENT_H;
                if (nx + g_drag_ghost_w > 1024) nx = 1024 - g_drag_ghost_w;
                if (ny + APP_TITLE_H > BOTBAR_Y) ny = BOTBAR_Y - APP_TITLE_H;

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
                /* Erase ghost */
                erase_drag_ghost(g_drag_ghost_x, g_drag_ghost_y,
                                 g_drag_ghost_w);

                /* Update WM registry + notify app to repaint */
                sys_wm_move(g_drag_win_id,
                            g_drag_ghost_x, g_drag_ghost_y);

                /* Redraw focus border at new position
                 * (app will repaint its own content after WM_EV_REDRAW) */
                draw_focus_border(g_drag_win_id, 1);

                g_drag_active = 0;
                g_drag_win_id = -1;
            }
        }

        sys_sleep(1);
    }

    return 0;
}
