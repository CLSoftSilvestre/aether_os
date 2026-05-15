/*
 * AetherOS — Lumina Compositing Window Manager
 * File: userspace/apps/compositor/main.c
 *
 * Architecture:
 *   - Runs as a userspace daemon spawned by init at boot.
 *   - Registers itself as the compositor via SYS_WM_SET_COMPOSITOR.
 *   - Kernel delivers WM_EV_DAMAGE events to this PID whenever any window
 *     updates its GPU buffer and calls sys_wm_damage().
 *   - Main loop: vsync-paced at 60 fps; skips frames with no damage.
 *   - Composites GPU-BO-backed windows (buf_handle > 0) in z-index order.
 *   - Legacy windows (no buf_handle) are rendered by their own processes
 *     directly to the framebuffer; the compositor does not touch them.
 *
 * Phase WM2 (CPU compositor — compile WITHOUT -DAETHER_GPU_COMPOSITOR):
 *   Per-window round trip: sys_wp_blend_fill + sys_gpu_blit + sys_wm_damage_clear.
 *   Transparent windows get their background filled before blending.
 *   Blur (blur_radius > 0) is noted but deferred to Phase WM4.
 *
 * Phase WM3 (batch GPU compositor — compile WITH -DAETHER_GPU_COMPOSITOR):
 *   Two syscalls per frame regardless of window count:
 *     1. sys_gpu_composite_frame(layers, n) — kernel fills back buffer with
 *        wallpaper background then alpha-blends all layers in one shot.
 *     2. sys_fb_flip() — present back buffer → framebuffer (zero-copy on Pi 4,
 *        word-copy fallback on QEMU).
 *   Reduces N×3 syscalls to 2 per frame and eliminates per-window round-trip.
 *
 * Phase WM4 (Dual Kawase blur) and WM5 (spring animations) will extend this
 * file with further paths guarded by their own compile-time flags.
 */

#include <sys.h>
#include <string.h>

/* Maximum simultaneous windows (matches WM_MAX_WINDOWS in kernel wm.h) */
#define COMPOSITOR_MAX_WINDOWS  16

/* Lumina dark background colour (fallback when no wallpaper registered).
 * Matches LUMINA_BG_COLOR in kernel/drivers/gpu/v3d.c. */
#define LUMINA_BG  0x0d1117u

/* ── Helpers ─────────────────────────────────────────────────────────────── */

static inline unsigned int wm_ev_type(unsigned long long ev)
{
    return (unsigned int)(ev >> 56);
}

/* Insertion sort — stable, ascending z_index. N ≤ 16 so O(N²) is fine. */
static void sort_layers(wm_entry_t *e, int n)
{
    for (int i = 1; i < n; i++) {
        wm_entry_t key = e[i];
        int j = i - 1;
        while (j >= 0 && e[j].z_index > key.z_index) {
            e[j + 1] = e[j];
            j--;
        }
        e[j + 1] = key;
    }
}

/* ── WM5: Spring animation state ─────────────────────────────────────────── */

#ifdef AETHER_SPRING_ANIM

/*
 * Spring constants at 60 Hz, Q12 fixed-point (4096 = 1.0).
 * Continuous spring model: a = -k*(x - x0) - d*v
 * Discrete Euler step:     v += (K_Q12 * err) >> 12
 *                          v -= (D_Q12 * v)   >> 12
 *                          x += v
 *
 * K_Q12 = k / 60² × 4096  where k = 280 /s²  →  319
 * D_Q12 = d / 60  × 4096  where d =  28 /s   → 1912
 */
#define SPRING_K_Q12    319
#define SPRING_D_Q12   1912
#define SCALE_ONE_Q12  4096    /* 1.0 — natural size */
#define SCALE_OPEN_Q12 3482    /* 0.85 — open animation start */

typedef enum { ANIM_IDLE = 0, ANIM_OPEN, ANIM_CLOSE, ANIM_MINIMIZE } anim_phase_t;

