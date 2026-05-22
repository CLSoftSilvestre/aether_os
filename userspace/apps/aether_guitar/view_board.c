/*
 * AetherGuitar — Pedalboard view (Phase 8.10)
 * File: userspace/apps/aether_guitar/view_board.c
 *
 * 5 effect pedals in a horizontal row (nodes 1-5 of the chain).
 * Each pedal renders an awgt_panel with up to 3 knobs and a stomp switch.
 * Knobs control the first 3 automatable parameters; stomp toggles bypass.
 */

#include "aeguitar.h"

/* ── Layout ──────────────────────────────────────────────────────────── */

#define N_SLOTS   5       /* nodes 1..5 (0 is tuner, hidden)  */
#define SLOT_W  144       /* per-pedal width (pixels)          */
#define SLOT_H  248       /* per-pedal height                  */
#define SLOT_GAP  10      /* horizontal gap between pedals     */
#define KNOB_SZ   44      /* knob diameter                     */
#define KNOB_GAP  12

/* ── Slot state ──────────────────────────────────────────────────────── */

typedef struct {
    int          node_idx;   /* chain node index; -1 = empty slot */
    awgt_panel_t panel;
    awgt_knob_t  knobs[3];
    int          n_knobs;
    awgt_stomp_t stomp;
    char         title[32];
} slot_t;

static slot_t g_slots[N_SLOTS];

/* ── Helper: knob → param_post ───────────────────────────────────────── */

typedef struct { int slot; int knob; } knob_ctx_t;
static knob_ctx_t g_knob_ctx[N_SLOTS * 3];

static void knob_changed(float value, void *ctx)
{
    knob_ctx_t *kc   = (knob_ctx_t *)ctx;
    slot_t     *slot = &g_slots[kc->slot];
    if (slot->node_idx < 0) return;

    aplug_t *plug = achain_node(g_chain, slot->node_idx);
    if (!plug) return;

    /* Find the kc->knob-th automatable (non-bypass) param */
    int param_count = plug->desc->num_params;
    int found = 0;
    for (int p = 0; p < param_count; p++) {
        const aplug_param_t *par = &plug->desc->params[p];
        if (par->flags & APLUG_PARAM_AUTOMATABLE) {
            if (found == kc->knob) {
                float v = par->min + value * (par->max - par->min);
                achain_param_post(g_chain, slot->node_idx, par->id, v);
                return;
            }
            found++;
        }
    }
}

/* ── Helper: stomp toggle → bypass ──────────────────────────────────── */

typedef struct { int slot; } stomp_ctx_t;
static stomp_ctx_t g_stomp_ctx[N_SLOTS];

static void stomp_toggled(int active, void *ctx)
{
    stomp_ctx_t *sc = (stomp_ctx_t *)ctx;
    slot_t      *sl = &g_slots[sc->slot];
    if (sl->node_idx >= 0)
        achain_set_bypass(g_chain, sl->node_idx, active ? 0 : 1);
}

/* ── Init one slot ───────────────────────────────────────────────────── */

static void slot_init(int i, int cx, int cy)
{
    slot_t *sl = &g_slots[i];
    sl->node_idx = i + 1;   /* chain node 1..5 */

    int bx = cx + i * (SLOT_W + SLOT_GAP);
    int by = cy + (CONT_H - SLOT_H) / 2;

    aplug_t *plug = achain_node(g_chain, sl->node_idx);
    if (!plug) { sl->node_idx = -1; return; }

    /* Slot title from plugin name (trimmed to 14 chars) */
    const char *pname = plug->desc->name;
    int nlen = (int)strlen(pname);
    if (nlen > 13) nlen = 13;
    memcpy(sl->title, pname, (unsigned)nlen);
    sl->title[nlen] = '\0';

    /* Panel */
    sl->panel.x = bx; sl->panel.y = by;
    sl->panel.w = SLOT_W; sl->panel.h = SLOT_H;
    sl->panel.style  = AWGT_PANEL_PEDAL;
    sl->panel.title  = sl->title;
    sl->panel.accent = C_ACCENT;

    /* Collect automatable params */
    int param_count = plug->desc->num_params;
    sl->n_knobs = 0;
    int ki = 0;
    for (int p = 0; p < param_count && sl->n_knobs < 3; p++) {
        const aplug_param_t *par = &plug->desc->params[p];
        if (!(par->flags & APLUG_PARAM_AUTOMATABLE)) continue;

        int col   = sl->n_knobs % 2;
        int row   = sl->n_knobs / 2;
        int kx    = bx + 18 + col * (KNOB_SZ + KNOB_GAP);
        int ky    = by + 38 + row * (KNOB_SZ + 24);

        awgt_knob_init(&sl->knobs[sl->n_knobs], kx, ky,
                       KNOB_SZ, AWGT_KNOB_STYLE_DAVIES, par->name);

        /* Set knob value from current param */
        float cur = aplug_param_get(plug, par->id);
        float range = par->max - par->min;
        float norm  = (range > 0.0f) ? (cur - par->min) / range : 0.5f;
        if (norm < 0.0f) norm = 0.0f;
        if (norm > 1.0f) norm = 1.0f;
        sl->knobs[sl->n_knobs].value = norm;

        /* Wire callback */
        g_knob_ctx[ki].slot = i;
        g_knob_ctx[ki].knob = sl->n_knobs;
        sl->knobs[sl->n_knobs].on_change = knob_changed;
        sl->knobs[sl->n_knobs].ctx       = &g_knob_ctx[ki];
        ki++;
        sl->n_knobs++;
    }

    /* Stomp switch */
    int stompy = by + SLOT_H - 80;
    awgt_stomp_init(&sl->stomp, bx + 22, stompy, SLOT_W - 44, 60, "");
    sl->stomp.active   = achain_get_bypass(g_chain, sl->node_idx) ? 0 : 1;
    g_stomp_ctx[i].slot = i;
    sl->stomp.on_toggle = stomp_toggled;
    sl->stomp.ctx       = &g_stomp_ctx[i];
}

