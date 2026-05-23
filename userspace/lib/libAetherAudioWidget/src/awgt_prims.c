/*
 * AetherOS — libAetherAudioWidget: drawing primitives (Phase 8.9)
 * File: userspace/lib/libAetherAudioWidget/src/awgt_prims.c
 *
 * Internal helpers — circle fill/outline + Bresenham line.
 * Not exposed through awgt.h.
 */

#include "awgt_prims.h"
#include <gfx.h>

/* ── Integer square root (Newton's method, no FPU) ──────────────────── */

static int isqrt(int n)
{
    if (n <= 0) return 0;
    int x = n, y = (x + 1) >> 1;
    while (y < x) { x = y; y = (x + n / x) >> 1; }
    return x;
}

/* ── Filled circle ───────────────────────────────────────────────────── */

void awgt_fill_circle(int cx, int cy, int r, unsigned color)
{
    for (int dy = -r; dy <= r; dy++) {
        int dx = isqrt(r * r - dy * dy);
        if (dx < 0) continue;
        gfx_hline((unsigned)(cx - dx), (unsigned)(cy + dy),
                  (unsigned)(dx * 2 + 1), color);
    }
}

/* ── Circle outline (1 px Bresenham) ────────────────────────────────── */

void awgt_circle(int cx, int cy, int r, unsigned color)
{
    int x = 0, y = r, d = 3 - 2 * r;
    while (x <= y) {
        /* 8 symmetric points */
        gfx_fill((unsigned)(cx + x), (unsigned)(cy - y), 1, 1, color);
        gfx_fill((unsigned)(cx - x), (unsigned)(cy - y), 1, 1, color);
        gfx_fill((unsigned)(cx + x), (unsigned)(cy + y), 1, 1, color);
        gfx_fill((unsigned)(cx - x), (unsigned)(cy + y), 1, 1, color);
        gfx_fill((unsigned)(cx + y), (unsigned)(cy - x), 1, 1, color);
        gfx_fill((unsigned)(cx - y), (unsigned)(cy - x), 1, 1, color);
        gfx_fill((unsigned)(cx + y), (unsigned)(cy + x), 1, 1, color);
        gfx_fill((unsigned)(cx - y), (unsigned)(cy + x), 1, 1, color);
        if (d < 0) d += 4 * x + 6;
        else { d += 4 * (x - y) + 10; y--; }
        x++;
    }
}

/* ── Bresenham line (integer only) ──────────────────────────────────── */

void awgt_line(int x0, int y0, int x1, int y1, unsigned color)
{
    int dx  = x1 - x0; if (dx < 0) dx = -dx;
    int dy  = y1 - y0; if (dy < 0) dy = -dy;
    int sx  = (x0 < x1) ? 1 : -1;
    int sy  = (y0 < y1) ? 1 : -1;
    int err = dx - dy;

    for (;;) {
        gfx_fill((unsigned)x0, (unsigned)y0, 1, 1, color);
        if (x0 == x1 && y0 == y1) break;
        int e2 = err * 2;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 <  dx) { err += dx; y0 += sy; }
    }
}

/* ── Thick line (draws 'thick' parallel 1-px lines) ─────────────────── */

void awgt_line_thick(int x0, int y0, int x1, int y1,
                     int thick, unsigned color)
{
    for (int t = -(thick / 2); t <= thick / 2; t++) {
        int dx = x1 - x0, dy = y1 - y0;
        /* perpendicular offset */
        if (dx == 0 && dy == 0) { gfx_fill((unsigned)x0, (unsigned)y0, 1, 1, color); return; }
        int adx = dx < 0 ? -dx : dx;
        int ady = dy < 0 ? -dy : dy;
        if (ady >= adx)
            awgt_line(x0 + t, y0, x1 + t, y1, color);
        else
            awgt_line(x0, y0 + t, x1, y1 + t, color);
    }
}
