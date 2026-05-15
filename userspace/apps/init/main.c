/*
 * AetherOS — Desktop Manager (PID 1)
 * File: userspace/apps/init/main.c
 *
 * Desktop layout (dynamic — queried from kernel via SYS_FB_INFO):
 *   [0]         Top bar    SCR_W × 36  (topbar process, GPU BO)
 *   [36]        Accent     SCR_W × 2
 *   [38]        Main area  SCR_W × (SCR_H - 36 - 2 - 56)
 *   [SCR_H-56]  Dock       SCR_W × 56 (dock process, GPU BO)
 */

#include <gfx.h>
#include <sys.h>
#include <input.h>
#include <widget.h>

/* ── Layout constants (size-invariant) ───────────────────────────────────── */

#define TOPBAR_Y    0
#define TOPBAR_H   36
#define ACCENT_Y   36
#define ACCENT_H    2
#define DOCK_H     56

/*
 * Position variables derived from screen size — set in main() after gfx_init().
 * Upper-case names kept for minimal diff against previous hard-coded usage.
 */
static int SCR_W;
static int SCR_H;
static int DOCK_Y;     /* SCR_H - DOCK_H */

/* Title-bar height used by all windows — must match apps */
#define APP_TITLE_H  28

/* Maximum number of WM window slots to query */
#define WM_WIN_MAX   16

/* (topbar glass moved to userspace/apps/topbar/main.c — Phase 7) */

/* ── WM8: subprocess PIDs and focus tracking ─────────────────────────────── */
static long g_dock_pid       = 0;  /* dock process; init forwards mouse events to it */
static long g_desktop_pid    = 0;  /* desktop process; owns wallpaper + icons */
static long g_prev_focus_pid = 0;  /* last known focused pid; detect external changes */

/* ── Helpers ─────────────────────────────────────────────────────────────── */

/* ── WM helper: find win_id whose owner is pid ───────────────────────────── */

static long find_win_for_pid(long pid)
{
    for (int i = 0; i < WM_WIN_MAX; i++) {
        if (sys_wm_get_pid(i) == pid) return i;
    }
    return -1;
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

    /* WM7b resize grip indicator: 3 diagonal dots at bottom-right corner */
    if (focused) {
        unsigned gc = C_ACCENT;
        int rx = x + w - 5;
        int ry = y + h - 5;
        gfx_fill((unsigned)rx,       (unsigned)ry,       2, 2, gc);
        gfx_fill((unsigned)(rx - 4), (unsigned)ry,       2, 2, gc);
        gfx_fill((unsigned)rx,       (unsigned)(ry - 4), 2, 2, gc);
    }
}

/* ── WM helper: traffic-light button hit-tests ───────────────────────────── */

static int hit_close_button(int wx, int wy, int mx, int my)
{
    return mx >= wx + 10 && mx < wx + 22 &&
           my >= wy +  8 && my < wy + 20;
}

/* Yellow minimize button at wx+26, same vertical range as close */
static int hit_minimize_button(int wx, int wy, int mx, int my)
{
    return mx >= wx + 26 && mx < wx + 38 &&
           my >= wy +  8 && my < wy + 20;
}

/* 8×8 resize grip at bottom-right corner of the window frame (WM7b) */
static int hit_resize_handle(int wx, int wy, int ww, int wh, int mx, int my)
{
    return mx >= wx + ww - 8 && mx < wx + ww &&
           my >= wy + wh - 8 && my < wy + wh;
}

/* ── WM helper: repaint desktop after a window closes ───────────────────── */

static void on_window_closed(unsigned long long ev)
{
    int cx, cy, cw, ch;
    wm_decode_closed(ev, &cx, &cy, &cw, &ch);

    /* Forward window-closed event to dock so it refreshes running indicators */
    if (g_dock_pid > 0)
        sys_wm_push_event(g_dock_pid, ev);

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
            if (pid && pid != g_dock_pid) {
                sys_wm_focus_set(pid);
                g_prev_focus_pid = pid;
                draw_focus_border(i, 1);
                break;
            }
        }
    }

    /* Redraw focus borders for all surviving windows */
    long focused_pid = sys_wm_focus_get();
    for (int i = 0; i < WM_WIN_MAX; i++) {
        long pid = sys_wm_get_pid(i);
        if (pid && pid != g_dock_pid)
            draw_focus_border(i, (pid == focused_pid));
    }
}

/* ── WM helper: hit-test click (x, y) against registered windows ─────────── */

static long hit_test(int mx, int my, unsigned char *out_flags)
{
    /* Return the window with the highest z-index that contains (mx, my).
     * Minimized and invisible windows are excluded so they cannot receive
     * mouse events after being hidden. */
    wm_entry_t entries[WM_WIN_MAX];
    int n = sys_wm_enum(entries, WM_WIN_MAX);
    long best_id          = -1;
    int  best_z           = -1;
    unsigned char best_flags = 0;
    for (int i = 0; i < n; i++) {
        wm_entry_t *e = &entries[i];
        if (!e->pid || e->minimized || !e->visible) continue;
        if (mx >= e->x && mx < e->x + e->w &&
            my >= e->y && my < e->y + e->h) {
            if (e->z_index > best_z) {
                best_z     = e->z_index;
                best_id    = (long)e->win_id;
                best_flags = e->flags;
            }
        }
    }
    if (out_flags) *out_flags = best_flags;
    return best_id;
}