typedef struct {
    int          win_id;
    anim_phase_t phase;
    int          scale_q12,   scale_v;
    int          opacity_q12, opacity_v;
} wm_anim_t;

static wm_anim_t g_anims[COMPOSITOR_MAX_WINDOWS];   /* zero-init → ANIM_IDLE */
static int       g_prev_ids[COMPOSITOR_MAX_WINDOWS];
static int       g_prev_n;
static int       g_has_active_anims;
static int       g_settled_this_frame; /* set when a window finishes ANIM_MINIMIZE */

static void spring_step(int *x, int *v, int target)
{
    int err = target - *x;
    *v += (SPRING_K_Q12 * err) >> 12;
    *v -= (SPRING_D_Q12 * (*v)) >> 12;
    *x += *v;
    if (*x < 0)                    *x = 0;
    if (*x > SCALE_ONE_Q12 * 2)   *x = SCALE_ONE_Q12 * 2;
}

static wm_anim_t *anim_find(int win_id)
{
    for (int i = 0; i < COMPOSITOR_MAX_WINDOWS; i++)
        if (g_anims[i].phase != ANIM_IDLE && g_anims[i].win_id == win_id)
            return &g_anims[i];
    return (wm_anim_t *)0;
}

static wm_anim_t *anim_alloc(int win_id)
{
    for (int i = 0; i < COMPOSITOR_MAX_WINDOWS; i++) {
        if (g_anims[i].phase == ANIM_IDLE) {
            g_anims[i].win_id = win_id;
            return &g_anims[i];
        }
    }
    return (wm_anim_t *)0;
}

static void anim_free(int win_id)
{
    for (int i = 0; i < COMPOSITOR_MAX_WINDOWS; i++)
        if (g_anims[i].win_id == win_id)
            g_anims[i].phase = ANIM_IDLE;
}

#endif /* AETHER_SPRING_ANIM */

/* ── WM7b: per-window BO dimension tracking ──────────────────────────────── */
/*
 * Tracks the actual GPU BO dimensions (bo_w × bo_h) per window, decoupled
 * from the kernel-registered w/h.  This allows the compositor to bilinear-
 * stretch the old BO to the new display rect for exactly one frame after a
 * resize call, while the app reallocates its BO in the background.
 *
 * Reallocation is detected by checking whether buf_handle changed since
 * the last frame.  When it changes, bo_w/bo_h are updated to match e->w/h
 * (the new BO now has the new dimensions).
 */
typedef struct {
    int          in_use;
    int          win_id;
    unsigned int buf_handle;   /* last known handle; change = BO reallocated */
    int          bo_w, bo_h;   /* actual BO dimensions */
} wm_win_state_t;

static wm_win_state_t g_win_state[COMPOSITOR_MAX_WINDOWS]; /* zero-init */

static wm_win_state_t *win_state_find(int win_id)
{
    for (int i = 0; i < COMPOSITOR_MAX_WINDOWS; i++)
        if (g_win_state[i].in_use && g_win_state[i].win_id == win_id)
            return &g_win_state[i];
    return (wm_win_state_t *)0;
}

static wm_win_state_t *win_state_alloc(int win_id)
{
    for (int i = 0; i < COMPOSITOR_MAX_WINDOWS; i++) {
        if (!g_win_state[i].in_use) {
            g_win_state[i].in_use = 1;
            g_win_state[i].win_id = win_id;
            return &g_win_state[i];
        }
    }
    return (wm_win_state_t *)0;
}

static void win_state_free(int win_id)
{
    for (int i = 0; i < COMPOSITOR_MAX_WINDOWS; i++)
        if (g_win_state[i].in_use && g_win_state[i].win_id == win_id) {
            g_win_state[i].in_use = 0;
            return;
        }
}

/* ── Compositor frame — Phase WM3 batch GPU path ─────────────────────────── */

#ifdef AETHER_GPU_COMPOSITOR

