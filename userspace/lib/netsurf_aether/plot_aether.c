/*
 * Phase 7.7 — NetSurf plotter for AetherOS
 *
 * Implements struct plotter_table entirely in software, writing pixels
 * directly into an off-screen XRGB8888 buffer.  The caller provides an
 * aether_plot_ctx_t as redraw_context.priv; after browser_window_redraw()
 * it blits the buffer to screen via gfx_raw_blit().
 *
 * Coordinate system: (0,0) = top-left of the content area.
 * Colour format:  NetSurf 'colour' = 0x00RRGGBB = AetherOS XRGB8888.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "utils/errors.h"
#include "netsurf/plotters.h"
#include "netsurf/plot_style.h"
#include "netsurf/bitmap.h"
#include "aether_font.h"
#include "bitmap_aether.h"
#include "plot_aether.h"
#include "netsurf_aether.h"

/* ── colour helpers ───────────────────────────────────────────────────────── */

/* NetSurf colour 0x00RRGGBB → AetherOS XRGB (same format) */
static inline uint32_t ns_rgb(colour c) { return (uint32_t)(c & 0x00FFFFFFu); }

static inline bool ns_transparent(colour c) { return c == NS_TRANSPARENT; }

/* Alpha-blend src (8-bit R,G,B,A) over dst XRGB uint32_t */
static inline uint32_t alpha_blend_rgba(uint8_t sr, uint8_t sg, uint8_t sb,
                                         uint8_t a, uint32_t dst)
{
    if (a == 255) return ((uint32_t)sr << 16) | ((uint32_t)sg << 8) | sb;
    if (a ==   0) return dst;
    uint32_t inv = 255 - a;
    uint32_t dr = (dst >> 16) & 0xFF;
    uint32_t dg = (dst >>  8) & 0xFF;
    uint32_t db =  dst        & 0xFF;
    uint32_t r = (sr * a + dr * inv) / 255;
    uint32_t g = (sg * a + dg * inv) / 255;
    uint32_t b = (sb * a + db * inv) / 255;
    return (r << 16) | (g << 8) | b;
}

/* ── pixel write helpers ─────────────────────────────────────────────────── */

static inline void put_pixel(aether_plot_ctx_t *pc, int x, int y, uint32_t c)
{
    if ((unsigned)x >= (unsigned)pc->clip.x1 || x < pc->clip.x0) return;
    if ((unsigned)y >= (unsigned)pc->clip.y1 || y < pc->clip.y0) return;
    pc->pixels[(size_t)y * pc->stride + x] = c;
}

static void fill_rect(aether_plot_ctx_t *pc,
                       int x0, int y0, int x1, int y1, uint32_t color)
{
    int cx0 = x0 < pc->clip.x0 ? pc->clip.x0 : x0;
    int cy0 = y0 < pc->clip.y0 ? pc->clip.y0 : y0;
    int cx1 = x1 > pc->clip.x1 ? pc->clip.x1 : x1;
    int cy1 = y1 > pc->clip.y1 ? pc->clip.y1 : y1;
    if (cx0 >= cx1 || cy0 >= cy1) return;
    for (int y = cy0; y < cy1; y++) {
        uint32_t *row = pc->pixels + (size_t)y * pc->stride + cx0;
        int n = cx1 - cx0;
        for (int i = 0; i < n; i++) row[i] = color;
    }
}

static void hline(aether_plot_ctx_t *pc, int x0, int x1, int y, uint32_t c)
{
    if (y < pc->clip.y0 || y >= pc->clip.y1) return;
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
    int cx0 = x0 < pc->clip.x0 ? pc->clip.x0 : x0;
    int cx1 = x1 > pc->clip.x1 ? pc->clip.x1 : x1;
    if (cx0 >= cx1) return;
    uint32_t *p = pc->pixels + (size_t)y * pc->stride + cx0;
    for (int x = cx0; x < cx1; x++) *p++ = c;
}

static void vline(aether_plot_ctx_t *pc, int x, int y0, int y1, uint32_t c)
{
    if (x < pc->clip.x0 || x >= pc->clip.x1) return;
    if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
    int cy0 = y0 < pc->clip.y0 ? pc->clip.y0 : y0;
    int cy1 = y1 > pc->clip.y1 ? pc->clip.y1 : y1;
    for (int y = cy0; y < cy1; y++)
        pc->pixels[(size_t)y * pc->stride + x] = c;
}

