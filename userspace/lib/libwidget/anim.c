/*
 * AetherOS libwidget — Spring animations (Phase 6.1.7)
 * File: userspace/lib/libwidget/anim.c
 *
 * Implements damped-spring physics for window open/close/dock-bounce
 * animations at 60 fps.  Each animation allocates one GPU BO to hold a
 * snapshot of the window content at the moment the animation starts;
 * per-frame compositing writes the scaled+alpha-blended result directly
 * to the framebuffer via SYS_GPU_BLIT.
 *
 * Architecture notes:
 *   - No heap allocation beyond a single PMM-backed GPU BO per animation.
 *   - All math uses float (userspace has -mgeneral-regs-only stripped, so
 *     AArch64 VFP is available).
 *   - win_anim_tick() calls sys_vsync_wait() before each step, pacing the
 *     animation loop to the display's 60 Hz vblank boundary.
 *   - widget_run() polls anim_any_active() and uses sys_vsync_wait() instead
 *     of sys_sched_yield() while any animation is active.
 */

#include <widget.h>
#include <gpu.h>
#include <sys.h>
#include <stdlib.h>   /* NULL */

/* ── Active animation counter ────────────────────────────────────────────── */

static int s_anim_count = 0;

int anim_any_active(void) { return s_anim_count > 0; }

/* ── Spring physics ──────────────────────────────────────────────────────── */

void spring_init(spring_interp_t *s, float pos, float target, float k, float d)
{
    s->pos    = pos;
    s->vel    = 0.0f;
    s->target = target;
    s->k      = k;
    s->d      = d;
}

void spring_set_target(spring_interp_t *s, float target)
{
    s->target = target;
}

void spring_step(spring_interp_t *s, float dt)
{
    float force = -s->k * (s->pos - s->target) - s->d * s->vel;
    s->vel += force * dt;
    s->pos += s->vel * dt;
}

int spring_settled(const spring_interp_t *s)
{
    float dp = s->pos - s->target;
    if (dp < 0.0f) dp = -dp;
    float v = s->vel < 0.0f ? -s->vel : s->vel;
    return (dp < 0.005f && v < 0.05f);
}

/* ── Window open animation ───────────────────────────────────────────────── */

int win_anim_open(win_anim_t *a, int wx, int wy, int ww, int wh)
{
    a->active      = 0;
    a->is_open     = 1;
    a->win_x       = wx;
    a->win_y       = wy;
    a->win_w       = ww;
    a->win_h       = wh;
    a->content_bo  = GPU_BO_INVALID;

    /* Allocate a BO large enough to hold the window pixels */
    a->content_bo = gpu_alloc((unsigned)(ww * wh * 4));
    if (a->content_bo == GPU_BO_INVALID) return -1;

    /* Capture the already-drawn window into the BO */
    if (sys_fb_capture(a->content_bo, (unsigned)wx, (unsigned)wy,
                       (unsigned)ww, (unsigned)wh) < 0) {
        gpu_free(a->content_bo);
        a->content_bo = GPU_BO_INVALID;
        return -1;
    }

    /* Springs: open from small+transparent to full+opaque
     * k=600, d=35 → ~120ms settle at 60fps                */
    spring_init(&a->scale_sp, 0.85f, 1.0f, 600.0f, 35.0f);
    spring_init(&a->alpha_sp, 0.0f,  1.0f, 600.0f, 35.0f);

    a->active = 1;
    s_anim_count++;
    return 0;
}

/* ── Window close animation ──────────────────────────────────────────────── */

int win_anim_close(win_anim_t *a, int wx, int wy, int ww, int wh)
{
    a->active      = 0;
    a->is_open     = 0;
    a->win_x       = wx;
    a->win_y       = wy;
    a->win_w       = ww;
    a->win_h       = wh;
    a->content_bo  = GPU_BO_INVALID;

    a->content_bo = gpu_alloc((unsigned)(ww * wh * 4));
    if (a->content_bo == GPU_BO_INVALID) return -1;

    if (sys_fb_capture(a->content_bo, (unsigned)wx, (unsigned)wy,
                       (unsigned)ww, (unsigned)wh) < 0) {
        gpu_free(a->content_bo);
        a->content_bo = GPU_BO_INVALID;
        return -1;
    }

    /* Springs: close from full+opaque to small+transparent
     * k=800, d=50 → ~80ms settle at 60fps (snappier close) */
    spring_init(&a->scale_sp, 1.0f, 0.85f, 800.0f, 50.0f);
    spring_init(&a->alpha_sp, 1.0f, 0.0f,  800.0f, 50.0f);

    a->active = 1;
    s_anim_count++;
    return 0;
}

