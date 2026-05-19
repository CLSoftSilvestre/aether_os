/*
 * AetherOS — Window Manager
 * File: kernel/core/wm.c
 *
 * Phase 4.6: kernel-side window registry and key event routing.
 * wmanager branch: compositing extension — z-index, opacity, blur, GPU BO,
 * damage tracking, compositor PID registration, and wm_enum().
 *
 * Design notes:
 *   - Window registry: WM_MAX_WINDOWS static slots; first-free allocation.
 *   - Key FIFO: one WM_KEY_RING-deep ring per PID slot (indexed by pid-1).
 *   - wm_deliver_key()  → routes to g_focused_pid's ring.
 *   - wm_deliver_to_pid() → routes to a specific PID's ring.
 *   - wm_move() updates position and sends WM_EV_REDRAW to the window's PID.
 *   - wm_damage() marks a window dirty and sends WM_EV_DAMAGE to the
 *     registered compositor PID so it knows to refresh the scene.
 *   - wm_enum() fills a wm_entry_t array for the compositor to inspect.
 */

#include "aether/wm.h"
#include "aether/printk.h"

/* Must match MAX_TASKS in scheduler.h */
#define WM_MAX_PIDS  32

/* ── Window registry ─────────────────────────────────────────────────────── */

static wm_window_t g_wins[WM_MAX_WINDOWS];
static u32         g_focused_pid;
static u32         g_compositor_pid;

/* ── Per-PID key event ring buffers ──────────────────────────────────────── */

typedef struct {
    u64 buf[WM_KEY_RING];
    u32 head;   /* write index */
    u32 tail;   /* read index  */
} key_fifo_t;

static key_fifo_t g_fifos[WM_MAX_PIDS];   /* index = pid - 1 */

/* ── Initialisation ─────────────────────────────────────────────────────── */

void wm_init(void)
{
    for (int i = 0; i < WM_MAX_WINDOWS; i++)
        g_wins[i].active = 0;

    for (int i = 0; i < WM_MAX_PIDS; i++)
        g_fifos[i].head = g_fifos[i].tail = 0;

    g_focused_pid    = 0;
    g_compositor_pid = 0;
    kinfo("[WM] window manager initialised\n");
}

/* ── Window registry ─────────────────────────────────────────────────────── */

int wm_register(u32 pid, int x, int y, int w, int h, const char *title)
{
    for (int i = 0; i < WM_MAX_WINDOWS; i++) {
        if (g_wins[i].active)
            continue;

        g_wins[i].pid = pid;
        g_wins[i].x   = x;
        g_wins[i].y   = y;
        g_wins[i].w   = w;
        g_wins[i].h   = h;

        int j = 0;
        while (title && title[j] && j < 31) {
            g_wins[i].title[j] = title[j];
            j++;
        }
        g_wins[i].title[j] = '\0';

        g_wins[i].active      = 1;
        g_wins[i].z_index     = i;      /* default: registration order */
        g_wins[i].opacity     = 255;    /* fully opaque */
        g_wins[i].blur_radius = 0;
        g_wins[i].damaged     = 1;      /* needs first composite */
        g_wins[i].visible     = 1;
        g_wins[i].closing     = 0;
        g_wins[i].minimized   = 0;
        g_wins[i].flags       = 0;
        g_wins[i].buf_handle  = 0;

        kinfo("[WM] register pid=%u win=%d (%dx%d+%d+%d) '%s'\n",
              pid, i, w, h, x, y, g_wins[i].title);
        return i;
    }

    kwarn("[WM] register: no free slots (pid=%u)\n", pid);
    return -1;
}

void wm_unregister(int id)
{
    if (id < 0 || id >= WM_MAX_WINDOWS || !g_wins[id].active)
        return;

    kinfo("[WM] unregister win=%d pid=%u\n", id, g_wins[id].pid);

    /* Snapshot rect before clearing — init needs it to repaint the desktop */
    int x = g_wins[id].x, y = g_wins[id].y;
    int w = g_wins[id].w, h = g_wins[id].h;

    if (g_focused_pid == g_wins[id].pid)
        g_focused_pid = 0;

    g_wins[id].active = 0;

    /* Notify init (PID 1) so it can repaint the vacated region */
    wm_deliver_to_pid(1, wm_pack_window_closed(x, y, w, h));
}

void wm_unregister_by_pid(u32 pid, int force)
{
    if (!pid)
        return;

    for (int i = 0; i < WM_MAX_WINDOWS; i++) {
        if (g_wins[i].active && g_wins[i].pid == pid) {
            if (!force && g_wins[i].closing)
                return;   /* compositor owns the close — leave BO intact for animation */
            wm_unregister(i);
            return;   /* one window per PID in current design */
        }
    }
}

/* ── Focus management ────────────────────────────────────────────────────── */

void wm_focus_set(u32 pid) { g_focused_pid = pid; }
u32  wm_focus_get(void)    { return g_focused_pid; }

/* ── Position / size / pid queries ─────────────────────────────────────── */