/* Bresenham line — 1 px thick */
static void draw_line(aether_plot_ctx_t *pc,
                       int x0, int y0, int x1, int y1, uint32_t c)
{
    int dx = x1 - x0, dy = y1 - y0;
    if (dx == 0) { vline(pc, x0, y0, y1 + (dy >= 0 ? 1 : -1), c); return; }
    if (dy == 0) { hline(pc, x0, x1 + (dx >= 0 ? 1 : -1), y0, c); return; }
    int sx = dx > 0 ? 1 : -1, sy = dy > 0 ? 1 : -1;
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    int err = dx - dy;
    while (1) {
        put_pixel(pc, x0, y0, c);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 <  dx) { err += dx; y0 += sy; }
    }
}

/* ── font handle ─────────────────────────────────────────────────────────── */

static aether_font_t *g_pfont   = NULL;
static bool           g_ptried  = false;

static aether_font_t *get_pfont(void)
{
    if (g_ptried) return g_pfont;
    g_ptried = true;
    aether_font_load("/fonts/NotoSans-Regular.ttf", &g_pfont);
    return g_pfont;
}

/* plot_style_fixed (22:10) → screen pixels at 96 dpi */
static int pts_to_px(plot_style_fixed sz)
{
    int pts = plot_style_fixed_to_int(sz);
    if (pts <= 0) pts = 12;
    return pts * 96 / 72;
}

/* ── public init ─────────────────────────────────────────────────────────── */

void aether_plot_ctx_init(aether_plot_ctx_t *ctx,
                           uint32_t *pixels, int width, int height)
{
    ctx->pixels   = pixels;
    ctx->stride   = width;
    ctx->width    = width;
    ctx->height   = height;
    ctx->clip.x0  = 0;
    ctx->clip.y0  = 0;
    ctx->clip.x1  = width;
    ctx->clip.y1  = height;
}

/* ── plotter callbacks ───────────────────────────────────────────────────── */

static nserror aether_plot_clip(const struct redraw_context *ctx,
                                 const struct rect *clip)
{
    aether_plot_ctx_t *pc = ctx->priv;
    pc->clip.x0 = clip->x0 < 0        ? 0        : clip->x0;
    pc->clip.y0 = clip->y0 < 0        ? 0        : clip->y0;
    pc->clip.x1 = clip->x1 > pc->width  ? pc->width  : clip->x1;
    pc->clip.y1 = clip->y1 > pc->height ? pc->height : clip->y1;
    return NSERROR_OK;
}

/* ── arc ─────────────────────────────────────────────────────────────────── */
/* Midpoint circle.  angle1/angle2 are ignored for the Phase-7.7 MVP
 * (arcs appear only in scroll-bar decorations and similar minor elements). */
static nserror aether_plot_arc(const struct redraw_context *ctx,
                                const plot_style_t *pstyle,
                                int x, int y, int r,
                                int angle1, int angle2)
{
    (void)angle1; (void)angle2;
    aether_plot_ctx_t *pc = ctx->priv;
    if (ns_transparent(pstyle->fill_colour)) return NSERROR_OK;
    uint32_t c = ns_rgb(pstyle->fill_colour);

    int cx = 0, cy = r, d = 3 - 2 * r;
    while (cx <= cy) {
        put_pixel(pc, x + cx, y + cy, c);
        put_pixel(pc, x - cx, y + cy, c);
        put_pixel(pc, x + cx, y - cy, c);
        put_pixel(pc, x - cx, y - cy, c);
        put_pixel(pc, x + cy, y + cx, c);
        put_pixel(pc, x - cy, y + cx, c);
        put_pixel(pc, x + cy, y - cx, c);
        put_pixel(pc, x - cy, y - cx, c);
        if (d < 0) { d += 4 * cx + 6; }
        else       { d += 4 * (cx - cy) + 10; cy--; }
        cx++;
    }
    return NSERROR_OK;
}