/* ── win_anim_tick ───────────────────────────────────────────────────────── */

int win_anim_tick(win_anim_t *a)
{
    if (!a->active) return 0;

    /* Block until next vblank (~16.67ms) for consistent 60fps cadence */
    sys_vsync_wait();

    /* Advance springs one frame */
    spring_step(&a->scale_sp, 1.0f / 60.0f);
    spring_step(&a->alpha_sp, 1.0f / 60.0f);

    /* Clamp scale to visible range */
    float scale = a->scale_sp.pos;
    if (scale < 0.01f) scale = 0.01f;
    if (scale > 1.0f)  scale = 1.0f;

    int anim_w = (int)((float)a->win_w * scale);
    int anim_h = (int)((float)a->win_h * scale);
    if (anim_w < 1) anim_w = 1;
    if (anim_h < 1) anim_h = 1;

    /* Centre the scaled window on the natural window position */
    int anim_x = a->win_x + (a->win_w - anim_w) / 2;
    int anim_y = a->win_y + (a->win_h - anim_h) / 2;

    unsigned alpha_u = (unsigned)(a->alpha_sp.pos * 255.0f);
    if (alpha_u > 255u) alpha_u = 255u;

    /*
     * Single-pass composite: restore wallpaper bg over the natural window rect
     * AND scale+alpha-blend the BO into the animated rect in one kernel call.
     * This writes each FB pixel exactly once — no intermediate blank frame.
     */
    sys_composite_anim(a->content_bo,
                       (unsigned)a->win_x, (unsigned)a->win_y,
                       (unsigned)a->win_w, (unsigned)a->win_h,
                       (unsigned)anim_x,   (unsigned)anim_y,
                       (unsigned)anim_w,   (unsigned)anim_h,
                       (unsigned char)alpha_u);

    /* Check settlement */
    if (spring_settled(&a->scale_sp) && spring_settled(&a->alpha_sp)) {
        a->active = 0;
        s_anim_count--;
        return 0;
    }
    return 1;
}

/* ── win_anim_free ───────────────────────────────────────────────────────── */

void win_anim_free(win_anim_t *a)
{
    if (a->active) {
        a->active = 0;
        s_anim_count--;
    }
    if (a->content_bo != GPU_BO_INVALID) {
        gpu_free(a->content_bo);
        a->content_bo = GPU_BO_INVALID;
    }
}

/* ── Dock bounce animation ───────────────────────────────────────────────── */

/*
 * dock_anim_start — launch a bounce animation for a dock/taskbar icon.
 * The spring drives scale from 1.0 up to 1.3 then back to 1.0.
 * Caller re-renders the icon each tick using a->scale_sp.pos.
 */
void dock_anim_start(dock_anim_t *a, int ix, int iy, int icon_size)
{
    a->icon_x    = ix;
    a->icon_y    = iy;
    a->icon_size = icon_size;

    /* Shoot spring to 1.3 first; redirect to 1.0 once it peaks */
    spring_init(&a->scale_sp, 1.0f, 1.3f, 400.0f, 20.0f);

    a->active = 1;
    s_anim_count++;
}

/*
 * dock_anim_tick — advance the bounce animation by one vsync frame.
 * Returns 1 if still running, 0 when settled.
 * After each call read a->scale_sp.pos to know the current icon scale.
 */
int dock_anim_tick(dock_anim_t *a)
{
    if (!a->active) return 0;

    spring_step(&a->scale_sp, 1.0f / 60.0f);

    /* Once the spring overshoots the peak and is heading back,
     * redirect it to settle at 1.0 (natural size). */
    if (a->scale_sp.target > 1.0f && a->scale_sp.vel < 0.0f)
        spring_set_target(&a->scale_sp, 1.0f);

    if (spring_settled(&a->scale_sp)) {
        a->scale_sp.pos = 1.0f;
        a->scale_sp.vel = 0.0f;
        a->active = 0;
        s_anim_count--;
        return 0;
    }
    return 1;
}
