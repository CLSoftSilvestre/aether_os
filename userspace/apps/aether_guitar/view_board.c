/*
 * AetherGuitar — Pedalboard view (Phase 8.10 visual overhaul)
 * File: userspace/apps/aether_guitar/view_board.c
 *
 * Image-based rendering: pedalboard_bg.bmp + pedal_*.bmp backgrounds,
 * Phong-lit knob_base.bmp with computed indicator lines, stomp_on/off.bmp.
 * Interaction (drag, click) unchanged — awgt hit-test functions still used.
 *
 * Pedal layout within image (144×248):
 *   knob wells  : y=45, x=[34,72,110], r=20
 *   LED centre  : (72, 142)
 *   stomp centre: (72, 186)
 */

#include "aeguitar.h"
#include "awgt_prims.h"

extern void awgt_line(int x0, int y0, int x1, int y1, unsigned color);
extern void awgt_fill_circle(int cx, int cy, int r, unsigned color);
extern void awgt_circle(int cx, int cy, int r, unsigned color);

/* ── Layout constants ────────────────────────────────────────────────── */

#define N_SLOTS    5
#define SLOT_W   150   /* render width  — image (144) scaled up */
#define SLOT_H   340   /* render height — image (248) scaled up */
#define SLOT_GAP   8

/* Positions within the RENDERED slot (proportionally scaled from image):
 *   scale_x = 150/144, scale_y = 340/248
 * Original image positions: knob_y=45, knob_r=20, knob_xs=[34,72,110]
 *                            led=(72,142), stomp=(72,186)
 */
#define PEDAL_KNOB_Y   62   /* 45 * 340/248 */
#define PEDAL_KNOB_R   21   /* 20 * 150/144 */
static const int PEDAL_KNOB_X[3] = { 35, 75, 115 };  /* 34/72/110 * 150/144 */
#define PEDAL_LED_X    75   /* 72 * 150/144 */
#define PEDAL_LED_Y   195   /* 142 * 340/248 */
#define PEDAL_STOMP_X  75   /* 72 * 150/144 */
#define PEDAL_STOMP_Y 255   /* 186 * 340/248 */

/* Knob angle maths — match awgt_knob.c conventions */
#define KNOB_START_DEG  135.0f
#define KNOB_SWEEP_DEG  270.0f
#define DEG2RAD(d)      ((d) * 3.14159265f / 180.0f)

/* Indicator line length (slightly shorter than knob radius) */
#define IND_LEN  (PEDAL_KNOB_R - 4)

/* ── Image pixel buffers (loaded once at init) ────────────────────────── */

#define BOARD_PIXELS  (804 * 496)
#define PEDAL_PIXELS  (144 * 248)
#define KNOB_PIXELS   (64  * 64)
#define STOMP_PIXELS  (100 * 60)
#define LED_PIXELS    (20  * 20)

static unsigned g_px_board[BOARD_PIXELS];
static unsigned g_px_pedal[N_SLOTS][PEDAL_PIXELS];
static unsigned g_px_knob[KNOB_PIXELS];
static unsigned g_px_stomp_off[STOMP_PIXELS];
static unsigned g_px_stomp_on[STOMP_PIXELS];
static unsigned g_px_led_on[LED_PIXELS];
static unsigned g_px_led_off[LED_PIXELS];

static unsigned g_board_w, g_board_h;
static unsigned g_pedal_w[N_SLOTS], g_pedal_h[N_SLOTS];
static unsigned g_knob_w, g_knob_h;
static unsigned g_stomp_w, g_stomp_h;
static unsigned g_led_w, g_led_h;

static int g_imgs_ok = 0;  /* set 1 if at least board bg loaded */

static const char *pedal_bmp[N_SLOTS] = {
    "/initrd/aeguitar/pedal_blue.bmp",     /* node 1: Noise Gate   */
    "/initrd/aeguitar/pedal_cobalt.bmp",   /* node 2: Overdrive    */
    "/initrd/aeguitar/pedal_teal.bmp",     /* node 3: Chorus       */
    "/initrd/aeguitar/pedal_amber.bmp",    /* node 4: Delay        */
    "/initrd/aeguitar/pedal_violet.bmp",   /* node 5: Plate Reverb */
};

/* ── Slot state ──────────────────────────────────────────────────────── */

typedef struct {
    int   node_idx;
    float knob_val[3];   /* normalized 0..1 */
    int   n_knobs;
    int   active;        /* 1 = not bypassed */
    char  title[32];
    /* sx/sy NOT stored — computed dynamically each frame so window moves work */
} slot_t;

static slot_t g_slots[N_SLOTS];

