/*
 * AetherOS — Window Manager Kernel Interface
 * File: kernel/include/aether/wm.h
 *
 * Phase 4.6: kernel-side window registry.
 *   - Tracks up to WM_MAX_WINDOWS registered windows (pid, rect, title).
 *   - Maintains the focused PID and routes key events to its per-PID FIFO.
 *   - Provides position/size/pid query helpers for init's WM loop.
 *
 * Key event routing:
 *   Hardware kbd IRQ → wm_deliver_key() → focused PID's ring
 *   SYS_WM_KEY_RECV polls own ring, yielding until an event arrives.
 *
 * WM_EV_REDRAW:
 *   When a window is moved (wm_move), the app receives a packed WM event
 *   with keycode = WM_EV_REDRAW and new (x, y) embedded in bits [31:0].
 *   Userspace helpers in input.h decode this.
 */

#ifndef AETHER_WM_H
#define AETHER_WM_H

#include "aether/types.h"

/* Maximum number of simultaneously registered windows */
#define WM_MAX_WINDOWS  16

/* Per-process key event ring size (events dropped if full) */
#define WM_KEY_RING     16

/*
 * WM_EV_REDRAW — synthetic WM event delivered to a window's PID when its
 * position changes via wm_move().  The keycode field in the packed u64 is
 * set to this value; bits [31:16] = new x, bits [15:0] = new y.
 */
#define WM_EV_REDRAW  0xFEu

/* ── Window registry entry ───────────────────────────────────────────────── */

typedef struct {
    u32  pid;           /* owning process */
    int  x, y;         /* top-left corner (screen coordinates) */
    int  w, h;         /* dimensions */
    char title[32];    /* display title */
    int  active;       /* 1 = registered, 0 = free slot */
} wm_window_t;

/* ── Public API ──────────────────────────────────────────────────────────── */

void wm_init(void);

/* Register a window for pid; returns win_id (0 … WM_MAX_WINDOWS-1) or -1 */
int  wm_register(u32 pid, int x, int y, int w, int h, const char *title);

/* Unregister a window; clears focus if the window owned it */
void wm_unregister(int win_id);

/* Focus management */
void wm_focus_set(u32 pid);
u32  wm_focus_get(void);

/*
 * Move a window in the registry and deliver WM_EV_REDRAW to its PID.
 * init calls this at the end of a drag gesture.
 */
void wm_move(int win_id, int x, int y);

/* Query helpers (return -1 / 0 on invalid win_id) */
long wm_get_pos(int win_id);   /* → (x<<32 | (u32)y), signed long */
long wm_get_size(int win_id);  /* → (w<<32 | (u32)h), or 0        */
u32  wm_get_pid(int win_id);   /* → owning pid, or 0              */
const wm_window_t *wm_get_window(int win_id);

/* ── Key event routing ────────────────────────────────────────────────────── */

/* Deliver a key event to the currently focused PID's ring */
void wm_deliver_key(u64 packed_event);

/* Deliver a key event to a specific PID's ring (ignores focused_pid) */
void wm_deliver_to_pid(u32 pid, u64 packed_event);

/* Dequeue one event from pid's ring; returns 0 if empty */
u64  wm_key_dequeue(u32 pid);

/* ── WM event packing ─────────────────────────────────────────────────────── */

/* Build a WM_EV_REDRAW event carrying the new window position */
static inline u64 wm_pack_redraw(int x, int y)
{
    return ((u64)WM_EV_REDRAW << 32) |
           ((u64)((unsigned int)x & 0xFFFFu) << 16) |
           ((u64)((unsigned int)y & 0xFFFFu));
}

#endif /* AETHER_WM_H */
