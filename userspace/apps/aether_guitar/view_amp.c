/*
 * AetherGuitar — Amp view (Phase 8.10)
 * File: userspace/apps/aether_guitar/view_amp.c
 *
 * Simulates an amp head face:
 *   - awgt_panel (AMP style) for the chrome face plate
 *   - 5 knobs: GAIN / BASS / MID / TREBLE / MASTER
 *   - Tonestack type selector (FENDER / MARSHALL / VOX)
 *   - NAM model name display + status row
 *   - Cabinet IR name display
 *
 * The knob values are stored locally; they influence the overdrive node
 * in the chain for GAIN and the (tonestack-emulated) EQ parameters.
 */

#include "aeguitar.h"
#include "aamp.h"

/* ── Layout ──────────────────────────────────────────────────────────── */

#define AMP_PANEL_H  210
#define AMP_PANEL_W  CONT_W
#define KNOB_SZ       56
#define KNOB_SPACING  (CONT_W / 5)   /* ~160px per knob */

/* ── Amp knob labels & params ─────────────────────────────────────────── */

static const char *amp_knob_labels[5] = {
    "GAIN", "BASS", "MID", "TREBLE", "MASTER"
};

static awgt_knob_t g_amp_knobs[5];
static awgt_panel_t g_amp_panel;

/* Tonestack selector: 0=Fender 1=Marshall 2=Vox */
static int g_tonestack = 0;
static const char *ts_names[3] = { "FENDER", "MARSHALL", "VOX" };

/* NAM / cab display strings (set at runtime from loaded files) */
static const char *g_nam_name  = "No model loaded";
static const char *g_cab_name  = "No IR loaded";

/* ── Knob callbacks ───────────────────────────────────────────────────── */

static void gain_changed(float v, void *ctx)
{
    (void)ctx;
    /* Map 0..1 → overdrive drive param 0..1 */
    achain_param_post(g_chain, NODE_DRIVE, 1, v);
}

static void master_changed(float v, void *ctx)
{
    (void)ctx;
    /* Master volume: post to all nodes' level-type param?
     * For now, drive node output level param (index 3 = level) */
    achain_param_post(g_chain, NODE_DRIVE, 3, v);
}

/* ── Init ─────────────────────────────────────────────────────────────── */

void view_amp_init(void)
{
    /* Panel */
    g_amp_panel.x      = cont_x();
    g_amp_panel.y      = cont_y();
    g_amp_panel.w      = AMP_PANEL_W;
    g_amp_panel.h      = AMP_PANEL_H;
    g_amp_panel.style  = AWGT_PANEL_AMP;
    g_amp_panel.title  = "AetherAmp";
    g_amp_panel.accent = C_ACCENT;

    /* Knobs: evenly spaced within the panel */
    float defaults[5] = { 0.5f, 0.5f, 0.5f, 0.5f, 0.7f };
    for (int k = 0; k < 5; k++) {
        int kx = cont_x() + k * KNOB_SPACING + (KNOB_SPACING - KNOB_SZ) / 2;
        int ky = cont_y() + 70;
        awgt_knob_init(&g_amp_knobs[k], kx, ky, KNOB_SZ,
                       AWGT_KNOB_STYLE_DAVIES, amp_knob_labels[k]);
        g_amp_knobs[k].value = defaults[k];
    }
    g_amp_knobs[0].on_change = gain_changed;
    g_amp_knobs[4].on_change = master_changed;
}

/* ── Draw ─────────────────────────────────────────────────────────────── */