/* ── disc ────────────────────────────────────────────────────────────────── */
static nserror aether_plot_disc(const struct redraw_context *ctx,
                                 const plot_style_t *pstyle,
                                 int x, int y, int r)
{
    aether_plot_ctx_t *pc = ctx->priv;

    if (pstyle->fill_type != PLOT_OP_TYPE_NONE &&
        !ns_transparent(pstyle->fill_colour)) {
        uint32_t fc = ns_rgb(pstyle->fill_colour);
        /* Scanline fill using circle equation */
        for (int dy = -r; dy <= r; dy++) {
            int span = (int)(__builtin_sqrt((double)(r*r - dy*dy)));
            hline(pc, x - span, x + span + 1, y + dy, fc);
        }
    }
    if (pstyle->stroke_type != PLOT_OP_TYPE_NONE &&
        !ns_transparent(pstyle->stroke_colour)) {
        /* Reuse arc for outline */
        plot_style_t tmp = *pstyle;
        tmp.fill_colour = pstyle->stroke_colour;
        aether_plot_arc(ctx, &tmp, x, y, r, 0, 360);
    }
    return NSERROR_OK;
}

/* ── line ────────────────────────────────────────────────────────────────── */
static nserror aether_plot_line(const struct redraw_context *ctx,
                                 const plot_style_t *pstyle,
                                 const struct rect *line)
{
    aether_plot_ctx_t *pc = ctx->priv;
    if (pstyle->stroke_type == PLOT_OP_TYPE_NONE) return NSERROR_OK;
    if (ns_transparent(pstyle->stroke_colour))    return NSERROR_OK;

    uint32_t c = ns_rgb(pstyle->stroke_colour);
    int sw = plot_style_fixed_to_int(pstyle->stroke_width);
    if (sw <= 0) sw = 1;

    if (sw == 1) {
        draw_line(pc, line->x0, line->y0, line->x1, line->y1, c);
        return NSERROR_OK;
    }

    /* Thick line: draw sw parallel 1-px lines */
    int half = sw / 2;
    int dx = line->x1 - line->x0, dy = line->y1 - line->y0;
    if (dx == 0) {
        fill_rect(pc, line->x0 - half, line->y0,
                      line->x1 + sw - half, line->y1, c);
    } else if (dy == 0) {
        fill_rect(pc, line->x0, line->y0 - half,
                      line->x1, line->y1 + sw - half, c);
    } else {
        for (int i = -half; i < sw - half; i++)
            draw_line(pc, line->x0 + i, line->y0,
                          line->x1 + i, line->y1, c);
    }
    return NSERROR_OK;
}

/* ── rectangle ───────────────────────────────────────────────────────────── */
static nserror aether_plot_rectangle(const struct redraw_context *ctx,
                                      const plot_style_t *pstyle,
                                      const struct rect *r)
{
    aether_plot_ctx_t *pc = ctx->priv;

    if (pstyle->fill_type != PLOT_OP_TYPE_NONE &&
        !ns_transparent(pstyle->fill_colour)) {
        fill_rect(pc, r->x0, r->y0, r->x1, r->y1,
                  ns_rgb(pstyle->fill_colour));
    }

    if (pstyle->stroke_type != PLOT_OP_TYPE_NONE &&
        !ns_transparent(pstyle->stroke_colour)) {
        uint32_t sc = ns_rgb(pstyle->stroke_colour);
        int sw = plot_style_fixed_to_int(pstyle->stroke_width);
        if (sw <= 0) sw = 1;
        /* top, bottom, left, right */
        fill_rect(pc, r->x0, r->y0,        r->x1,      r->y0 + sw, sc);
        fill_rect(pc, r->x0, r->y1 - sw,   r->x1,      r->y1,      sc);
        fill_rect(pc, r->x0, r->y0 + sw,   r->x0 + sw, r->y1 - sw, sc);
        fill_rect(pc, r->x1 - sw, r->y0 + sw, r->x1,  r->y1 - sw, sc);
    }

    return NSERROR_OK;
}

