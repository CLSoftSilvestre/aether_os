/*
 * AetherOS — Window Manager Kernel Interface
 * File: kernel/include/aether/wm.h
 *
 * Phase 4.6: kernel-side window registry + key event routing.
 * wmanager branch: extended for compositing (z-index, opacity, blur, GPU BO).
 *
 * Key event routing:
 *   Hardware kbd IRQ → wm_deliver_key() → focused PID's ring
 *   SYS_WM_KEY_RECV polls own ring, yielding until an event arrives.
 *
 * Compositing extension:
 *   Each window owns a GPU BO (buf_handle) that it renders into.
 *   The compositor process queries all windows via wm_enum() and composites
 *   them back-to-front by z_index onto the final framebuffer each frame.
 *   wm_damage() marks a window dirty and notifies the compositor PID.
 *   wm_damage_clear() is called by the compositor after rendering.
 */

#ifndef AETHER_WM_H
#define AETHER_WM_H

#include "aether/types.h"

/* Maximum number of simultaneously registered windows */
#define WM_MAX_WINDOWS  16

/* Per-process key event ring size (events dropped if full) */
#define WM_KEY_RING     16

/* Reserved z-index for the dock — always above all regular windows */
#define WM_Z_DOCK  32767

/* ── Window behaviour flags (WM9) ────────────────────────────────────────── */

/* Window is system chrome (topbar, dock) — init must not move/resize/close it */
#define WM_FLAG_NO_CHROME  (1u << 0)

/* ── WM event type codes ─────────────────────────────────────────────────── */

/*
 * WM_EV_REDRAW — legacy: delivered to a window's PID when wm_move() runs.
 *   bits [31:16] = new x, bits [15:0] = new y.
 */
#define WM_EV_REDRAW         0xFEu

/*
 * WM_EV_WINDOW_CLOSED — delivered to PID 1 whenever a window unregisters.
 *   bits [63:56]=0xFF  [55:44]=x(12b)  [43:32]=y(12b)  [31:16]=w  [15:0]=h
 */
#define WM_EV_WINDOW_CLOSED  0xFFu

/*
 * WM_EV_DAMAGE — delivered to the registered compositor PID when a window
 * calls wm_damage(), signalling its GPU buffer has been updated.
 *   bits [63:56]=0xF0  bits [55:48]=win_id
 */
#define WM_EV_DAMAGE         0xF0u

/*
 * WM_EV_COMPOSITOR_REQ — compositor asks a window to re-render its buffer.
 *   bits [63:56]=0xF1  bits [55:48]=win_id
 */
#define WM_EV_COMPOSITOR_REQ 0xF1u

/* Focus change notifications */
#define WM_EV_FOCUS_GAINED   0xF2u
#define WM_EV_FOCUS_LOST     0xF3u

/*
 * WM_EV_CLOSE_REQUEST — delivered to the compositor when an app calls
 * sys_wm_request_close().  Window stays registered (active=1, closing=1)
 * so the compositor can animate it out before calling sys_wm_close_done().
 *   bits [63:56]=0xF4  bits [55:48]=win_id
 */
#define WM_EV_CLOSE_REQUEST  0xF4u

/*
 * WM_EV_MINIMIZE — delivered to the compositor when init calls sys_wm_minimize().
 *   Window is still registered (minimized=1, visible=1 during animation).
 *   Compositor plays ANIM_MINIMIZE then calls sys_wm_set_visible(0).
 *   bits [63:56]=0xF5  bits [55:48]=win_id
 */
#define WM_EV_MINIMIZE  0xF5u

/*
 * WM_EV_RESTORE — delivered to the compositor when init calls sys_wm_restore().
 *   Kernel sets minimized=0, visible=1 before sending.
 *   Compositor plays ANIM_OPEN (spring open) for the window.
 *   bits [63:56]=0xF6  bits [55:48]=win_id
 */
#define WM_EV_RESTORE   0xF6u

/*
 * WM_EV_RESIZE — delivered to the window's own PID when init calls sys_wm_resize().
 *   Kernel updates w/h and marks the window damaged before sending.
 *   App should reallocate its GPU BO to new_w×new_h, redraw, then call
 *   sys_wm_set_buffer() + sys_wm_damage() to signal completion.
 *   bits [63:56]=0xF7  bits [55:32]=new_w (24 bits)  bits [31:0]=new_h
 */
