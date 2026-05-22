/*
 * AetherGuitar — Amp view (Phase 8.10 visual overhaul)
 * File: userspace/apps/aether_guitar/view_amp.c
 *
 * amp_head_bg.bmp (804×200) is the photorealistic chrome amp face plate.
 * Interactive knob_base.bmp sprites are composited over the recessed wells.
 *
 * Amp head image knob well positions (image-relative, y=100):
 *   x = [260, 340, 420, 520, 600, 680, 760]
 *   labels = GAIN BASS MID TREBLE PRES VOL MASTER
 *
 * We drive 5 interactive knobs: GAIN(260) BASS(340) MID(420) TREBLE(520) MASTER(760).
 * PRES(600) and VOL(680) are decorative (image-only).
 */

#include "aeguitar.h"
#include "awgt_prims.h"

extern void awgt_fill_circle(int cx, int cy, int r, unsigned color);
extern void awgt_circle(int cx, int cy, int r, unsigned color);
extern void awgt_line(int x0, int y0, int x1, int y1, unsigned color);

/* ── Amp image constants ─────────────────────────────────────────────── */

#define AMP_IMG_W   804
#define AMP_IMG_H   200
#define AMP_KNOB_R   22   /* well radius in image */
#define AMP_KNOB_Y  100   /* knob center Y in image */

/* Interactive knob image-X positions (5 of the 7 wells) */
static const int AMP_KNOB_IX[5] = { 260, 340, 420, 520, 760 };

#define KNOB_START_DEG  135.0f
#define KNOB_SWEEP_DEG  270.0f
#define DEG2RAD(d)      ((d) * 3.14159265f / 180.0f)
#define IND_LEN         (AMP_KNOB_R - 4)

/* ── Knob/image state ────────────────────────────────────────────────── */

#define AMP_PX    (AMP_IMG_W * AMP_IMG_H)
#define KNOB_PX   (64 * 64)

static unsigned g_px_amp[AMP_PX];
static unsigned g_px_knob[KNOB_PX];
static unsigned g_amp_w, g_amp_h;
static unsigned g_knob_w, g_knob_h;
static int      g_imgs_ok;

/* 5 interactive knob values (normalized 0..1) */
static float g_knob_val[5] = { 0.5f, 0.5f, 0.5f, 0.5f, 0.7f };

/* Tonestack selector */
static int g_tonestack = 0;
static const char *ts_names[3] = { "FENDER", "MARSHALL", "VOX" };

static const char *g_nam_name = "No model loaded";
static const char *g_cab_name = "No IR loaded";

/* Drag state */
static int g_drag_knob = -1;
static int g_drag_y0   = 0;
static float g_drag_val0 = 0.0f;

/* ── Init ────────────────────────────────────────────────────────────── */

void view_amp_init(void)
{
    int ok = gfx_bmp_load_icon("/initrd/aeguitar/amp_head_bg.bmp",
                                g_px_amp, AMP_PX,
                                &g_amp_w, &g_amp_h);
    g_imgs_ok = (ok == 0);

    gfx_bmp_load_icon("/initrd/aeguitar/knob_base.bmp",
                      g_px_knob, KNOB_PX,
                      &g_knob_w, &g_knob_h);
}

/* ── Helpers ─────────────────────────────────────────────────────────── */

static void draw_amp_knob(int screen_cx, int screen_cy, float value)
{
    int kd = AMP_KNOB_R * 2;
    int kx = screen_cx - AMP_KNOB_R;
    int ky = screen_cy - AMP_KNOB_R;

    if (g_knob_w > 0) {
        gfx_icon_blit(g_px_knob, g_knob_w, g_knob_h, kx, ky, kd, kd);
    } else {
        awgt_fill_circle(screen_cx, screen_cy, AMP_KNOB_R, GFX_RGB(40, 42, 50));
        awgt_circle(screen_cx, screen_cy, AMP_KNOB_R, GFX_RGB(120, 120, 140));
    }

    /* Indicator line */
    float angle = DEG2RAD(KNOB_START_DEG) + value * DEG2RAD(KNOB_SWEEP_DEG);
    float fc = (float)cos((double)angle);
    float fs = (float)sin((double)angle);
    int tx = screen_cx + (int)(fc * (float)IND_LEN);
    int ty = screen_cy + (int)(fs * (float)IND_LEN);

    unsigned col = GFX_RGB(240, 240, 255);
    awgt_line(screen_cx, screen_cy, tx, ty, col);
    awgt_line(screen_cx + 1, screen_cy, tx + 1, ty, col);
    awgt_fill_circle(tx, ty, 2, GFX_RGB(255, 220, 100));
}

/* ── Draw ────────────────────────────────────────────────────────────── */