/* ── polygon ─────────────────────────────────────────────────────────────── */
/* Scanline fill — vertices in p[] as {x0,y0, x1,y1, ...}, n vertex count. */
static nserror aether_plot_polygon(const struct redraw_context *ctx,
                                    const plot_style_t *pstyle,
                                    const int *p, unsigned int n)
{
    aether_plot_ctx_t *pc = ctx->priv;
    if (pstyle->fill_type == PLOT_OP_TYPE_NONE) return NSERROR_OK;
    if (ns_transparent(pstyle->fill_colour))    return NSERROR_OK;
    if (n < 3) return NSERROR_OK;
    uint32_t fc = ns_rgb(pstyle->fill_colour);

    /* Find Y bounds */
    int ymin = p[1], ymax = p[1];
    for (unsigned i = 1; i < n; i++) {
        int y = p[i * 2 + 1];
        if (y < ymin) ymin = y;
        if (y > ymax) ymax = y;
    }
    /* Clip to buffer */
    if (ymin < pc->clip.y0) ymin = pc->clip.y0;
    if (ymax > pc->clip.y1) ymax = pc->clip.y1;

    /* Allocate intersection buffer on stack — limit n to 256 vertices */
    int xs[256];
    if (n > 256) n = 256;

    for (int y = ymin; y < ymax; y++) {
        int cnt = 0;
        for (unsigned i = 0; i < n; i++) {
            unsigned j = (i + 1) % n;
            int y0 = p[i * 2 + 1], y1 = p[j * 2 + 1];
            int x0 = p[i * 2],     x1 = p[j * 2];
            if ((y0 <= y && y1 > y) || (y1 <= y && y0 > y)) {
                /* Intersection x = x0 + (y - y0) * (x1-x0) / (y1-y0) */
                xs[cnt++] = x0 + (y - y0) * (x1 - x0) / (y1 - y0);
            }
        }
        /* Sort intersections */
        for (int a = 0; a < cnt - 1; a++)
            for (int b = a + 1; b < cnt; b++)
                if (xs[a] > xs[b]) { int t = xs[a]; xs[a] = xs[b]; xs[b] = t; }

        for (int a = 0; a + 1 < cnt; a += 2)
            hline(pc, xs[a], xs[a + 1], y, fc);
    }
    return NSERROR_OK;
}

/* ── path ────────────────────────────────────────────────────────────────── */
/* De Casteljau cubic bezier subdivision → line segments. */
static void bezier_subdivide(aether_plot_ctx_t *pc,
                              float ax, float ay, float bx, float by,
                              float cx, float cy, float dx, float dy,
                              uint32_t c, int depth)
{
    if (depth >= 6) {
        draw_line(pc, (int)ax, (int)ay, (int)dx, (int)dy, c);
        return;
    }
    float abx = (ax + bx) * 0.5f, aby = (ay + by) * 0.5f;
    float bcx = (bx + cx) * 0.5f, bcy = (by + cy) * 0.5f;
    float cdx = (cx + dx) * 0.5f, cdy = (cy + dy) * 0.5f;
    float abcx = (abx + bcx) * 0.5f, abcy = (aby + bcy) * 0.5f;
    float bcdx = (bcx + cdx) * 0.5f, bcdy = (bcy + cdy) * 0.5f;
    float mx = (abcx + bcdx) * 0.5f, my = (abcy + bcdy) * 0.5f;
    bezier_subdivide(pc, ax, ay, abx, aby, abcx, abcy, mx, my, c, depth + 1);
    bezier_subdivide(pc, mx, my, bcdx, bcdy, cdx, cdy, dx, dy, c, depth + 1);
}

static nserror aether_plot_path(const struct redraw_context *ctx,
                                 const plot_style_t *pstyle,
                                 const float *p, unsigned int n,
                                 const float transform[6])
{
    (void)transform; /* transform not applied in Phase-7.7 MVP */
    aether_plot_ctx_t *pc = ctx->priv;
    if (pstyle->stroke_type == PLOT_OP_TYPE_NONE &&
        pstyle->fill_type   == PLOT_OP_TYPE_NONE) return NSERROR_OK;

    uint32_t c = (!ns_transparent(pstyle->stroke_colour))
                     ? ns_rgb(pstyle->stroke_colour)
                     : ns_rgb(pstyle->fill_colour);

    float cx = 0.0f, cy = 0.0f, sx = 0.0f, sy = 0.0f;
    unsigned i = 0;
    while (i < n) {
        int cmd = (int)p[i];
        switch (cmd) {
        case PLOTTER_PATH_MOVE:
            if (i + 2 >= n) goto done;
            cx = p[i + 1]; cy = p[i + 2];
            sx = cx; sy = cy;
            i += 3; break;
        case PLOTTER_PATH_LINE:
            if (i + 2 >= n) goto done; {
            float nx = p[i + 1], ny = p[i + 2];
            draw_line(pc, (int)cx, (int)cy, (int)nx, (int)ny, c);
            cx = nx; cy = ny;
            i += 3; } break;
        case PLOTTER_PATH_BEZIER:
            if (i + 6 >= n) goto done; {
            float b1x = p[i + 1], b1y = p[i + 2];
            float b2x = p[i + 3], b2y = p[i + 4];
            float nx  = p[i + 5], ny  = p[i + 6];
            bezier_subdivide(pc, cx, cy, b1x, b1y, b2x, b2y, nx, ny, c, 0);
            cx = nx; cy = ny;
            i += 7; } break;
        case PLOTTER_PATH_CLOSE:
            draw_line(pc, (int)cx, (int)cy, (int)sx, (int)sy, c);
            cx = sx; cy = sy;
            i += 1; break;
        default:
            goto done;
        }
    }
done:
    return NSERROR_OK;
}