/* ── Knob callback context ───────────────────────────────────────────── */

typedef struct { int slot; int knob; } knob_ctx_t;
static knob_ctx_t g_knob_ctx[N_SLOTS * 3];

/* Drag state */
static int g_drag_slot = -1;
static int g_drag_knob = -1;
static int g_drag_y0   = 0;
static float g_drag_val0 = 0.0f;

/* ── Stomp callback context ──────────────────────────────────────────── */

typedef struct { int slot; } stomp_ctx_t;
static stomp_ctx_t g_stomp_ctx[N_SLOTS];

/* ── Helpers ─────────────────────────────────────────────────────────── */

/* Compute screen top-left for slot i given the current content origin.
 * Called every draw/mouse frame so window moves never cause stale positions. */
static void slot_screen_pos(int i, int bx, int by, int *sx_out, int *sy_out)
{
    int total_w = N_SLOTS * SLOT_W + (N_SLOTS - 1) * SLOT_GAP;
    *sx_out = bx + (CONT_W - total_w) / 2 + i * (SLOT_W + SLOT_GAP);
    *sy_out = by + (CONT_H - SLOT_H) / 2;
}

/* Returns denormalized value and (via *param_id_out) the actual param ID. */
static float knob_denorm(slot_t *sl, int k, float norm, unsigned *param_id_out)
{
    aplug_t *plug = achain_node(g_chain, sl->node_idx);
    if (!plug) { if (param_id_out) *param_id_out = 0; return norm; }
    int found = 0;
    for (int p = 0; p < plug->desc->num_params; p++) {
        const aplug_param_t *par = &plug->desc->params[p];
        if (!(par->flags & APLUG_PARAM_AUTOMATABLE)) continue;
        if (found == k) {
            if (param_id_out) *param_id_out = (unsigned)par->id;
            return par->min + norm * (par->max - par->min);
        }
        found++;
    }
    if (param_id_out) *param_id_out = 0;
    return norm;
}

static void draw_knob_indicator(int sx, int sy, float value)
{
    /* Compute angle in radians — screen-space (y-down, CW positive) */
    float angle = DEG2RAD(KNOB_START_DEG) + value * DEG2RAD(KNOB_SWEEP_DEG);

    float fc = (float)cos((double)angle);
    float fs = (float)sin((double)angle);

    int cx = sx + PEDAL_KNOB_R;
    int cy = sy + PEDAL_KNOB_R;
    int tx = cx + (int)(fc * (float)IND_LEN);
    int ty = cy + (int)(fs * (float)IND_LEN);

    /* White indicator line, 2px thick via one parallel offset */
    unsigned col = GFX_RGB(240, 240, 255);
    awgt_line(cx, cy, tx, ty, col);
    awgt_line(cx + 1, cy, tx + 1, ty, col);
    awgt_line(cx, cy + 1, tx, ty + 1, col);
    /* Dot at tip */
    awgt_fill_circle(tx, ty, 2, GFX_RGB(255, 220, 100));
}

/* ── Init ────────────────────────────────────────────────────────────── */

void view_board_init(void)
{
    /* Load board background */
    int ok = gfx_bmp_load_icon("/initrd/aeguitar/pedalboard_bg.bmp",
                                g_px_board, BOARD_PIXELS,
                                &g_board_w, &g_board_h);
    g_imgs_ok = (ok == 0);

    /* Load pedal images */
    for (int i = 0; i < N_SLOTS; i++) {
        gfx_bmp_load_icon(pedal_bmp[i],
                          g_px_pedal[i], PEDAL_PIXELS,
                          &g_pedal_w[i], &g_pedal_h[i]);
    }

    /* Load knob base (Phong-lit sphere) */
    gfx_bmp_load_icon("/initrd/aeguitar/knob_base.bmp",
                      g_px_knob, KNOB_PIXELS,
                      &g_knob_w, &g_knob_h);

    /* Load stomp images */
    gfx_bmp_load_icon("/initrd/aeguitar/stomp_off.bmp",
                      g_px_stomp_off, STOMP_PIXELS,
                      &g_stomp_w, &g_stomp_h);
    gfx_bmp_load_icon("/initrd/aeguitar/stomp_on.bmp",
                      g_px_stomp_on, STOMP_PIXELS,
                      &g_stomp_w, &g_stomp_h);

    /* Load LED images */
    gfx_bmp_load_icon("/initrd/aeguitar/led_green_on.bmp",
                      g_px_led_on, LED_PIXELS,
                      &g_led_w, &g_led_h);
    gfx_bmp_load_icon("/initrd/aeguitar/led_green_off.bmp",
                      g_px_led_off, LED_PIXELS,
                      &g_led_w, &g_led_h);

    if (!g_chain) return;

    for (int i = 0; i < N_SLOTS; i++) {
        slot_t *sl = &g_slots[i];
        sl->node_idx = i + 1;

        aplug_t *plug = achain_node(g_chain, sl->node_idx);
        if (!plug) { sl->node_idx = -1; continue; }

        const char *pname = plug->desc->name;
        int nlen = (int)strlen(pname);
        if (nlen > 13) nlen = 13;
        memcpy(sl->title, pname, (unsigned)nlen);
        sl->title[nlen] = '\0';

        sl->active  = achain_get_bypass(g_chain, sl->node_idx) ? 0 : 1;

        /* Collect normalized param values */
        sl->n_knobs = 0;
        for (int p = 0; p < plug->desc->num_params && sl->n_knobs < 3; p++) {
            const aplug_param_t *par = &plug->desc->params[p];
            if (!(par->flags & APLUG_PARAM_AUTOMATABLE)) continue;
            float cur = aplug_param_get(plug, par->id);
            float range = par->max - par->min;
            float norm  = (range > 0.0f) ? (cur - par->min) / range : 0.5f;
            if (norm < 0.0f) norm = 0.0f;
            if (norm > 1.0f) norm = 1.0f;
            sl->knob_val[sl->n_knobs] = norm;

            g_knob_ctx[i * 3 + sl->n_knobs].slot = i;
            g_knob_ctx[i * 3 + sl->n_knobs].knob = sl->n_knobs;
            g_stomp_ctx[i].slot = i;
            sl->n_knobs++;
        }
    }
}