void view_amp_draw(int cx, int cy)
{
    (void)cx; (void)cy;

    int bx = cont_x(), by = cont_y();

    /* ── Amp head image (top portion of view) ── */
    if (g_imgs_ok && g_amp_w > 0) {
        gfx_icon_blit(g_px_amp, g_amp_w, g_amp_h,
                      bx, by, AMP_IMG_W, AMP_IMG_H);
    } else {
        gfx_fill((unsigned)bx, (unsigned)by,
                 (unsigned)CONT_W, (unsigned)AMP_IMG_H, GFX_RGB(12, 12, 16));
        gfx_text_transparent((unsigned)(bx + 8), (unsigned)(by + 8),
                             "AetherAmp MODEL 50", GFX_RGB(160, 160, 180));
    }

    /* ── Overlay interactive knobs on the amp face ── */
    for (int k = 0; k < 5; k++) {
        int scx = bx + AMP_KNOB_IX[k];
        int scy = by + AMP_KNOB_Y;
        draw_amp_knob(scx, scy, g_knob_val[k]);

        /* Label below knob (in image space, avoid redrawing over image text) */
    }

    /* ── Controls below the amp image ── */
    int ctrl_y = by + AMP_IMG_H + 10;

    /* Tonestack selector */
    gfx_text_transparent((unsigned)(bx + 8), (unsigned)ctrl_y,
                         "Tonestack:", C_LABEL);
    for (int t = 0; t < 3; t++) {
        int tx = bx + 100 + t * 96;
        unsigned bg = (g_tonestack == t) ? GFX_RGB(60, 55, 120) : C_BTN_N;
        gfx_fill_rounded((unsigned)tx, (unsigned)ctrl_y, 88, 20, 3, bg);
        gfx_rect_rounded((unsigned)tx, (unsigned)ctrl_y, 88, 20, 3, C_BTN_BORDER);
        int nlen = (int)strlen(ts_names[t]);
        int nx   = tx + (88 - nlen * 8) / 2;
        gfx_text_transparent((unsigned)nx, (unsigned)(ctrl_y + 2),
                             ts_names[t], C_VALUE);
    }

    /* NAM model */
    int nam_y = ctrl_y + 34;
    gfx_text_transparent((unsigned)(bx + 8), (unsigned)nam_y, "NAM Model:", C_LABEL);
    gfx_fill_rounded((unsigned)(bx + 100), (unsigned)nam_y,
                     (unsigned)(CONT_W - 200), 20, 3, GFX_RGB(14, 14, 22));
    gfx_rect_rounded((unsigned)(bx + 100), (unsigned)nam_y,
                     (unsigned)(CONT_W - 200), 20, 3, GFX_RGB(50, 50, 80));
    gfx_text_transparent((unsigned)(bx + 104), (unsigned)(nam_y + 2),
                         g_nam_name, C_VALUE);
    draw_btn(bx + CONT_W - 92, nam_y, 88, 20, "Browse", 0);

    /* Cabinet IR */
    int cab_y = nam_y + 34;
    gfx_text_transparent((unsigned)(bx + 8), (unsigned)cab_y, "Cabinet IR:", C_LABEL);
    gfx_fill_rounded((unsigned)(bx + 100), (unsigned)cab_y,
                     (unsigned)(CONT_W - 200), 20, 3, GFX_RGB(14, 14, 22));
    gfx_rect_rounded((unsigned)(bx + 100), (unsigned)cab_y,
                     (unsigned)(CONT_W - 200), 20, 3, GFX_RGB(50, 50, 80));
    gfx_text_transparent((unsigned)(bx + 104), (unsigned)(cab_y + 2),
                         g_cab_name, C_VALUE);
    draw_btn(bx + CONT_W - 92, cab_y, 88, 20, "Browse", 0);

    /* Status bar */
    int st_y = by + CONT_H - 22;
    gfx_fill((unsigned)bx, (unsigned)st_y,
              (unsigned)CONT_W, 20, GFX_RGB(16, 16, 26));
    char buf[64];
    snprintf(buf, sizeof(buf), "SR: %u Hz   Period: %u frames", g_sr, g_period);
    gfx_text_transparent((unsigned)(bx + 8), (unsigned)(st_y + 2),
                         buf, GFX_RGB(80, 80, 110));
}

/* ── Mouse ───────────────────────────────────────────────────────────── */

void view_amp_mouse(int mx, int my, unsigned btn, unsigned prev_btn)
{
    int pressed  = (btn & 1) && !(prev_btn & 1);
    int released = !(btn & 1) && (prev_btn & 1);
    int held     = (btn & 1) && (prev_btn & 1);
    int bx = cont_x(), by = cont_y();

    /* Release drag */
    if (released && g_drag_knob >= 0) {
        g_drag_knob = -1;
        return;
    }

    /* Drag knob */
    if (held && g_drag_knob >= 0) {
        float delta = (float)(g_drag_y0 - my) / 200.0f;
        float v = g_drag_val0 + delta;
        if (v < 0.0f) v = 0.0f;
        if (v > 1.0f) v = 1.0f;
        g_knob_val[g_drag_knob] = v;
        /* Post to chain */
        if (g_drag_knob == 0)
            achain_param_post(g_chain, NODE_DRIVE, 1, v);
        else if (g_drag_knob == 4)
            achain_param_post(g_chain, NODE_DRIVE, 3, v);
        return;
    }

    if (!pressed) return;

    /* Knob hit-test (amp face area) */
    for (int k = 0; k < 5; k++) {
        int scx = bx + AMP_KNOB_IX[k];
        int scy = by + AMP_KNOB_Y;
        int dx = mx - scx, dy = my - scy;
        if (dx * dx + dy * dy <= AMP_KNOB_R * AMP_KNOB_R) {
            g_drag_knob  = k;
            g_drag_y0    = my;
            g_drag_val0  = g_knob_val[k];
            return;
        }
    }

    /* Tonestack buttons */
    int ctrl_y = by + AMP_IMG_H + 10;
    for (int t = 0; t < 3; t++) {
        int tx = bx + 100 + t * 96;
        if (btn_hit(tx, ctrl_y, 88, 20, mx, my)) {
            g_tonestack = t;
            return;
        }
    }
}
