/*
 * AetherOS — libAetherAudioWidget: VU meter + spectrum analyzer (Phase 8.9)
 * File: userspace/lib/libAetherAudioWidget/src/awgt_vu.c
 */

#include "awgt.h"
#include <gfx.h>
#include <string.h>

/* ── VU meter ─────────────────────────────────────────────────────────── */

/* Segment colour thresholds (normalised 0..1 mapped to 0..AWGT_VU_SEGS) */
#define VU_GREEN_END    12   /* segments 0-11: green  */
#define VU_YELLOW_END   17   /* segments 12-16: yellow */
                             /* segments 17-19: red    */

static unsigned vu_seg_color(int seg, int lit)
{
    if (!lit) return GFX_RGB(18, 22, 18);
    if (seg < VU_GREEN_END)  return GFX_RGB( 50, 210,  50);
    if (seg < VU_YELLOW_END) return GFX_RGB(230, 210,  50);
    return GFX_RGB(235, 60, 60);
}

void awgt_vu_init(awgt_vu_t *v, int x, int y, int w, int h,
                  int orientation, const char *label)
{
    memset(v, 0, sizeof(*v));
    v->x = x; v->y = y; v->w = w; v->h = h;
    v->orientation = orientation;
    v->label = label;
}

void awgt_vu_feed(awgt_vu_t *v, float level)
{
    if (level < 0.0f) level = 0.0f;
    if (level > 1.0f) level = 1.0f;
    v->level = level;
    if (level >= v->peak) {
        v->peak       = level;
        v->peak_timer = AWGT_VU_PEAK_HOLD;
    } else if (v->peak_timer > 0) {
        v->peak_timer--;
    } else {
        v->peak -= 0.01f;
        if (v->peak < 0.0f) v->peak = 0.0f;
    }
}

void awgt_vu_draw(const awgt_vu_t *v)
{
    /* Background */
    gfx_fill_rounded((unsigned)v->x, (unsigned)v->y,
                     (unsigned)v->w, (unsigned)v->h,
                     3, GFX_RGB(12, 14, 12));

    int n = AWGT_VU_SEGS;
    int lit_segs  = (int)(v->level * (float)n);
    int peak_seg  = (int)(v->peak  * (float)(n - 1));

    if (v->orientation == AWGT_VU_VERTICAL) {
        int seg_h  = (v->h - 2) / n;
        int seg_w  = v->w - 4;
        int gap    = 1;
        for (int i = 0; i < n; i++) {
            /* draw bottom-to-top */
            int seg_y = v->y + v->h - 2 - (i + 1) * seg_h + gap;
            int lit   = (i < lit_segs) ? 1 : 0;
            unsigned c = vu_seg_color(i, lit);
            gfx_fill((unsigned)(v->x + 2), (unsigned)seg_y,
                     (unsigned)seg_w, (unsigned)(seg_h - gap), c);
        }
        /* Peak hold */
        if (v->peak > 0.0f) {
            int seg_y = v->y + v->h - 2 - (peak_seg + 1) * seg_h + gap;
            gfx_fill((unsigned)(v->x + 2), (unsigned)seg_y,
                     (unsigned)(v->w - 4), (unsigned)(seg_h - gap),
                     GFX_RGB(255, 255, 255));
        }
    } else {
        int seg_w  = (v->w - 2) / n;
        int seg_h  = v->h - 4;
        int gap    = 1;
        for (int i = 0; i < n; i++) {
            int seg_x = v->x + 1 + i * seg_w;
            int lit   = (i < lit_segs) ? 1 : 0;
            unsigned c = vu_seg_color(i, lit);
            gfx_fill((unsigned)seg_x, (unsigned)(v->y + 2),
                     (unsigned)(seg_w - gap), (unsigned)seg_h, c);
        }
        if (v->peak > 0.0f) {
            int seg_x = v->x + 1 + peak_seg * seg_w;
            gfx_fill((unsigned)seg_x, (unsigned)(v->y + 2),
                     (unsigned)(seg_w - gap), (unsigned)seg_h,
                     GFX_RGB(255, 255, 255));
        }
    }

    if (v->label) {
        gfx_text_transparent((unsigned)(v->x + 2),
                             (unsigned)(v->y + v->h + 2),
                             v->label, GFX_RGB(120, 120, 140));
    }
}

/* ── Spectrum analyzer ────────────────────────────────────────────────── */

void awgt_spectrum_init(awgt_spectrum_t *s, int x, int y,
                        int w, int h, int n_bins)
{
    memset(s, 0, sizeof(*s));
    s->x = x; s->y = y; s->w = w; s->h = h;
    if (n_bins > AWGT_SPECTRUM_MAX_BINS) n_bins = AWGT_SPECTRUM_MAX_BINS;
    s->n_bins = n_bins;
}

void awgt_spectrum_feed(awgt_spectrum_t *s, const float *magnitudes)
{
    for (int i = 0; i < s->n_bins; i++) {
        float m = magnitudes[i];
        if (m < 0.0f) m = 0.0f;
        if (m > 1.0f) m = 1.0f;
        s->bins[i] = m;
        if (m >= s->peaks[i]) {
            s->peaks[i]        = m;
            s->peak_timers[i]  = AWGT_SPEC_PEAK_HOLD;
        } else if (s->peak_timers[i] > 0) {
            s->peak_timers[i]--;
        } else {
            s->peaks[i] -= 0.008f;
            if (s->peaks[i] < 0.0f) s->peaks[i] = 0.0f;
        }
    }
}

static unsigned spec_bar_color(float frac_x, float frac_y)
{
    /* Horizontal: low freq = blue, high = red
     * Vertical:   low level = dim, high = bright */
    int r = (int)(frac_x * 200.0f + 55.0f);
    int g = (int)((1.0f - frac_x) * 80.0f * frac_y);
    int b = (int)((1.0f - frac_x) * 200.0f + 55.0f);
    int bright = (int)(frac_y * 1.5f * 255.0f);
    if (bright > 255) bright = 255;
    /* mix toward bright */
    r = r * bright / 255;
    g = g * bright / 255;
    b = b * bright / 255;
    return GFX_RGB(r, g, b);
}

void awgt_spectrum_draw(const awgt_spectrum_t *s)
{
    /* Background */
    gfx_fill((unsigned)s->x, (unsigned)s->y,
              (unsigned)s->w, (unsigned)s->h, GFX_RGB(8, 8, 16));

    if (s->n_bins == 0) return;

    int bar_w   = (s->w / s->n_bins) - 1;
    if (bar_w < 1) bar_w = 1;

    for (int i = 0; i < s->n_bins; i++) {
        float fx   = (float)i / (float)(s->n_bins - 1);
        float mag  = s->bins[i];
        int   bar_h = (int)(mag * (float)(s->h - 2));
        int   bx    = s->x + i * (bar_w + 1);
        int   by    = s->y + s->h - 1 - bar_h;

        /* Draw bar segments from bottom up, gradient by height */
        for (int seg = 0; seg < bar_h; seg++) {
            float fy  = (float)seg / (float)(s->h - 2);
            unsigned c = spec_bar_color(fx, fy);
            gfx_hline((unsigned)bx, (unsigned)(by + bar_h - 1 - seg),
                      (unsigned)bar_w, c);
        }

        /* Peak dot */
        int peak_h = (int)(s->peaks[i] * (float)(s->h - 2));
        if (peak_h > 0) {
            int py = s->y + s->h - 1 - peak_h;
            gfx_hline((unsigned)bx, (unsigned)py,
                      (unsigned)bar_w, GFX_RGB(220, 220, 255));
        }
    }
}