long wm_get_pos(int id)
{
    if (id < 0 || id >= WM_MAX_WINDOWS || !g_wins[id].active)
        return -1L;
    return ((long)g_wins[id].x << 32) | (long)(unsigned int)g_wins[id].y;
}

long wm_get_size(int id)
{
    if (id < 0 || id >= WM_MAX_WINDOWS || !g_wins[id].active)
        return 0L;
    return ((long)g_wins[id].w << 32) | (long)(unsigned int)g_wins[id].h;
}

u32 wm_get_pid(int id)
{
    if (id < 0 || id >= WM_MAX_WINDOWS || !g_wins[id].active)
        return 0;
    return g_wins[id].pid;
}

int wm_is_pid_minimized(u32 pid)
{
    if (!pid) return 0;
    for (int i = 0; i < WM_MAX_WINDOWS; i++)
        if (g_wins[i].active && g_wins[i].pid == pid && g_wins[i].minimized)
            return 1;
    return 0;
}

const wm_window_t *wm_get_window(int id)
{
    if (id < 0 || id >= WM_MAX_WINDOWS || !g_wins[id].active)
        return (void *)0;
    return &g_wins[id];
}

/* ── Key event routing ────────────────────────────────────────────────────── */

void wm_deliver_to_pid(u32 pid, u64 packed)
{
    if (!pid)
        return;

    u32 idx = pid - 1;
    if (idx >= WM_MAX_PIDS)
        return;

    key_fifo_t *f    = &g_fifos[idx];
    u32         next = (f->head + 1) % WM_KEY_RING;

    if (next == f->tail)
        return;   /* ring full — drop event */

    f->buf[f->head] = packed;
    f->head         = next;
}

void wm_deliver_key(u64 packed)
{
    wm_deliver_to_pid(g_focused_pid, packed);
}

u64 wm_key_dequeue(u32 pid)
{
    if (!pid)
        return 0;

    u32 idx = pid - 1;
    if (idx >= WM_MAX_PIDS)
        return 0;

    key_fifo_t *f = &g_fifos[idx];
    if (f->head == f->tail)
        return 0;

    u64 ev   = f->buf[f->tail];
    f->tail  = (f->tail + 1) % WM_KEY_RING;
    return ev;
}

/* ── Window move (notifies app via WM_EV_REDRAW) ─────────────────────────── */

void wm_move(int id, int x, int y)
{
    if (id < 0 || id >= WM_MAX_WINDOWS || !g_wins[id].active)
        return;

    int old_x = g_wins[id].x;
    int old_y = g_wins[id].y;
    int w     = g_wins[id].w;
    int h     = g_wins[id].h;

    g_wins[id].x       = x;
    g_wins[id].y       = y;
    g_wins[id].damaged = 1;

    /* Legacy: tell init to repaint the vacated region */
    wm_deliver_to_pid(1, wm_pack_window_closed(old_x, old_y, w, h));

    /* Notify window owner so it can redraw at the new position */
    wm_deliver_to_pid(g_wins[id].pid, wm_pack_redraw(x, y));

    /* Notify compositor so it re-composites the moved window */
    if (g_compositor_pid)
        wm_deliver_to_pid(g_compositor_pid, wm_pack_damage(id));
}

/* ── Compositing API ─────────────────────────────────────────────────────── */

void wm_set_zindex(int id, s32 z)
{
    if (id < 0 || id >= WM_MAX_WINDOWS || !g_wins[id].active) return;
    g_wins[id].z_index = z;
}

void wm_set_opacity(int id, u8 opacity)
{
    if (id < 0 || id >= WM_MAX_WINDOWS || !g_wins[id].active) return;
    g_wins[id].opacity = opacity;
}

void wm_set_blur(int id, u8 blur_radius)
{
    if (id < 0 || id >= WM_MAX_WINDOWS || !g_wins[id].active) return;
    g_wins[id].blur_radius = blur_radius;
}

void wm_set_visible(int id, u8 visible)
{
    if (id < 0 || id >= WM_MAX_WINDOWS || !g_wins[id].active) return;
    g_wins[id].visible = visible ? 1 : 0;
}

s32 wm_get_zindex(int id)
{
    if (id < 0 || id >= WM_MAX_WINDOWS || !g_wins[id].active) return 0;
    return g_wins[id].z_index;
}

void wm_set_flags(int id, u8 flags)
{
    if (id < 0 || id >= WM_MAX_WINDOWS || !g_wins[id].active) return;
    g_wins[id].flags = flags;
}

u8 wm_get_flags(int id)
{
    if (id < 0 || id >= WM_MAX_WINDOWS || !g_wins[id].active) return 0;
    return g_wins[id].flags;
}

void wm_set_buffer(int id, u32 buf_handle)
{
    if (id < 0 || id >= WM_MAX_WINDOWS || !g_wins[id].active) return;
    g_wins[id].buf_handle = buf_handle;
}

