/*
 * Phase 7.5 — gui_layout_table: AetherOS text metrics via FreeType
 *
 * NetSurf calls these for every text node during layout to measure character
 * widths and find line-break positions.  We route them through aether_font,
 * which uses a single FreeType instance with Noto Sans loaded.
 *
 * The font handle is lazy-initialised on first call; failure falls back to a
 * simple monospace estimate (6 px per point) so layout degrades gracefully
 * if /fonts/ isn't on the disk yet.
 *
 * plot_font_style.size is a plot_style_fixed (int32_t fixed-point, radix 10).
 * Convert to integer points via plot_style_fixed_to_int(), then scale to
 * screen pixels assuming 96 dpi: px = pt * 96 / 72.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "utils/errors.h"
#include "netsurf/layout.h"
#include "netsurf/plot_style.h"
#include "aether_font.h"
#include "netsurf_aether.h"

/* ── font handle ─────────────────────────────────────────────────────────── */

static aether_font_t *g_font_regular = NULL;
static bool           g_font_tried   = false;

static aether_font_t *get_font(void)
{
    if (g_font_tried) return g_font_regular;
    g_font_tried = true;
    aether_font_load("/fonts/NotoSans-Regular.ttf", &g_font_regular);
    return g_font_regular;
}

/* Convert NetSurf fixed-point pt size to screen pixels at 96 dpi */
static int pts_to_px(plot_style_fixed size_fp)
{
    int pts = plot_style_fixed_to_int(size_fp);
    if (pts <= 0) pts = 12;
    return pts * 96 / 72;
}

/* Fallback width estimate when FreeType is unavailable: 6px per pt */
static int fallback_width(plot_style_fixed size_fp, size_t nchars)
{
    int pts = plot_style_fixed_to_int(size_fp);
    if (pts <= 0) pts = 12;
    return (int)(nchars * (size_t)(pts * 6 / 10));
}

/* ── width ───────────────────────────────────────────────────────────────── */

static nserror layout_width(const plot_font_style_t *fstyle,
                             const char *string, size_t length,
                             int *width_out)
{
    aether_font_t *font = get_font();
    if (!font) {
        *width_out = fallback_width(fstyle->size, length);
        return NSERROR_OK;
    }

    /* aether_font_measure_width works on NUL-terminated strings; we may
       receive a slice, so copy into a stack buffer (most strings are short) */
    char buf[1024];
    size_t n = length < sizeof(buf) - 1 ? length : sizeof(buf) - 1;
    memcpy(buf, string, n);
    buf[n] = '\0';

    int px = pts_to_px(fstyle->size);
    int w  = aether_font_measure_width(font, buf, px);
    *width_out = (w > 0) ? w : fallback_width(fstyle->size, length);
    return NSERROR_OK;
}

/* ── position ────────────────────────────────────────────────────────────── */

static nserror layout_position(const plot_font_style_t *fstyle,
                                const char *string, size_t length,
                                int x, size_t *char_offset, int *actual_x)
{
    /* Walk character-by-character until cumulative width exceeds x */
    aether_font_t *font = get_font();
    int px = pts_to_px(fstyle->size);
    int avg_w = font ? aether_font_measure_width(font, "x", px) : 6;
    if (avg_w <= 0) avg_w = 6;

    int accum = 0;
    size_t i;
    for (i = 0; i < length; i++) {
        char tmp[2] = { string[i], '\0' };
        int cw = font ? aether_font_measure_width(font, tmp, px) : avg_w;
        if (cw <= 0) cw = avg_w;
        if (accum + cw / 2 >= x) break;
        accum += cw;
    }
    *char_offset = i;
    *actual_x    = accum;
    return NSERROR_OK;
}

/* ── split ───────────────────────────────────────────────────────────────── */

static nserror layout_split(const plot_font_style_t *fstyle,
                             const char *string, size_t length,
                             int x, size_t *char_offset, int *actual_x)
{
    aether_font_t *font = get_font();
    int px    = pts_to_px(fstyle->size);
    int avg_w = font ? aether_font_measure_width(font, "x", px) : 6;
    if (avg_w <= 0) avg_w = 6;

    int     accum      = 0;
    size_t  last_space = length; /* last feasible split point */
    int     last_accum = 0;

    for (size_t i = 0; i < length; i++) {
        char tmp[2] = { string[i], '\0' };
        int cw = font ? aether_font_measure_width(font, tmp, px) : avg_w;
        if (cw <= 0) cw = avg_w;

        if (string[i] == ' ' || string[i] == '\t') {
            last_space = i + 1;
            last_accum = accum;
        }

        accum += cw;
        if (accum > x) {
            /* split at last space, or at this character if no space seen */
            if (last_space < length) {
                *char_offset = last_space;
                *actual_x    = last_accum;
            } else {
                *char_offset = (i > 0) ? i : 1;
                *actual_x    = accum - cw;
            }
            return NSERROR_OK;
        }
    }

    /* No split needed — entire string fits */
    *char_offset = length;
    *actual_x    = accum;
    return NSERROR_OK;
}

/* ── exported table ──────────────────────────────────────────────────────── */

struct gui_layout_table aether_layout_table = {
    .width    = layout_width,
    .position = layout_position,
    .split    = layout_split,
};
