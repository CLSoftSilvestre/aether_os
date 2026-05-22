/*
 * AetherOS — libAetherAudioWidget: rotary knob widget (Phase 8.9)
 * File: userspace/lib/libAetherAudioWidget/src/awgt_knob.c
 *
 * Davies 1900H-inspired: dark body, outer silver ring, white tick line.
 * Sweep: 270° from lower-left (value=0) CW to lower-right (value=1).
 *
 * Screen-space angle convention: 0° = right, positive = clockwise.
 * Start: 135° (lower-left), End: 135°+270° = 405° = 45° (lower-right).
 */

#include "awgt.h"
#include "awgt_prims.h"
#include <gfx.h>
#include <string.h>
#include <math.h>

/* ── Colours ─────────────────────────────────────────────────────────── */

#define C_KNOB_RIM      GFX_RGB( 90,  90, 105)
#define C_KNOB_RIM_HI   GFX_RGB(130, 130, 150)
#define C_KNOB_BODY     GFX_RGB( 42,  42,  55)
#define C_KNOB_TICK     GFX_RGB(240, 240, 255)
#define C_KNOB_MARK     GFX_RGB( 80,  80, 100)   /* arc tick marks */
#define C_KNOB_FLAT_BG  GFX_RGB( 50,  50, 140)   /* flat style body */
#define C_KNOB_LABEL    GFX_RGB(160, 160, 180)

/* ── Angle math ──────────────────────────────────────────────────────── */

#define AWF_PI     3.14159265358979f
#define DEG2RAD(d) ((d) * AWF_PI / 180.0f)

/* Start angle (lower-left in screen coords), sweep clockwise 270°. */
#define KNOB_START_RAD  DEG2RAD(135.0f)
#define KNOB_SWEEP_RAD  DEG2RAD(270.0f)

static void knob_tick_xy(float angle, int cx, int cy, int r,
                          int *ox, int *oy)
{
    *ox = cx + (int)(cosf(angle) * (float)r);
    *oy = cy + (int)(sinf(angle) * (float)r);
}

/* ── Init ────────────────────────────────────────────────────────────── */

void awgt_knob_init(awgt_knob_t *k, int x, int y, int size,
                    int style, const char *label)
{
    memset(k, 0, sizeof(*k));
    k->x = x; k->y = y; k->size = size;
    k->style = style;
    k->label = label;
    k->value = 0.5f;
}

/* ── Draw ────────────────────────────────────────────────────────────── */

void awgt_knob_draw(const awgt_knob_t *k)
{
    int r    = k->size / 2;
    int cx   = k->x + r;
    int cy   = k->y + r;

    /* ── Outer rim (silver ring, 2 px wide) ── */
    awgt_fill_circle(cx, cy, r, C_KNOB_RIM);
    /* top-left specular highlight on rim */
    int hr   = r - 1;
    for (int dy = -hr; dy <= 0; dy++) {
        int dx = 0;
        /* only left half */
        while ((dx + 1) * (dx + 1) + dy * dy <= hr * hr) dx++;
        gfx_hline((unsigned)(cx - dx), (unsigned)(cy + dy),
                  (unsigned)dx, C_KNOB_RIM_HI);
    }

    /* ── Body fill ── */
    int br = r - 2;
    unsigned body_color = (k->style == AWGT_KNOB_STYLE_FLAT)
                          ? C_KNOB_FLAT_BG : C_KNOB_BODY;
    awgt_fill_circle(cx, cy, br, body_color);

    /* ── Tick marks around the arc ── */
    int tr_outer = r - 1;
    int tr_inner = br - 1;
    for (int t = 0; t <= 10; t++) {
        float frac  = (float)t / 10.0f;
        float angle = KNOB_START_RAD + frac * KNOB_SWEEP_RAD;
        int ox, oy, ix, iy;
        int tick_r_out = tr_outer;
        int tick_r_in  = (t == 0 || t == 5 || t == 10)
                         ? tr_inner - 2 : tr_inner + 1;
        knob_tick_xy(angle, cx, cy, tick_r_out, &ox, &oy);
        knob_tick_xy(angle, cx, cy, tick_r_in,  &ix, &iy);
        awgt_line(ox, oy, ix, iy, C_KNOB_MARK);
    }

    /* ── Indicator line (white tick from body-edge to half-radius) ── */
    float ind_angle = KNOB_START_RAD + k->value * KNOB_SWEEP_RAD;
    int tick_outer, tick_inner;
    if (k->style == AWGT_KNOB_STYLE_POINTER) {
        /* pointer goes to center */
        tick_outer = br;
        tick_inner = 2;
    } else {
        tick_outer = br;
        tick_inner = br / 2;
    }
    int tx0, ty0, tx1, ty1;
    knob_tick_xy(ind_angle, cx, cy, tick_outer, &tx0, &ty0);
    knob_tick_xy(ind_angle, cx, cy, tick_inner, &tx1, &ty1);
    awgt_line_thick(tx0, ty0, tx1, ty1, 2, C_KNOB_TICK);

    /* Dot at tip for DAVIES style */
    if (k->style == AWGT_KNOB_STYLE_DAVIES) {
        int dx, dy2;
        knob_tick_xy(ind_angle, cx, cy, br - 2, &dx, &dy2);
        awgt_fill_circle(dx, dy2, 2, C_KNOB_TICK);
    }

    /* ── Label ── */
    if (k->label) {
        int lx = k->x + r - (int)(strlen(k->label) * 4);
        int ly = k->y + k->size + 3;
        gfx_text((unsigned)lx, (unsigned)ly, k->label,
                 C_KNOB_LABEL, 0);
    }
}

/* ── Input ───────────────────────────────────────────────────────────── */

int awgt_knob_mouse_down(awgt_knob_t *k, int mx, int my)
{
    /* hit-test bounding circle */
    int r  = k->size / 2;
    int cx = k->x + r, cy = k->y + r;
    int dx = mx - cx, dy = my - cy;
    if (dx * dx + dy * dy > r * r) return 0;

    k->dragging       = 1;
    k->drag_start_y   = my;
    k->drag_start_val = k->value;
    return 1;
}

int awgt_knob_mouse_move(awgt_knob_t *k, int mx, int my)
{
    (void)mx;
    if (!k->dragging) return 0;
    /* 200 pixels of vertical drag = full range */
    float delta = (float)(k->drag_start_y - my) / 200.0f;
    float v     = k->drag_start_val + delta;
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    k->value = v;
    if (k->on_change) k->on_change(v, k->ctx);
    return 1;
}

void awgt_knob_mouse_up(awgt_knob_t *k)
{
    k->dragging = 0;
}