/* ── Draw ────────────────────────────────────────────────────────────── */

void view_board_draw(int cx, int cy)
{
    (void)cx; (void)cy;

    int bx = cont_x(), by = cont_y();

    /* ── Board background ── */
    if (g_imgs_ok && g_board_w > 0) {
        gfx_icon_blit(g_px_board, g_board_w, g_board_h,
                      bx, by, CONT_W, CONT_H);
    } else {
        /* Fallback: dark wood procedural */
        gfx_fill((unsigned)bx, (unsigned)by,
                 (unsigned)CONT_W, (unsigned)CONT_H, C_BOARD_BG);
        for (int r = 0; r < CONT_H; r += 6)
            gfx_hline((unsigned)bx, (unsigned)(by + r),
                      (unsigned)CONT_W, C_BOARD_STRIP);
    }

    /* ── Pedal slots ── */
    for (int i = 0; i < N_SLOTS; i++) {
        slot_t *sl = &g_slots[i];
        if (sl->node_idx < 0) continue;

        int sx, sy;
        slot_screen_pos(i, bx, by, &sx, &sy);

        /* Pedal body image */
        if (g_pedal_w[i] > 0) {
            gfx_icon_blit(g_px_pedal[i], g_pedal_w[i], g_pedal_h[i],
                          sx, sy, SLOT_W, SLOT_H);
        } else {
            gfx_fill((unsigned)sx, (unsigned)sy,
                     (unsigned)SLOT_W, (unsigned)SLOT_H, GFX_RGB(30, 30, 50));
        }

        /* ── Knobs ── */
        for (int k = 0; k < sl->n_knobs; k++) {
            int kx = sx + PEDAL_KNOB_X[k] - PEDAL_KNOB_R;
            int ky = sy + PEDAL_KNOB_Y    - PEDAL_KNOB_R;
            int kd = PEDAL_KNOB_R * 2;  /* 40px */

            if (g_knob_w > 0) {
                /* Phong-lit knob base image */
                gfx_icon_blit(g_px_knob, g_knob_w, g_knob_h,
                              kx, ky, kd, kd);
            } else {
                /* Procedural fallback */
                awgt_fill_circle(kx + PEDAL_KNOB_R, ky + PEDAL_KNOB_R,
                                 PEDAL_KNOB_R, GFX_RGB(40, 42, 50));
                awgt_circle(kx + PEDAL_KNOB_R, ky + PEDAL_KNOB_R,
                            PEDAL_KNOB_R, GFX_RGB(120, 120, 140));
            }

            /* Indicator line on top of knob */
            draw_knob_indicator(kx, ky, sl->knob_val[k]);
        }

        /* ── LED indicator ── */
        {
            int lx = sx + PEDAL_LED_X - (int)(g_led_w / 2);
            int ly = sy + PEDAL_LED_Y - (int)(g_led_h / 2);

            if (g_led_w > 0) {
                const unsigned *lp = sl->active ? g_px_led_on : g_px_led_off;
                gfx_icon_blit(lp, g_led_w, g_led_h, lx, ly,
                              (int)g_led_w, (int)g_led_h);
            } else {
                /* Procedural LED */
                unsigned lc = sl->active ? GFX_RGB(60, 220, 60)
                                         : GFX_RGB(20, 60, 20);
                awgt_fill_circle(sx + PEDAL_LED_X, sy + PEDAL_LED_Y, 5, lc);
            }
        }

        /* ── Stomp switch ── */
        {
            int stx = sx + PEDAL_STOMP_X - (int)(g_stomp_w / 2);
            int sty = sy + PEDAL_STOMP_Y - (int)(g_stomp_h / 2);

            if (g_stomp_w > 0) {
                const unsigned *sp = sl->active ? g_px_stomp_on
                                                : g_px_stomp_off;
                gfx_icon_blit(sp, g_stomp_w, g_stomp_h,
                              stx, sty, (int)g_stomp_w, (int)g_stomp_h);
            } else {
                /* Procedural stomp */
                unsigned sc = sl->active ? GFX_RGB(60, 180, 60)
                                         : GFX_RGB(60, 60, 70);
                awgt_fill_circle(sx + PEDAL_STOMP_X, sy + PEDAL_STOMP_Y,
                                 18, sc);
                awgt_circle(sx + PEDAL_STOMP_X, sy + PEDAL_STOMP_Y,
                            18, GFX_RGB(100, 100, 120));
            }
        }
    }

    /* Footer labels */
    gfx_text_transparent((unsigned)(bx + 4),
                         (unsigned)(by + CONT_H - 18),
                         "IN", GFX_RGB(180, 180, 200));
    gfx_text_transparent((unsigned)(bx + CONT_W - 24),
                         (unsigned)(by + CONT_H - 18),
                         "OUT", GFX_RGB(180, 180, 200));
}