/* Raise a window to the top of the z-stack (capped below WM_Z_DOCK so the
 * dock always stays on top) and ask it to repaint itself. */
static void raise_to_front(long win_id)
{
    int max_z = 0;
    for (int i = 0; i < WM_WIN_MAX; i++) {
        if (!sys_wm_get_pid(i)) continue;
        int z = sys_wm_get_zindex(i);
        if (z >= WM_Z_DOCK) continue;   /* dock stays above all regular windows */
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
    sys_fb_fill((unsigned)x, (unsigned)y, (unsigned)w, APP_TITLE_H + 2, 0x0d1117u);
}

/* ── Resize drag state (WM7b) ────────────────────────────────────────────── */

#define WIN_MIN_W  160
#define WIN_MIN_H  120

static int  g_resize_active   = 0;
static long g_resize_win_id   = -1;
static int  g_resize_orig_x   = 0;   /* window top-left at drag start */
static int  g_resize_orig_y   = 0;
static int  g_resize_orig_w   = 0;   /* original size */
static int  g_resize_orig_h   = 0;
static int  g_resize_start_mx = 0;   /* mouse position at drag start */
static int  g_resize_start_my = 0;
static int  g_resize_cur_w    = 0;   /* current ghost size */
static int  g_resize_cur_h    = 0;

/* Draw an outline ghost of the new window size during resize drag */
static void draw_resize_ghost(int wx, int wy, int nw, int nh)
{
    sys_fb_fill((unsigned)wx,          (unsigned)wy,          (unsigned)nw, 1, C_ACCENT);
    sys_fb_fill((unsigned)wx,          (unsigned)(wy + nh - 1),(unsigned)nw, 1, C_ACCENT);
    sys_fb_fill((unsigned)wx,          (unsigned)wy,          1, (unsigned)nh, C_ACCENT);
    sys_fb_fill((unsigned)(wx + nw - 1),(unsigned)wy,          1, (unsigned)nh, C_ACCENT);
}

static void erase_resize_ghost(int wx, int wy, int nw, int nh)
{
    sys_fb_fill((unsigned)wx, (unsigned)wy, (unsigned)nw, (unsigned)nh, 0x0d1117u);
}

/* ── Main ────────────────────────────────────────────────────────────────── */

int main(void)
{
    sys_fb_claim();
    gfx_init();

    /* Compute layout from actual screen dimensions */
    SCR_W  = (int)gfx_width();
    SCR_H  = (int)gfx_height();
    DOCK_Y = SCR_H - DOCK_H;

    /* Spawn compositor then system processes.  Compositor registers as the WM
     * compositor so the kernel routes WM_EV_CLOSE_REQUEST events to it.
     * Desktop owns wallpaper + icons as a GPU BO window (below all apps). */
    sys_spawn("/compositor");
    sys_spawn("/topbar");
    g_desktop_pid = sys_spawn("/desktop");
    g_dock_pid    = sys_spawn("/dock");

    sys_cursor_show(1);

    int  prev_buttons = 0;

    for (;;) {
        /* ── Detect focus changes from dock or other external processes ── */
        {
            long cur_focus = sys_wm_focus_get();
            if (cur_focus != g_prev_focus_pid) {
                g_prev_focus_pid = cur_focus;
                for (int i = 0; i < WM_WIN_MAX; i++) {
                    long pid = sys_wm_get_pid(i);
                    if (pid && pid != g_dock_pid)
                        draw_focus_border(i, (pid == cur_focus));
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

            /* Forward ALL mouse events to dock for hover-magnification tracking */
            if (g_dock_pid > 0)
                sys_wm_push_event(g_dock_pid,
                                  wm_pack_mouse((unsigned)mx, (unsigned)my,
                                                ev.buttons));

            /* ── Left button pressed ─────────────────────────────────── */
            if (pressed) {
                /* Dock area — already forwarded above; skip window handling */
                if (my >= DOCK_Y)
                    continue;

                unsigned char win_flags = 0;
                long win_id = hit_test(mx, my, &win_flags);
                /* System chrome (topbar, dock) is fixed — no drag/resize/focus border */
                int is_chrome = (int)(win_flags & WM_FLAG_NO_CHROME);
                if (win_id >= 0) {
                    long new_pid = sys_wm_get_pid(win_id);
                    if (!is_chrome) {
                        /* Raise app windows to front; desktop/topbar/dock stay fixed */
                        raise_to_front(win_id);

                        long prev_pid = sys_wm_focus_get();
                        if (new_pid != (long)prev_pid) {
                            if (prev_pid) {
                                long old_win = find_win_for_pid(prev_pid);
                                if (old_win >= 0)
                                    draw_focus_border(old_win, 0);
                            }
                            draw_focus_border(win_id, 1);
                            sys_wm_focus_set(new_pid);
                            g_prev_focus_pid = new_pid;
                        }
                    }

                    long pos  = sys_wm_get_pos(win_id);
                    long size = sys_wm_get_size(win_id);
                    if (pos != -1 && size != 0) {
                        int wx = (int)((unsigned long long)pos  >> 32);
                        int wy = (int)((unsigned long long)pos  & 0xFFFFFFFFu);
                        int ww = (int)((unsigned long long)size >> 32);
                        int wh = (int)((unsigned long long)size & 0xFFFFFFFFu);

                        if (!is_chrome && my >= wy && my < wy + APP_TITLE_H) {
                            if (hit_close_button(wx, wy, mx, my)) {
                                sys_wm_close(win_id);
                            } else if (hit_minimize_button(wx, wy, mx, my)) {
                                sys_wm_minimize(win_id);
                                /* Notify dock so it refreshes the minimized thumbnail list */
                                if (g_dock_pid > 0) {
                                    unsigned long long min_ev =
                                        ((unsigned long long)WM_EV_MINIMIZE << 56)
                                        | ((unsigned long long)(unsigned)win_id << 48);
                                    sys_wm_push_event(g_dock_pid, min_ev);
                                }
                                /* Transfer focus away from the minimized window */
                                if (sys_wm_focus_get() == new_pid) {
                                    sys_wm_focus_set(0);
                                    g_prev_focus_pid = 0;
                                    for (int fi = 0; fi < WM_WIN_MAX; fi++) {
                                        long fpid = sys_wm_get_pid(fi);
                                        if (fpid && fpid != new_pid && fpid != g_dock_pid) {
                                            sys_wm_focus_set(fpid);
                                            g_prev_focus_pid = fpid;
                                            draw_focus_border(fi, 1);
                                            break;
                                        }
                                    }
                                }
                                /* Clear the minimized window: redraw surviving focus borders */
                                {
                                    long focused_after = sys_wm_focus_get();
                                    for (int fi = 0; fi < WM_WIN_MAX; fi++) {
                                        long fpid = sys_wm_get_pid(fi);
                                        if (fpid && fpid != new_pid && fpid != g_dock_pid)
                                            draw_focus_border(fi, (fpid == focused_after));
                                    }
                                }
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
                        } else if (!is_chrome && hit_resize_handle(wx, wy, ww, wh, mx, my)) {
                            /* WM7b: start resize drag from bottom-right grip */
                            g_resize_active   = 1;
                            g_resize_win_id   = win_id;
                            g_resize_orig_x   = wx;
                            g_resize_orig_y   = wy;
                            g_resize_orig_w   = ww;
                            g_resize_orig_h   = wh;
                            g_resize_start_mx = mx;
                            g_resize_start_my = my;
                            g_resize_cur_w    = ww;
                            g_resize_cur_h    = wh;
                            draw_resize_ghost(wx, wy, ww, wh);
                        }
                    }
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

            /* ── Button held during resize: update ghost ─────────────── */
            if (g_resize_active && btn) {
                int dx = mx - g_resize_start_mx;
                int dy = my - g_resize_start_my;
                int nw = g_resize_orig_w + dx;
                int nh = g_resize_orig_h + dy;
                if (nw < WIN_MIN_W) nw = WIN_MIN_W;
                if (nh < WIN_MIN_H) nh = WIN_MIN_H;
                /* Clamp so the window doesn't overflow the screen */
                if (g_resize_orig_x + nw > SCR_W) nw = SCR_W - g_resize_orig_x;
                if (g_resize_orig_y + nh > DOCK_Y) nh = DOCK_Y - g_resize_orig_y;
                if (nw != g_resize_cur_w || nh != g_resize_cur_h) {
                    erase_resize_ghost(g_resize_orig_x, g_resize_orig_y,
                                       g_resize_cur_w,  g_resize_cur_h);
                    g_resize_cur_w = nw;
                    g_resize_cur_h = nh;
                    draw_resize_ghost(g_resize_orig_x, g_resize_orig_y, nw, nh);
                }
            }

            /* ── Button released: commit resize ─────────────────────── */
            if (g_resize_active && released) {
                erase_resize_ghost(g_resize_orig_x, g_resize_orig_y,
                                   g_resize_cur_w,  g_resize_cur_h);
                sys_wm_resize(g_resize_win_id, g_resize_cur_w, g_resize_cur_h);
                draw_focus_border(g_resize_win_id, 1);
                g_resize_active = 0;
                g_resize_win_id = -1;
            }

            /* ── Forward mouse event to the window under the cursor ──── */
            /* Skip the dock: it already received the event at the top of
             * this loop. Forwarding again would double-deliver. */
            if (!g_drag_active && !g_resize_active) {
                long fw_id = hit_test(mx, my, (void *)0);
                if (fw_id >= 0) {
                    long fw_pid = sys_wm_get_pid(fw_id);
                    if (fw_pid > 1 && fw_pid != g_dock_pid)
                        sys_wm_push_event(fw_pid,
                                          wm_pack_mouse((unsigned)mx,
                                                        (unsigned)my,
                                                        ev.buttons));
                }
            }
        }

        sys_vsync_wait();   /* pace the event loop to ~60 fps */
    }

    return 0;
}