#define WM_EV_RESIZE    0xF7u

/* ── Window registry entry ───────────────────────────────────────────────── */

typedef struct {
    u32  pid;           /* owning process */
    int  x, y;         /* top-left corner (screen coordinates) */
    int  w, h;         /* dimensions */
    char title[32];    /* display title */
    int  active;       /* 1 = registered, 0 = free slot */

    /* Compositing properties (wmanager branch) */
    s32  z_index;      /* layer order: higher = closer to user */
    u8   opacity;      /* 0=transparent … 255=opaque (default 255) */
    u8   blur_radius;  /* 0=none; pixel radius for Kawase blur */
    u8   damaged;      /* 1=compositor must re-composite this window */
    u8   visible;      /* 0=skip in composite pass */
    u8   closing;      /* 1=close animation in progress (WM5.5) */
    u8   minimized;    /* 1=minimized to dock (WM7a) */
    u8   flags;        /* WM_FLAG_* bitmask (WM9) */
    u8   _pad;
    u32  buf_handle;   /* GPU BO handle for content buffer (0=none) */
} wm_window_t;

/*
 * wm_entry_t — read-only compositor snapshot filled by wm_enum().
 * Must match layout of wm_entry_t in userspace sys.h.
 */
typedef struct {
    int   win_id;
    u32   pid;
    s32   z_index;
    int   x, y, w, h;
    u8    opacity;
    u8    blur_radius;
    u8    damaged;
    u8    visible;
    u8    closing;     /* 1 = close animation in progress */
    u8    minimized;   /* 1 = minimized to dock (WM7a) */
    u8    flags;       /* WM_FLAG_* bitmask (WM9) */
    u8    _pad;        /* align buf_handle to 4 bytes */
    u32   buf_handle;
    char  title[32];
} wm_entry_t;

/* ── Public API ──────────────────────────────────────────────────────────── */

void wm_init(void);

/* Register a window; returns win_id (0…WM_MAX_WINDOWS-1) or -1 */
int  wm_register(u32 pid, int x, int y, int w, int h, const char *title);

/* Unregister; clears focus if this window owned it */
void wm_unregister(int win_id);
/* force=0: skip closing windows (compositor will call wm_close_done when done)
 * force=1: always unregister regardless (used by task_kill / external kill) */
void wm_unregister_by_pid(u32 pid, int force);

/* Focus */
void wm_focus_set(u32 pid);
u32  wm_focus_get(void);

/* Move a window and deliver WM_EV_REDRAW to its PID */
void wm_move(int win_id, int x, int y);

/* Basic property queries */
long               wm_get_pos(int win_id);    /* → (x<<32|(u32)y) or -1 */
long               wm_get_size(int win_id);   /* → (w<<32|(u32)h) or 0  */
u32                wm_get_pid(int win_id);    /* → owner pid or 0       */
int                wm_is_pid_minimized(u32 pid); /* 1 if pid's window is minimized */
const wm_window_t *wm_get_window(int win_id);

/* ── Compositing API (wmanager branch) ──────────────────────────────────── */

/* Window behaviour flags (WM9) */
void    wm_set_flags(int win_id, u8 flags);
u8      wm_get_flags(int win_id);

/* Layer order and visual properties */
void    wm_set_zindex(int win_id, s32 z);
void    wm_set_opacity(int win_id, u8 opacity);
void    wm_set_blur(int win_id, u8 blur_radius);
void    wm_set_visible(int win_id, u8 visible);
s32     wm_get_zindex(int win_id);

/* GPU buffer association */
void    wm_set_buffer(int win_id, u32 buf_handle);
u32     wm_get_buffer(int win_id);

/*
 * Damage tracking: app calls wm_damage() after writing new pixels into its
 * GPU BO.  Kernel sets the damaged flag and notifies the compositor PID.
 * Compositor calls wm_damage_clear() after compositing the frame.
 */
void    wm_damage(int win_id);
void    wm_damage_clear(int win_id);

/* Register the compositor PID — receives WM_EV_DAMAGE notifications */
void    wm_set_compositor(u32 pid);
u32     wm_get_compositor(void);

/*
 * WM5.5 close-animation protocol.
 * wm_request_close() — marks window closing=1, notifies compositor via
 *   WM_EV_CLOSE_REQUEST.  Window stays registered so compositor can snapshot
 *   and animate its BO before removal.
 * wm_close_done() — called by compositor when animation finishes; internally
 *   calls wm_unregister() to free the slot and BO.
 */