/*
 * composite_frame — WM3 batch path (WM5: spring animation applied when
 * AETHER_SPRING_ANIM is defined).
 *
 * Detects newly-appeared windows each frame and runs a spring-physics
 * open animation (scale 0.85→1.0, opacity 0→1) guarded by AETHER_SPRING_ANIM.
 * All arithmetic uses Q12 fixed-point (4096 = 1.0) to avoid FP state issues
 * across context switches that don't save NEON/FP registers.
 */
static void composite_frame(void)
{
    wm_entry_t entries[COMPOSITOR_MAX_WINDOWS];
    int n = sys_wm_enum(entries, COMPOSITOR_MAX_WINDOWS);
    if (n > 0) sort_layers(entries, n);

#ifdef AETHER_SPRING_ANIM
    /* ── Detect window birth / death, update anim state ── */
    int curr_ids[COMPOSITOR_MAX_WINDOWS];
    for (int i = 0; i < n; i++)
        curr_ids[i] = entries[i].win_id;

    /* New windows → allocate an ANIM_OPEN slot (skip closing windows) */
    for (int i = 0; i < n; i++) {
        if (entries[i].closing) continue;
        int seen = 0;
        for (int j = 0; j < g_prev_n; j++)
            if (g_prev_ids[j] == curr_ids[i]) { seen = 1; break; }
        if (!seen) {
            wm_anim_t *a = anim_alloc(curr_ids[i]);
            if (a) {
                a->phase       = ANIM_OPEN;
                a->scale_q12   = SCALE_OPEN_Q12;
                a->scale_v     = 0;
                a->opacity_q12 = 0;
                a->opacity_v   = 0;
            }
        }
    }
    /* Disappeared windows → free their slot */
    for (int j = 0; j < g_prev_n; j++) {
        int seen = 0;
        for (int i = 0; i < n; i++)
            if (curr_ids[i] == g_prev_ids[j]) { seen = 1; break; }
        if (!seen) {
            anim_free(g_prev_ids[j]);
            win_state_free(g_prev_ids[j]);
        }
    }
    for (int i = 0; i < n; i++)
        g_prev_ids[i] = curr_ids[i];
    g_prev_n = n;
    g_has_active_anims   = 0;
    g_settled_this_frame = 0;
#endif /* AETHER_SPRING_ANIM */

    v3d_layer_t layers[COMPOSITOR_MAX_WINDOWS];
    int out = 0;

    for (int i = 0; i < n; i++) {
        wm_entry_t *e = &entries[i];
        if (!e->visible || !e->buf_handle) continue;

        int dst_x = e->x, dst_y = e->y;
        int dst_w = e->w, dst_h = e->h;
        unsigned char eff_opacity = e->opacity;

#ifdef AETHER_SPRING_ANIM
        wm_anim_t *a = anim_find(e->win_id);
        if (a) {
            /* ANIM_OPEN/RESTORE: spring toward natural size + full opacity.
             * ANIM_CLOSE:        spring toward 85% scale + zero opacity → unregister.
             * ANIM_MINIMIZE:     spring toward zero scale + zero opacity → hide. */
            int target_scale, target_opacity;
            if (a->phase == ANIM_CLOSE) {
                target_scale   = SCALE_OPEN_Q12;
                target_opacity = 0;
            } else if (a->phase == ANIM_MINIMIZE) {
                target_scale   = 0;
                target_opacity = 0;
            } else {
                target_scale   = SCALE_ONE_Q12;
                target_opacity = SCALE_ONE_Q12;
            }

            spring_step(&a->scale_q12,   &a->scale_v,   target_scale);
            spring_step(&a->opacity_q12, &a->opacity_v, target_opacity);

            /* Scale from center */
            dst_w = ((int)e->w * a->scale_q12) >> 12;
            dst_h = ((int)e->h * a->scale_q12) >> 12;
            if (dst_w < 1) dst_w = 1;
            if (dst_h < 1) dst_h = 1;
            dst_x = e->x + (e->w - dst_w) / 2;
            dst_y = e->y + (e->h - dst_h) / 2;

            int op = ((int)e->opacity * a->opacity_q12) >> 12;
            eff_opacity = (op > 255) ? (unsigned char)255 : (unsigned char)op;

            if (a->phase == ANIM_CLOSE) {
                /* Settle: opacity low enough — trigger kernel unregister */
                if (a->opacity_q12 <= 8) {
                    a->phase = ANIM_IDLE;
                    sys_wm_close_done(e->win_id);
                    continue;
                } else {
                    g_has_active_anims = 1;
                }
            } else if (a->phase == ANIM_MINIMIZE) {
                /* Settle: opacity low enough — hide window from compositor. */
                if (a->opacity_q12 <= 8) {
                    a->phase = ANIM_IDLE;
                    sys_wm_set_visible(e->win_id, 0);
                    g_settled_this_frame = 1;
                    eff_opacity = 0;
                    /* Fall through — add layer with opacity=0 so out >= 1,
                     * guaranteeing sys_gpu_composite_frame + sys_fb_flip fire
                     * this frame and clear the window from the front buffer. */
                } else {
                    g_has_active_anims = 1;
                }
            } else {
                /* ANIM_OPEN settle: snap when spring is within 1/512 of target */
                int se = a->scale_q12   - SCALE_ONE_Q12; if (se < 0) se = -se;
                int oe = a->opacity_q12 - SCALE_ONE_Q12; if (oe < 0) oe = -oe;
                int sv = a->scale_v;                      if (sv < 0) sv = -sv;
                int ov = a->opacity_v;                    if (ov < 0) ov = -ov;
                if (se <= 8 && oe <= 8 && sv <= 4 && ov <= 4) {
                    a->phase = ANIM_IDLE;
                    dst_x = e->x; dst_y = e->y;
                    dst_w = e->w; dst_h = e->h;
                    eff_opacity = e->opacity;
                } else {
                    g_has_active_anims = 1;
                }
            }
        }
#endif /* AETHER_SPRING_ANIM */

        /* WM7b: look up (or create) per-window BO state.
         * If buf_handle changed since last frame the app reallocated its BO
         * at the new size — update bo_w/h.  Otherwise keep the previous BO
         * size as src_w/h so the compositor bilinear-stretches the old BO
         * into the new display rect for the one frame before the app redraws. */
        wm_win_state_t *ws = win_state_find(e->win_id);
        if (!ws) {
            ws = win_state_alloc(e->win_id);
            if (ws) {
                ws->buf_handle = e->buf_handle;
                ws->bo_w       = e->w;
                ws->bo_h       = e->h;
            }
        } else if (ws->buf_handle != e->buf_handle) {
            ws->buf_handle = e->buf_handle;
            ws->bo_w       = e->w;
            ws->bo_h       = e->h;
        }
        int src_w = ws ? ws->bo_w : e->w;
        int src_h = ws ? ws->bo_h : e->h;
        if (src_w < 1) src_w = 1;
        if (src_h < 1) src_h = 1;

        layers[out].bo_handle   = e->buf_handle;
        layers[out].src_w       = (unsigned)src_w;
        layers[out].src_h       = (unsigned)src_h;
        layers[out].dst_x       = dst_x;
        layers[out].dst_y       = dst_y;
        layers[out].dst_w       = dst_w;
        layers[out].dst_h       = dst_h;
        layers[out].opacity     = eff_opacity;
        layers[out].blur_radius = e->blur_radius;
        layers[out].flags       = 0;
        layers[out]._pad        = 0;
        out++;

        /* Clear damage before kernel renders so the app can begin its next
         * frame immediately without waiting for the flip. */
        sys_wm_damage_clear(e->win_id);
    }

    /* Only composite + flip when there are GPU-BO windows to render.
     * Exception: if a window settled its minimize animation this frame
     * (g_settled_this_frame), we must still flip once to clear it from
     * the screen.  Without the flip, stale front-buffer pixels persist. */
    if (out == 0 && !g_settled_this_frame) return;

    sys_gpu_composite_frame(layers, out);
    sys_fb_flip();
}