u32 wm_get_buffer(int id)
{
    if (id < 0 || id >= WM_MAX_WINDOWS || !g_wins[id].active) return 0;
    return g_wins[id].buf_handle;
}

void wm_damage(int id)
{
    if (id < 0 || id >= WM_MAX_WINDOWS || !g_wins[id].active) return;
    g_wins[id].damaged = 1;
    if (g_compositor_pid)
        wm_deliver_to_pid(g_compositor_pid, wm_pack_damage(id));
}

void wm_damage_clear(int id)
{
    if (id < 0 || id >= WM_MAX_WINDOWS || !g_wins[id].active) return;
    g_wins[id].damaged = 0;
}

void wm_request_close(int id)
{
    if (id < 0 || id >= WM_MAX_WINDOWS || !g_wins[id].active) return;
    if (g_wins[id].closing) return;   /* already closing */
    g_wins[id].closing = 1;
    kinfo("[WM] request_close win=%d pid=%u\n", id, g_wins[id].pid);
    if (g_compositor_pid)
        wm_deliver_to_pid(g_compositor_pid, wm_pack_close_request(id));
}

void wm_close_done(int id)
{
    if (id < 0 || id >= WM_MAX_WINDOWS || !g_wins[id].active) return;
    if (!g_wins[id].closing) return;  /* not in close protocol */
    kinfo("[WM] close_done win=%d\n", id);
    wm_unregister(id);
}

void wm_minimize(int id)
{
    if (id < 0 || id >= WM_MAX_WINDOWS || !g_wins[id].active) return;
    if (g_wins[id].minimized) return;   /* already minimized */
    g_wins[id].minimized = 1;
    kinfo("[WM] minimize win=%d pid=%u\n", id, g_wins[id].pid);
    if (g_compositor_pid)
        wm_deliver_to_pid(g_compositor_pid, wm_pack_minimize_event(id));
}

void wm_restore(int id)
{
    if (id < 0 || id >= WM_MAX_WINDOWS || !g_wins[id].active) return;
    if (!g_wins[id].minimized) return;  /* not minimized */
    g_wins[id].minimized = 0;
    g_wins[id].visible   = 1;
    kinfo("[WM] restore win=%d pid=%u\n", id, g_wins[id].pid);
    if (g_compositor_pid)
        wm_deliver_to_pid(g_compositor_pid, wm_pack_restore_event(id));
    /* Tell the app to repaint itself — unblocks sys_fb_blit and triggers redraw */
    wm_deliver_to_pid(g_wins[id].pid,
                      wm_pack_redraw(g_wins[id].x, g_wins[id].y));
}

/* WM7b: minimum window dimensions */
#define WM_MIN_W  160
#define WM_MIN_H  120

void wm_resize(int id, int new_w, int new_h)
{
    if (id < 0 || id >= WM_MAX_WINDOWS || !g_wins[id].active) return;
    if (new_w < WM_MIN_W) new_w = WM_MIN_W;
    if (new_h < WM_MIN_H) new_h = WM_MIN_H;
    g_wins[id].w       = new_w;
    g_wins[id].h       = new_h;
    g_wins[id].damaged = 1;
    kinfo("[WM] resize win=%d → %dx%d pid=%u\n", id, new_w, new_h, g_wins[id].pid);
    /* Notify the app so it can reallocate its GPU BO */
    wm_deliver_to_pid(g_wins[id].pid, wm_pack_resize_event(new_w, new_h));
    /* Notify the compositor to composite the stretched frame */
    if (g_compositor_pid)
        wm_deliver_to_pid(g_compositor_pid, wm_pack_damage(id));
}

void wm_set_compositor(u32 pid)
{
    g_compositor_pid = pid;
    kinfo("[WM] compositor PID set to %u\n", pid);
}

u32 wm_get_compositor(void)
{
    return g_compositor_pid;
}

int wm_enum(wm_entry_t *entries, int max)
{
    if (!entries || max <= 0) return 0;
    int count = 0;
    for (int i = 0; i < WM_MAX_WINDOWS && count < max; i++) {
        if (!g_wins[i].active) continue;
        wm_entry_t *e = &entries[count++];
        e->win_id      = i;
        e->pid         = g_wins[i].pid;
        e->z_index     = g_wins[i].z_index;
        e->x           = g_wins[i].x;
        e->y           = g_wins[i].y;
        e->w           = g_wins[i].w;
        e->h           = g_wins[i].h;
        e->opacity     = g_wins[i].opacity;
        e->blur_radius = g_wins[i].blur_radius;
        e->damaged     = g_wins[i].damaged;
        e->visible     = g_wins[i].visible;
        e->closing     = g_wins[i].closing;
        e->minimized   = g_wins[i].minimized;
        e->flags       = g_wins[i].flags;
        e->_pad        = 0;
        e->buf_handle  = g_wins[i].buf_handle;
        int j = 0;
        while (j < 31 && g_wins[i].title[j]) {
            e->title[j] = g_wins[i].title[j];
            j++;
        }
        e->title[j] = '\0';
    }
    return count;
}