/* ── Mouse dispatch ──────────────────────────────────────────────────── */

static int knob_hittest(int sx, int sy, int k, int mx, int my)
{
    int kx = sx + PEDAL_KNOB_X[k];
    int ky = sy + PEDAL_KNOB_Y;
    int dx = mx - kx, dy = my - ky;
    return (dx * dx + dy * dy) <= (PEDAL_KNOB_R * PEDAL_KNOB_R);
}

static int stomp_hittest(int sx, int sy, int mx, int my)
{
    int stw = (g_stomp_w > 0) ? (int)g_stomp_w : 36;
    int sth = (g_stomp_h > 0) ? (int)g_stomp_h : 36;
    int x0  = sx + PEDAL_STOMP_X - stw / 2;
    int y0  = sy + PEDAL_STOMP_Y - sth / 2;
    return (mx >= x0 && mx < x0 + stw && my >= y0 && my < y0 + sth);
}

void view_board_mouse(int mx, int my, unsigned btn, unsigned prev_btn)
{
    int pressed  = (btn & 1) && !(prev_btn & 1);
    int released = !(btn & 1) && (prev_btn & 1);
    int held     = (btn & 1) && (prev_btn & 1);

    /* Release drag */
    if (released && g_drag_slot >= 0) {
        g_drag_slot = -1;
        return;
    }

    /* Drag knob (200px = full sweep) */
    if (held && g_drag_slot >= 0) {
        slot_t *sl = &g_slots[g_drag_slot];
        int k = g_drag_knob;
        float delta = (float)(g_drag_y0 - my) / 200.0f;
        float v = g_drag_val0 + delta;
        if (v < 0.0f) v = 0.0f;
        if (v > 1.0f) v = 1.0f;
        sl->knob_val[k] = v;
        unsigned pid = 0;
        float dv = knob_denorm(sl, k, v, &pid);
        if (pid > 0)
            achain_param_post(g_chain, sl->node_idx, pid, dv);
        return;
    }

    /* Click events */
    if (!pressed) return;

    int bx = cont_x(), by = cont_y();
    for (int i = 0; i < N_SLOTS; i++) {
        slot_t *sl = &g_slots[i];
        if (sl->node_idx < 0) continue;

        int sx, sy;
        slot_screen_pos(i, bx, by, &sx, &sy);

        /* Stomp toggle */
        if (stomp_hittest(sx, sy, mx, my)) {
            sl->active = !sl->active;
            achain_set_bypass(g_chain, sl->node_idx, sl->active ? 0 : 1);
            return;
        }

        /* Knob drag start */
        for (int k = 0; k < sl->n_knobs; k++) {
            if (knob_hittest(sx, sy, k, mx, my)) {
                g_drag_slot  = i;
                g_drag_knob  = k;
                g_drag_y0    = my;
                g_drag_val0  = sl->knob_val[k];
                return;
            }
        }
    }
}
