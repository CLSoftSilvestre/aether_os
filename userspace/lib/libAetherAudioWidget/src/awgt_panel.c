/*
 * AetherOS — libAetherAudioWidget: panel / housing background (Phase 8.9)
 * File: userspace/lib/libAetherAudioWidget/src/awgt_panel.c
 *
 * AWGT_PANEL_PEDAL: stomp-box housing — dark brushed metal, corner screws.
 * AWGT_PANEL_AMP:   amp head face — wider, faux-leather border, chrome strip.
 */

#include "awgt.h"
#include "awgt_prims.h"
#include <gfx.h>
#include <string.h>

/* ── Colours ─────────────────────────────────────────────────────────── */

#define C_PANEL_BASE    GFX_RGB( 32,  32,  42)
#define C_PANEL_HI      GFX_RGB( 50,  50,  65)   /* brushed highlight     */
#define C_PANEL_LO      GFX_RGB( 24,  24,  32)   /* brushed shadow        */
#define C_PANEL_RIM     GFX_RGB( 75,  75,  90)
#define C_PANEL_RIM_HI  GFX_RGB(110, 110, 130)
#define C_SCREW_BODY    GFX_RGB(140, 140, 160)
#define C_SCREW_SLOT    GFX_RGB( 80,  80, 100)
#define C_AMP_LEATHER   GFX_RGB( 22,  18,  14)   /* dark brown faux-leather */
#define C_AMP_CHROME    GFX_RGB(160, 165, 175)   /* chrome face plate       */
#define C_AMP_CHROME_HI GFX_RGB(220, 225, 235)
#define C_TITLE_TEXT    GFX_RGB(240, 240, 255)

/* ── Draw corner screw (Phillips head look) ──────────────────────────── */

static void draw_screw(int cx, int cy)
{
    awgt_fill_circle(cx, cy, 6, GFX_RGB(55, 55, 70));   /* shadow rim */
    awgt_fill_circle(cx, cy, 5, C_SCREW_BODY);
    awgt_fill_circle(cx, cy, 3, GFX_RGB(100, 100, 120));
    /* Phillips slot (cross) */
    gfx_hline((unsigned)(cx - 2), (unsigned)cy,       4, C_SCREW_SLOT);
    gfx_vline((unsigned)cx,       (unsigned)(cy - 2), 4, C_SCREW_SLOT);
}

/* ── Brushed metal fill: alternate thin bands ────────────────────────── */

static void draw_brushed_metal(int x, int y, int w, int h)
{
    for (int row = 0; row < h; row++) {
        unsigned c = ((row & 3) == 0) ? C_PANEL_HI
                   : ((row & 3) == 2) ? C_PANEL_LO
                   :                    C_PANEL_BASE;
        gfx_hline((unsigned)x, (unsigned)(y + row), (unsigned)w, c);
    }
}

/* ── Pedal panel ─────────────────────────────────────────────────────── */

static void draw_pedal(const awgt_panel_t *p)
{
    /* Outer shadow */
    gfx_fill_rounded((unsigned)(p->x + 3), (unsigned)(p->y + 3),
                     (unsigned)p->w, (unsigned)p->h,
                     8, GFX_RGB(10, 10, 15));

    /* Rim */
    gfx_fill_rounded((unsigned)p->x, (unsigned)p->y,
                     (unsigned)p->w, (unsigned)p->h,
                     8, C_PANEL_RIM);

    /* Brushed face (inset 2px) */
    int fx = p->x + 2, fy = p->y + 2;
    int fw = p->w - 4, fh = p->h - 4;
    draw_brushed_metal(fx, fy, fw, fh);
    gfx_rect_rounded((unsigned)fx, (unsigned)fy,
                     (unsigned)fw, (unsigned)fh, 6, C_PANEL_RIM);

    /* Corner screws */
    draw_screw(p->x + 10,       p->y + 10);
    draw_screw(p->x + p->w - 10, p->y + 10);
    draw_screw(p->x + 10,       p->y + p->h - 10);
    draw_screw(p->x + p->w - 10, p->y + p->h - 10);

    /* Title strip (coloured accent band at top) */
    if (p->title) {
        unsigned acc = p->accent ? p->accent : GFX_RGB(90, 50, 170);
        int tx = fx + 18, ty = fy + 6;
        int tw = fw - 36, th = 18;
        gfx_fill_rounded((unsigned)tx, (unsigned)ty,
                         (unsigned)tw, (unsigned)th,
                         3, acc);
        /* Title text centred */
        int tlen  = (int)strlen(p->title);
        int text_x = tx + (tw - tlen * 8) / 2;
        gfx_text_transparent((unsigned)text_x, (unsigned)(ty + 2),
                             p->title, C_TITLE_TEXT);
    }
}

/* ── Amp head panel ──────────────────────────────────────────────────── */

static void draw_amp(const awgt_panel_t *p)
{
    /* Outer cabinet (faux leather look: very dark brown with subtle grain) */
    gfx_fill_rounded((unsigned)p->x, (unsigned)p->y,
                     (unsigned)p->w, (unsigned)p->h,
                     6, C_AMP_LEATHER);
    /* Leather "grain" — very subtle horizontal stripes */
    for (int row = 2; row < p->h - 2; row += 4) {
        gfx_hline((unsigned)(p->x + 2), (unsigned)(p->y + row),
                  (unsigned)(p->w - 4),
                  GFX_RGB(28, 22, 16));
    }

    /* Chrome face plate inset */
    int cx = p->x + 12, cy = p->y + 10;
    int cw = p->w - 24, ch = p->h - 20;
    gfx_fill_rounded((unsigned)cx, (unsigned)cy,
                     (unsigned)cw, (unsigned)ch,
                     4, C_AMP_CHROME);
    /* Chrome highlight bar along top */
    gfx_hline((unsigned)(cx + 1), (unsigned)(cy + 1),
               (unsigned)(cw - 2), C_AMP_CHROME_HI);

    /* Inner brushed metal area (inset further) */
    int mx = cx + 3, my = cy + 4;
    int mw = cw - 6,  mh = ch - 8;
    draw_brushed_metal(mx, my, mw, mh);
    gfx_rect((unsigned)mx, (unsigned)my, (unsigned)mw, (unsigned)mh,
             GFX_RGB(100, 100, 120));

    /* Brand / title centred at top of chrome plate */
    if (p->title) {
        int tlen  = (int)strlen(p->title);
        int text_x = cx + (cw - tlen * 8) / 2;
        gfx_text_transparent((unsigned)text_x, (unsigned)(cy + 5),
                             p->title, C_TITLE_TEXT);
    }
}

/* ── Public ───────────────────────────────────────────────────────────── */

void awgt_panel_draw(const awgt_panel_t *p)
{
    if (p->style == AWGT_PANEL_AMP)
        draw_amp(p);
    else
        draw_pedal(p);
}