/* ── bitmap ──────────────────────────────────────────────────────────────── */
static nserror aether_plot_bitmap(const struct redraw_context *ctx,
                                   struct bitmap *bm_,
                                   int x, int y, int width, int height,
                                   colour bg, bitmap_flags_t flags)
{
    aether_plot_ctx_t *pc = ctx->priv;
    aether_bitmap_t *bm = (aether_bitmap_t *)bm_;
    if (!bm || !bm->data) return NSERROR_OK;

    uint32_t bg_rgb = ns_rgb(bg);
    int src_w = bm->width, src_h = bm->height;

    /* Tile ranges: single tile by default, extended if repeat flags set */
    int tile_x0 = 0, tile_y0 = 0, tile_x1 = width, tile_y1 = height;
    if (flags & BITMAPF_REPEAT_X) { tile_x0 = pc->clip.x0 - x; tile_x1 = pc->clip.x1 - x; }
    if (flags & BITMAPF_REPEAT_Y) { tile_y0 = pc->clip.y0 - y; tile_y1 = pc->clip.y1 - y; }

    for (int dy = tile_y0; dy < tile_y1; dy++) {
        int fy = y + dy;
        if (fy < pc->clip.y0 || fy >= pc->clip.y1) continue;

        /* Map dst y → src y (with repeat wrap) */
        int src_y = (dy % src_h + src_h) % src_h;
        const uint8_t *src_row = bm->data + (size_t)src_y * src_w * 4;

        for (int dx = tile_x0; dx < tile_x1; dx++) {
            int fx = x + dx;
            if (fx < pc->clip.x0 || fx >= pc->clip.x1) continue;

            int src_x = (dx % src_w + src_w) % src_w;
            const uint8_t *p4 = src_row + src_x * 4;
            uint8_t r = p4[0], g = p4[1], b = p4[2], a = p4[3];

            uint32_t dst = bm->opaque
                ? (uint32_t)((r << 16) | (g << 8) | b)
                : alpha_blend_rgba(r, g, b, a, bg_rgb);

            pc->pixels[(size_t)fy * pc->stride + fx] = dst;
        }
    }
    return NSERROR_OK;
}

/* ── text ────────────────────────────────────────────────────────────────── */
static nserror aether_plot_text(const struct redraw_context *ctx,
                                 const plot_font_style_t *fstyle,
                                 int x, int y,
                                 const char *text, size_t length)
{
    aether_plot_ctx_t *pc = ctx->priv;
    aether_font_t *font = get_pfont();

    /* NUL-terminate the slice */
    char buf[512];
    size_t n = length < sizeof(buf) - 1 ? length : sizeof(buf) - 1;
    memcpy(buf, text, n);
    buf[n] = '\0';

    int px = pts_to_px(fstyle->size);
    uint32_t fg = ns_rgb(fstyle->foreground);

    if (!font) {
        /* Fallback: nothing drawn without FreeType */
        return NSERROR_OK;
    }

    /* aether_font_draw writes into a uint32_t buffer using the full
     * buffer dims for clipping.  Pass pc->pixels so it draws in-place. */
    aether_font_draw(font, buf, px, fg,
                     pc->pixels, pc->stride, pc->width, pc->height,
                     x, y);
    return NSERROR_OK;
}

/* ── exported table ──────────────────────────────────────────────────────── */

const struct plotter_table aether_plotter_table = {
    .clip            = aether_plot_clip,
    .arc             = aether_plot_arc,
    .disc            = aether_plot_disc,
    .line            = aether_plot_line,
    .rectangle       = aether_plot_rectangle,
    .polygon         = aether_plot_polygon,
    .path            = aether_plot_path,
    .bitmap          = aether_plot_bitmap,
    .text            = aether_plot_text,
    .group_start     = NULL,   /* optional — not needed for direct FB rendering */
    .group_end       = NULL,   /* optional */
    .flush           = NULL,   /* must be NULL for frontend plotters */
    .option_knockout = true,   /* enable knockout optimisation */
};
