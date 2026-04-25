/*
 * AetherOS — Window Manager
 * File: kernel/core/wm.c
 *
 * Phase 4.6: kernel-side window registry and key event routing.
 *
 * Design notes:
 *   - Window registry: WM_MAX_WINDOWS static slots; first-free allocation.
 *   - Key FIFO: one WM_KEY_RING-deep ring per PID slot (indexed by pid-1).
 *   - wm_deliver_key()  → routes to g_focused_pid's ring.
 *   - wm_deliver_to_pid() → routes to a specific PID's ring (used by wm_move).
 *   - wm_move() updates position and enqueues WM_EV_REDRAW to the window's PID
 *     so the app redraws at the new coordinates.
 *   - Blocking wait (SYS_WM_KEY_RECV) is implemented in syscall.c by polling
 *     this ring in a task_yield() loop — consistent with how pipe_read works.
 */

#include "aether/wm.h"
#include "aether/printk.h"

/* Must match MAX_TASKS in scheduler.h */
#define WM_MAX_PIDS  32

/* ── Window registry ─────────────────────────────────────────────────────── */

static wm_window_t g_wins[WM_MAX_WINDOWS];
static u32         g_focused_pid;

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

    g_focused_pid = 0;
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

        g_wins[i].active = 1;

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

    if (g_focused_pid == g_wins[id].pid)
        g_focused_pid = 0;

    g_wins[id].active = 0;
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

    g_wins[id].x = x;
    g_wins[id].y = y;

    /* Notify the window's owner so it can redraw at the new position */
    wm_deliver_to_pid(g_wins[id].pid, wm_pack_redraw(x, y));
}