void view_amp_draw(int cx, int cy)
{
    (void)cx; (void)cy;

    awgt_panel_draw(&g_amp_panel);

    for (int k = 0; k < 5; k++)
        awgt_knob_draw(&g_amp_knobs[k]);

    int bx = cont_x(), by = cont_y();

    /* ── Tonestack selector ── */
    int ts_y = by + AMP_PANEL_H + 14;
    gfx_text_transparent((unsigned)(bx + 8), (unsigned)ts_y,
                         "Tonestack:", C_LABEL);
    for (int t = 0; t < 3; t++) {
        int tx = bx + 100 + t * 96;
        int active = (g_tonestack == t);
        unsigned bg = active ? GFX_RGB(60, 55, 120) : C_BTN_N;
        gfx_fill_rounded((unsigned)tx, (unsigned)ts_y,
                         88, 20, 3, bg);
        gfx_rect_rounded((unsigned)tx, (unsigned)ts_y,
                         88, 20, 3, C_BTN_BORDER);
        int nlen = (int)strlen(ts_names[t]);
        int nx   = tx + (88 - nlen * 8) / 2;
        gfx_text_transparent((unsigned)nx, (unsigned)(ts_y + 2),
                             ts_names[t], C_VALUE);
    }

    /* ── NAM Model ── */
    int nam_y = ts_y + 34;
    gfx_text_transparent((unsigned)(bx + 8), (unsigned)nam_y,
                         "NAM Model:", C_LABEL);
    gfx_fill_rounded((unsigned)(bx + 100), (unsigned)nam_y,
                     (unsigned)(CONT_W - 200), 20, 3, GFX_RGB(14, 14, 22));
    gfx_rect_rounded((unsigned)(bx + 100), (unsigned)nam_y,
                     (unsigned)(CONT_W - 200), 20, 3,
                     GFX_RGB(50, 50, 80));
    gfx_text_transparent((unsigned)(bx + 104), (unsigned)(nam_y + 2),
                         g_nam_name, C_VALUE);
    draw_btn(bx + CONT_W - 92, nam_y, 88, 20, "Browse", 0);

    /* ── Cabinet IR ── */
    int cab_y = nam_y + 34;
    gfx_text_transparent((unsigned)(bx + 8), (unsigned)cab_y,
                         "Cabinet IR:", C_LABEL);
    gfx_fill_rounded((unsigned)(bx + 100), (unsigned)cab_y,
                     (unsigned)(CONT_W - 200), 20, 3, GFX_RGB(14, 14, 22));
    gfx_rect_rounded((unsigned)(bx + 100), (unsigned)cab_y,
                     (unsigned)(CONT_W - 200), 20, 3,
                     GFX_RGB(50, 50, 80));
    gfx_text_transparent((unsigned)(bx + 104), (unsigned)(cab_y + 2),
                         g_cab_name, C_VALUE);
    draw_btn(bx + CONT_W - 92, cab_y, 88, 20, "Browse", 0);

    /* ── Status bar ── */
    int st_y = by + CONT_H - 22;
    gfx_fill((unsigned)bx, (unsigned)st_y,
              (unsigned)CONT_W, 20, GFX_RGB(16, 16, 26));
    char buf[64];
    snprintf(buf, sizeof(buf), "SR: %u Hz   Period: %u frames",
             g_sr, g_period);
    gfx_text_transparent((unsigned)(bx + 8), (unsigned)(st_y + 2),
                         buf, GFX_RGB(80, 80, 110));
}

/* ── Mouse ────────────────────────────────────────────────────────────── */

static int g_amp_drag_knob = -1;

void view_amp_mouse(int mx, int my, unsigned btn, unsigned prev_btn)
{
    int pressed  = (btn & 1) && !(prev_btn & 1);
    int released = !(btn & 1) && (prev_btn & 1);
    int held     = (btn & 1) && (prev_btn & 1);

    if (released && g_amp_drag_knob >= 0) {
        awgt_knob_mouse_up(&g_amp_knobs[g_amp_drag_knob]);
        g_amp_drag_knob = -1;
        return;
    }
    if (held && g_amp_drag_knob >= 0) {
        awgt_knob_mouse_move(&g_amp_knobs[g_amp_drag_knob], mx, my);
        return;
    }

    for (int k = 0; k < 5; k++) {
        if (pressed && awgt_knob_mouse_down(&g_amp_knobs[k], mx, my)) {
            g_amp_drag_knob = k;
            return;
        }
    }

    /* Tonestack buttons */
    int ts_y = cont_y() + AMP_PANEL_H + 14;
    for (int t = 0; t < 3; t++) {
        int tx = cont_x() + 100 + t * 96;
        if (pressed && btn_hit(tx, ts_y, 88, 20, mx, my)) {
            g_tonestack = t;
            return;
        }
    }
}