void    wm_request_close(int win_id);
void    wm_close_done(int win_id);

/*
 * WM7a minimize protocol.
 * wm_minimize() — marks minimized=1, notifies compositor via WM_EV_MINIMIZE.
 *   visible stays 1 during the compositor animation; compositor calls
 *   sys_wm_set_visible(0) when ANIM_MINIMIZE settles.
 * wm_restore() — marks minimized=0, visible=1, notifies compositor via
 *   WM_EV_RESTORE; compositor plays ANIM_OPEN.
 */
void    wm_minimize(int win_id);
void    wm_restore(int win_id);

/*
 * WM7b live-resize protocol.
 * wm_resize() — clamps new_w/new_h to minimum (160×120), updates w/h, marks
 *   damaged, delivers WM_EV_RESIZE to the window's PID (app reallocates BO),
 *   and notifies the compositor via WM_EV_DAMAGE (compositor stretches old BO
 *   to the new rect for one frame while the app redraws).
 */
void    wm_resize(int win_id, int new_w, int new_h);

/*
 * wm_enum — fill entries[] with snapshots of all active windows.
 * Returns the count written (≤ max).  Safe to call from compositor.
 */
int     wm_enum(wm_entry_t *entries, int max);

/* ── Key event routing ────────────────────────────────────────────────────── */

void wm_deliver_key(u64 packed_event);
void wm_deliver_to_pid(u32 pid, u64 packed_event);
u64  wm_key_dequeue(u32 pid);

/* ── WM event packing ─────────────────────────────────────────────────────── */

static inline u64 wm_pack_redraw(int x, int y)
{
    return ((u64)WM_EV_REDRAW << 32) |
           ((u64)((unsigned int)x & 0xFFFFu) << 16) |
           ((u64)((unsigned int)y & 0xFFFFu));
}

static inline u64 wm_pack_window_closed(int x, int y, int w, int h)
{
    return ((u64)WM_EV_WINDOW_CLOSED << 56) |
           ((u64)((unsigned int)x & 0xFFFu) << 44) |
           ((u64)((unsigned int)y & 0xFFFu) << 32) |
           ((u64)((unsigned int)w & 0xFFFFu) << 16) |
           ((u64)((unsigned int)h & 0xFFFFu));
}

static inline u64 wm_pack_damage(int win_id)
{
    return ((u64)WM_EV_DAMAGE << 56) |
           ((u64)((unsigned int)win_id & 0xFFu) << 48);
}

static inline u64 wm_pack_compositor_req(int win_id)
{
    return ((u64)WM_EV_COMPOSITOR_REQ << 56) |
           ((u64)((unsigned int)win_id & 0xFFu) << 48);
}

static inline u64 wm_pack_close_request(int win_id)
{
    return ((u64)WM_EV_CLOSE_REQUEST << 56) |
           ((u64)((unsigned int)win_id & 0xFFu) << 48);
}

static inline u64 wm_pack_minimize_event(int win_id)
{
    return ((u64)WM_EV_MINIMIZE << 56) |
           ((u64)((unsigned int)win_id & 0xFFu) << 48);
}

static inline u64 wm_pack_restore_event(int win_id)
{
    return ((u64)WM_EV_RESTORE << 56) |
           ((u64)((unsigned int)win_id & 0xFFu) << 48);
}

/* bits [63:56]=0xF7  [55:32]=new_w (24 bits)  [31:0]=new_h */
static inline u64 wm_pack_resize_event(int new_w, int new_h)
{
    return ((u64)WM_EV_RESIZE << 56) |
           ((u64)((unsigned int)new_w & 0x00FFFFFFu) << 32) |
           ((u64)((unsigned int)new_h & 0xFFFFFFFFu));
}

static inline int wm_ev_resize_w(u64 ev) { return (int)((ev >> 32) & 0x00FFFFFFu); }
static inline int wm_ev_resize_h(u64 ev) { return (int)(ev & 0xFFFFFFFFu); }

static inline u64 wm_pack_focus_event(u8 ev_type, u32 pid)
{
    return ((u64)ev_type << 56) | ((u64)pid & 0x00FFFFFFFFFFFFFFuLL);
}

#endif /* AETHER_WM_H */