/* ── Public init ─────────────────────────────────────────────────────── */

void view_board_init(void)
{
    if (!g_chain) return;

    /* Total board width and centering */
    int total_w = N_SLOTS * SLOT_W + (N_SLOTS - 1) * SLOT_GAP;
    int cx      = cont_x() + (CONT_W - total_w) / 2;

    for (int i = 0; i < N_SLOTS; i++)
        slot_init(i, cx, cont_y());
}

/* ── Draw ─────────────────────────────────────────────────────────────── */

void view_board_draw(int cx, int cy)
{
    (void)cx; (void)cy;

    /* Pedalboard background */
    int bx = cont_x(), by = cont_y();
    gfx_fill((unsigned)bx, (unsigned)by,
              (unsigned)CONT_W, (unsigned)CONT_H, C_BOARD_BG);

    /* Horizontal wood strips */
    for (int r = 0; r < CONT_H; r += 6) {
        gfx_hline((unsigned)bx, (unsigned)(by + r),
                  (unsigned)CONT_W, C_BOARD_STRIP);
    }

    /* Velcro-like texture (dots) */
    for (int r = 2; r < CONT_H; r += 6) {
        for (int c = 3; c < CONT_W; c += 6) {
            unsigned color = GFX_RGB(30, 24, 18);
            gfx_fill((unsigned)(bx + c), (unsigned)(by + r), 2, 2, color);
        }
    }

    /* Draw each pedal slot */
    for (int i = 0; i < N_SLOTS; i++) {
        slot_t *sl = &g_slots[i];
        if (sl->node_idx < 0) continue;

        awgt_panel_draw(&sl->panel);

        for (int k = 0; k < sl->n_knobs; k++)
            awgt_knob_draw(&sl->knobs[k]);

        awgt_stomp_draw(&sl->stomp);
    }

    /* Footer: input/output labels */
    gfx_text_transparent((unsigned)(bx + 4),
                         (unsigned)(by + CONT_H - 18),
                         "IN", GFX_RGB(80, 80, 100));
    gfx_text_transparent((unsigned)(bx + CONT_W - 20),
                         (unsigned)(by + CONT_H - 18),
                         "OUT", GFX_RGB(80, 80, 100));
}

/* ── Mouse dispatch ───────────────────────────────────────────────────── */

static int g_drag_slot = -1;
static int g_drag_knob = -1;

void view_board_mouse(int mx, int my, unsigned btn, unsigned prev_btn)
{
    int pressed  = (btn & 1) && !(prev_btn & 1);
    int released = !(btn & 1) && (prev_btn & 1);
    int held     = (btn & 1) && (prev_btn & 1);

    if (released && g_drag_slot >= 0) {
        awgt_knob_mouse_up(&g_slots[g_drag_slot].knobs[g_drag_knob]);
        g_drag_slot = -1;
        return;
    }

    if (held && g_drag_slot >= 0) {
        awgt_knob_mouse_move(&g_slots[g_drag_slot].knobs[g_drag_knob],
                             mx, my);
        return;
    }

    for (int i = 0; i < N_SLOTS; i++) {
        slot_t *sl = &g_slots[i];
        if (sl->node_idx < 0) continue;

        /* Stomp click */
        if (pressed && awgt_stomp_click(&sl->stomp, mx, my))
            return;

        /* Knob drag start */
        for (int k = 0; k < sl->n_knobs; k++) {
            if (pressed && awgt_knob_mouse_down(&sl->knobs[k], mx, my)) {
                g_drag_slot = i;
                g_drag_knob = k;
                return;
            }
        }
    }
}