#else /* ── Phase WM2 per-window CPU path ───────────────────────────────── */

/*
 * composite_frame — WM2 fallback path.
 *
 * For each GPU-BO-backed window: optionally fill wallpaper background (for
 * transparent windows), blit the BO, then clear the damage flag.
 * This path is active when AETHER_GPU_COMPOSITOR is not defined.
 */
static void composite_frame(void)
{
    wm_entry_t layers[COMPOSITOR_MAX_WINDOWS];
    int n = sys_wm_enum(layers, COMPOSITOR_MAX_WINDOWS);
    if (n <= 0) return;

    sort_layers(layers, n);

    for (int i = 0; i < n; i++) {
        wm_entry_t *e = &layers[i];
        if (!e->visible || !e->buf_handle) continue;

        /*
         * Transparent window: paint the wallpaper (or Lumina background if no
         * wallpaper is registered) into this window's rect before blending.
         * Phase WM4 will replace this with the Dual Kawase blur path.
         */
        if (e->opacity < 255 || e->blur_radius > 0)
            sys_wp_blend_fill((unsigned)e->x, (unsigned)e->y,
                              (unsigned)e->w, (unsigned)e->h, LUMINA_BG);

        /*
         * Blit the window's GPU BO onto the framebuffer at its screen rect.
         * sys_gpu_blit: bilinear scale + alpha blend (alpha = e->opacity).
         * For opaque windows (opacity=255) this is a fast direct copy.
         */
        sys_gpu_blit(e->buf_handle,
                     (unsigned)e->w, (unsigned)e->h,
                     (unsigned)e->x, (unsigned)e->y,
                     (unsigned)e->w, (unsigned)e->h,
                     e->opacity);

        /* Clear damage flag — compositor has rendered this frame */
        sys_wm_damage_clear(e->win_id);
    }
}

#endif /* AETHER_GPU_COMPOSITOR */

/* ── Main ────────────────────────────────────────────────────────────────── */

int main(void)
{
    long my_pid = sys_getpid();

    /* Register this process as the compositor.
     * From this point, any window calling sys_wm_damage() delivers a
     * WM_EV_DAMAGE event to our event ring. */
    sys_wm_set_compositor(my_pid);

    /* Claim the framebuffer: disables the kernel's fb_console output
     * so we have clean ownership of every pixel.
     * Side effect: triggers v3d_dbl_init() in the kernel — allocates the
     * double-buffer back buffer used by SYS_GPU_COMPOSITE_FRAME. */
    sys_fb_claim();

    /*
     * Main loop — vsync-paced at 60 fps.
     *
     * Strategy:
     *   1. Wait for next vertical sync (16.67 ms deadline).
     *   2. Drain all pending WM events:
     *      - WM_EV_DAMAGE events set a flag — we need to composite.
     *      - Other events (keys, etc.) are routed by the kernel; draining
     *        here ensures hardware keyboard events reach focused apps even
     *        if those apps are blocked waiting for input.
     *   3. If any damage: composite all GPU-BO-backed windows in z order.
     */
    for (;;) {
        sys_vsync_wait();

        int needs_composite = 0;
        unsigned long long ev;

        /* Drain event ring — sys_wm_event_poll also routes kbd hardware
         * events to the focused PID as a side effect. */
        while ((ev = sys_wm_event_poll()) != 0) {
            if (wm_ev_type(ev) == WM_EV_DAMAGE)
                needs_composite = 1;
#ifdef AETHER_SPRING_ANIM
            else if (wm_ev_type(ev) == WM_EV_CLOSE_REQUEST) {
                int wid = wm_event_win_id(ev);

                /* Check whether this window has a GPU BO (can be animated).
                 * Legacy windows (buf_handle=0) draw to the framebuffer directly;
                 * there is nothing for us to composite, so close immediately. */
                wm_entry_t snap[COMPOSITOR_MAX_WINDOWS];
                int ns = sys_wm_enum(snap, COMPOSITOR_MAX_WINDOWS);
                int has_bo = 0;
                for (int k = 0; k < ns; k++) {
                    if (snap[k].win_id == wid) {
                        has_bo = (snap[k].buf_handle != 0);
                        break;
                    }
                }

                if (!has_bo) {
                    /* Legacy window: skip animation, complete close immediately.
                     * sys_wm_close_done() unregisters the window and kills the app. */
                    sys_wm_close_done(wid);
                } else {
                    /* GPU-BO window: set up spring close animation. */
                    wm_anim_t *a = anim_find(wid);
                    if (!a) a = anim_alloc(wid);
                    if (a) {
                        if (a->phase != ANIM_CLOSE) {
                            if (a->phase == ANIM_IDLE) {
                                a->scale_q12   = SCALE_ONE_Q12;
                                a->opacity_q12 = SCALE_ONE_Q12;
                                a->scale_v     = 0;
                                a->opacity_v   = 0;
                            }
                            a->phase = ANIM_CLOSE;
                        }
                    }
                    needs_composite = 1;
                }
            } else if (wm_ev_type(ev) == WM_EV_MINIMIZE) {
                int wid = wm_event_win_id(ev);
                /* Check for GPU BO. Legacy windows (no BO) can't be animated;
                 * hide them immediately from the compositor's scene. */
                wm_entry_t snap2[COMPOSITOR_MAX_WINDOWS];
                int ns2 = sys_wm_enum(snap2, COMPOSITOR_MAX_WINDOWS);
                int has_bo2 = 0;
                for (int k = 0; k < ns2; k++) {
                    if (snap2[k].win_id == wid) {
                        has_bo2 = (snap2[k].buf_handle != 0);
                        break;
                    }
                }
                if (has_bo2) {
                    /* GPU-BO window: spring scale 1→0, opacity 1→0 */
                    wm_anim_t *a = anim_find(wid);
                    if (!a) a = anim_alloc(wid);
                    if (a && a->phase != ANIM_MINIMIZE) {
                        if (a->phase == ANIM_IDLE) {
                            a->scale_q12   = SCALE_ONE_Q12;
                            a->opacity_q12 = SCALE_ONE_Q12;
                            a->scale_v     = 0;
                            a->opacity_v   = 0;
                        }
                        a->phase = ANIM_MINIMIZE;
                    }
                    needs_composite = 1;
                } else {
                    /* Legacy window: hide immediately (no BO to animate) */
                    sys_wm_set_visible(wid, 0);
                }
            } else if (wm_ev_type(ev) == WM_EV_RESTORE) {
                int wid = wm_event_win_id(ev);
                /* Kernel already set visible=1.  Play ANIM_OPEN spring.
                 * We explicitly start ANIM_OPEN here because the window was already
                 * in g_prev_ids (registered but hidden), so birth detection won't fire. */
                wm_anim_t *a = anim_find(wid);
                if (!a) a = anim_alloc(wid);
                if (a) {
                    a->phase       = ANIM_OPEN;
                    a->scale_q12   = SCALE_OPEN_Q12;
                    a->scale_v     = 0;
                    a->opacity_q12 = 0;
                    a->opacity_v   = 0;
                }
                needs_composite = 1;
            } else if (wm_ev_type(ev) == WM_EV_RESIZE) {
                /* WM7b: kernel already updated w/h and sent WM_EV_DAMAGE.
                 * The DAMAGE event sets needs_composite=1 separately.
                 * This branch exists for completeness and future per-event hooks. */
                needs_composite = 1;
            }
#endif
        }

#ifdef AETHER_SPRING_ANIM
        /* Drive animation frames even when no damage event arrives */
        if (g_has_active_anims)
            needs_composite = 1;
#endif

        if (needs_composite)
            composite_frame();
    }

    return 0;
}
