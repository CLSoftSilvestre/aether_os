/*
 * AetherOS — AetherView  (Phase 7.2 — Image Viewer)
 * File: userspace/apps/aether_view/main.c
 *
 * Displays JPEG, PNG and BMP image files using the Lumina glassmorphism UI.
 * Launched by the Files app when the user double-clicks an image file.
 *
 * Features:
 *   - Decode JPEG via libjpeg, PNG via libpng, BMP (24/32-bpp) inline
 *   - Scale-to-fit on open (nearest-neighbour, never upscales)
 *   - Zoom: discrete steps via +/− keys or toolbar buttons; Fit; 1:1
 *   - Pan: mouse drag or arrow keys (Shift = 4× step)
 *   - Checkerboard background (shows alpha-free regions)
 *   - Image info in toolbar: W×H px   ZZ%
 *   - Ctrl+W / Esc closes the window
 *
 * Usage:  aether_view [/path/to/image]
 */

#include <gfx.h>
#include <sys.h>
#include <input.h>
#include <widget.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <png.h>
#include <jpeglib.h>

/* ── Window geometry ──────────────────────────────────────────────────────── */

#define WIN_W      800
#define WIN_H      580
#define TITLE_H     32   /* glass titlebar */
#define TOOLBAR_H   36   /* zoom/info bar below titlebar */
#define VIEW_W     WIN_W
#define VIEW_H     (WIN_H - TITLE_H - TOOLBAR_H)   /* 512 */
#define CONT_W     WIN_W
#define CONT_H     (WIN_H - TITLE_H)               /* 548 */

/* ── Lumina glassmorphism colours ─────────────────────────────────────────── */

#define C_TOOLBAR       GFX_RGB( 24,  22,  46)
#define C_VIEW_BG       GFX_RGB( 20,  18,  36)
#define C_CHECKER_A     GFX_RGB( 38,  36,  60)
#define C_CHECKER_B     GFX_RGB( 28,  26,  48)
#define C_BTN_BG        GFX_RGB( 36,  32,  70)
#define C_GLASS_HIGH    GFX_RGB( 82,  70, 158)
#define C_GLASS_SPEC    GFX_RGB(190, 170, 255)
#define C_GLASS_EDGE    GFX_RGB(  8,   6,  16)
#define C_ERR_BG        GFX_RGB( 60,  20,  20)
#define C_ERR_BORDER    GFX_RGB(180,  60,  60)
#define C_ERR_TEXT      GFX_RGB(255, 160, 160)

/* ── Toolbar button layout ────────────────────────────────────────────────── */

#define BTN_H          24
#define BTN_ZOOM_M_X    8
#define BTN_ZOOM_M_W   56   /* "- Zoom" */
#define BTN_ZOOM_P_X   70
#define BTN_ZOOM_P_W   56   /* "+ Zoom" */
#define BTN_FIT_X     132
#define BTN_FIT_W      48   /* " Fit  " */
#define BTN_1TO1_X    186
#define BTN_1TO1_W     48   /* " 1:1  " */
#define INFO_X        244

/* ── Zoom levels ──────────────────────────────────────────────────────────── */

static const float ZOOM_LEVELS[] = {
    0.0625f, 0.125f, 0.25f, 0.333f, 0.5f,
    0.667f, 1.0f, 1.5f, 2.0f, 3.0f, 4.0f, 8.0f
};
#define ZOOM_COUNT      12
#define ZOOM_1TO1_IDX    6   /* index where ZOOM_LEVELS[i] == 1.0f */

/* ── Image state ──────────────────────────────────────────────────────────── */

static unsigned *g_pixels = (void *)0;   /* decoded XRGB8888, malloc'd */
static unsigned  g_img_w;
static unsigned  g_img_h;
static char      g_img_name[64];         /* basename for titlebar */
static char      g_err_msg[128];         /* decode error; empty = none */

/* ── View transform ───────────────────────────────────────────────────────── */

static int   g_zoom_idx  = ZOOM_1TO1_IDX;
static float g_zoom      = 1.0f;
static int   g_pan_x     = 0;            /* screen offset of image left edge */
static int   g_pan_y     = 0;            /* screen offset of image top edge  */

/* Mouse-drag tracking (absolute screen coords) */
static int   g_drag      = 0;
static int   g_drag_sx, g_drag_sy;      /* screen position at drag start */
static int   g_drag_px,  g_drag_py;     /* pan values at drag start */

/* ── Render buffer ────────────────────────────────────────────────────────── */

static unsigned *g_view_buf = (void *)0; /* malloc'd VIEW_W×VIEW_H */

/* ── Window state ─────────────────────────────────────────────────────────── */

static int  g_win_x  = 80;
static int  g_win_y  = 60;
static long g_win_id = -1;

static widget_ctx_t g_ctx;

/* ── Widget tree ──────────────────────────────────────────────────────────── */

static widget_t g_root;
static widget_t g_toolbar;
static widget_t g_canvas;

/* Absolute screen origin saved during draw_fn; used by event_fn */
static int g_tb_ax, g_tb_ay;
static int g_cv_ax, g_cv_ay;

/* ── Forward declarations ─────────────────────────────────────────────────── */

static void zoom_fit(void);
static void zoom_set(int idx, int pivot_sx, int pivot_sy);
static void render_image(void);
static void draw_frame(void);
static void update_title(void);

/* ── Basename helper ─────────────────────────────────────────────────────── */

static void extract_basename(const char *path, char *out, int outsz)
{
    const char *last = path;
    for (const char *p = path; *p; p++)
        if (*p == '/') last = p + 1;
    int i = 0;
    while (i < outsz - 1 && last[i]) { out[i] = last[i]; i++; }
    out[i] = '\0';
}

/* ── BMP decoder (24 / 32-bpp uncompressed) ─────────────────────────────── */

static int decode_bmp(FILE *fp, unsigned **out_px,
                      unsigned *out_w, unsigned *out_h)
{
    unsigned char hdr[54];
    if (fread(hdr, 1, 54, fp) < 54) return -1;
    if (hdr[0] != 'B' || hdr[1] != 'M') return -1;

    unsigned data_off =
        (unsigned)hdr[10] | ((unsigned)hdr[11] << 8) |
        ((unsigned)hdr[12] << 16) | ((unsigned)hdr[13] << 24);

    int w = (int)((unsigned)hdr[18] | ((unsigned)hdr[19] << 8) |
                  ((unsigned)hdr[20] << 16) | ((unsigned)hdr[21] << 24));
    int h = (int)((unsigned)hdr[22] | ((unsigned)hdr[23] << 8) |
                  ((unsigned)hdr[24] << 16) | ((unsigned)hdr[25] << 24));
    unsigned bpp   = (unsigned)hdr[28] | ((unsigned)hdr[29] << 8);
    unsigned compr = (unsigned)hdr[30] | ((unsigned)hdr[31] << 8) |
                     ((unsigned)hdr[32] << 16) | ((unsigned)hdr[33] << 24);

    if (bpp != 24 && bpp != 32) return -1;
    /* BI_RGB=0 always OK; BI_BITFIELDS=3 is valid for 32-bpp (standard XRGB masks) */
    if (compr != 0 && !(compr == 3 && bpp == 32)) return -1;

    int flip = (h > 0); /* positive height = rows stored bottom-up */
    if (h < 0) h = -h;
    if (w <= 0 || h <= 0 || w > 16384 || h > 16384) return -1;

    /* Skip any colour-table / padding bytes before pixel data */
    unsigned pos = 54;
    unsigned char skip[64];
    while (pos < data_off) {
        unsigned n = data_off - pos;
        if (n > 64) n = 64;
        if (fread(skip, 1, n, fp) != n) return -1;
        pos += n;
    }

    int bytes_pp  = (int)(bpp / 8);
    int row_bytes = w * bytes_pp;
    int row_stride = (row_bytes + 3) & ~3; /* DWORD aligned */

    unsigned *px = (unsigned *)malloc((unsigned)(w * h) * sizeof(unsigned));
    if (!px) return -1;
    unsigned char *row_buf = (unsigned char *)malloc((unsigned)row_stride);
    if (!row_buf) { free(px); return -1; }

    for (int y = 0; y < h; y++) {
        if ((int)fread(row_buf, 1, (unsigned)row_stride, fp) < row_stride) {
            free(row_buf); free(px); return -1;
        }
        int dst_y = flip ? (h - 1 - y) : y;
        unsigned *dst = px + dst_y * w;
        for (int x = 0; x < w; x++) {
            unsigned char *p = row_buf + x * bytes_pp;
            /* BMP stores BGR; GFX_RGB expects R,G,B */
            dst[x] = GFX_RGB(p[2], p[1], p[0]);
        }
    }
    free(row_buf);
    *out_px = px;
    *out_w  = (unsigned)w;
    *out_h  = (unsigned)h;
    return 0;
}

/* ── PNG decoder ─────────────────────────────────────────────────────────── */

static jmp_buf g_png_err;

static void _png_error_fn(png_structp p, png_const_charp msg)
{
    (void)p; (void)msg;
    longjmp(g_png_err, 1);
}
static void _png_warn_fn(png_structp p, png_const_charp msg)
{
    (void)p; (void)msg;
}
static void _png_read_fn(png_structp p, png_bytep data, size_t len)
{
    FILE *fp = (FILE *)png_get_io_ptr(p);
    if (fread(data, 1, len, fp) != len)
        png_error(p, "fread");
}

static int decode_png(FILE *fp, unsigned **out_px,
                      unsigned *out_w, unsigned *out_h)
{
    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING,
                          NULL, _png_error_fn, _png_warn_fn);
    if (!png) return -1;

    png_infop info = png_create_info_struct(png);
    if (!info) { png_destroy_read_struct(&png, NULL, NULL); return -1; }

    if (setjmp(g_png_err)) {
        png_destroy_read_struct(&png, &info, NULL);
        return -1;
    }

    png_set_read_fn(png, fp, _png_read_fn);
    png_read_info(png, info);

    unsigned w = png_get_image_width(png, info);
    unsigned h = png_get_image_height(png, info);

    /* Normalise to 8-bit RGB (strips alpha, 16-bit depth, palette) */
    png_set_strip_16(png);
    png_set_packing(png);
    png_set_expand(png);
    png_set_strip_alpha(png);
    png_read_update_info(png, info);

    png_bytep *rows = (png_bytep *)malloc(h * sizeof(png_bytep));
    if (!rows) { png_destroy_read_struct(&png, &info, NULL); return -1; }
    for (unsigned y = 0; y < h; y++)
        rows[y] = (png_bytep)malloc(w * 3);

    png_read_image(png, rows);
    png_read_end(png, NULL);
    png_destroy_read_struct(&png, &info, NULL);

    unsigned *px = (unsigned *)malloc(w * h * sizeof(unsigned));
    if (!px) {
        for (unsigned y = 0; y < h; y++) free(rows[y]);
        free(rows);
        return -1;
    }

    for (unsigned y = 0; y < h; y++) {
        for (unsigned x = 0; x < w; x++) {
            unsigned r = rows[y][x * 3 + 0];
            unsigned g = rows[y][x * 3 + 1];
            unsigned b = rows[y][x * 3 + 2];
            px[y * w + x] = GFX_RGB(r, g, b);
        }
        free(rows[y]);
    }
    free(rows);

    *out_px = px;
    *out_w  = w;
    *out_h  = h;
    return 0;
}

/* ── JPEG decoder ────────────────────────────────────────────────────────── */

static int decode_jpeg(FILE *fp, unsigned **out_px,
                       unsigned *out_w, unsigned *out_h)
{
    struct jpeg_decompress_struct cinfo;
    struct jpeg_error_mgr jerr;
    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_decompress(&cinfo);
    jpeg_stdio_src(&cinfo, fp);
    jpeg_read_header(&cinfo, TRUE);
    cinfo.out_color_space = JCS_RGB;
    jpeg_start_decompress(&cinfo);

    unsigned w = cinfo.output_width;
    unsigned h = cinfo.output_height;

    unsigned *px = (unsigned *)malloc(w * h * sizeof(unsigned));
    if (!px) { jpeg_destroy_decompress(&cinfo); return -1; }

    JSAMPARRAY buf = (*cinfo.mem->alloc_sarray)(
        (j_common_ptr)&cinfo, JPOOL_IMAGE, w * 3, 1);

    for (unsigned y = 0; y < h; y++) {
        jpeg_read_scanlines(&cinfo, buf, 1);
        for (unsigned x = 0; x < w; x++) {
            unsigned r = buf[0][x * 3 + 0];
            unsigned g = buf[0][x * 3 + 1];
            unsigned b = buf[0][x * 3 + 2];
            px[y * w + x] = GFX_RGB(r, g, b);
        }
    }

    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);

    *out_px = px;
    *out_w  = w;
    *out_h  = h;
    return 0;
}

/* ── Extension detection helpers ─────────────────────────────────────────── */

static int ext_eq(const char *path, int n, const char *ext, int extlen)
{
    if (n < extlen + 1) return 0;
    if (path[n - extlen - 1] != '.') return 0;
    for (int i = 0; i < extlen; i++) {
        char c = path[n - extlen + i];
        char e = ext[i];
        if (c >= 'A' && c <= 'Z') c += 32; /* tolower */
        if (c != e) return 0;
    }
    return 1;
}

/* ── Load image from path ────────────────────────────────────────────────── */

static void load_image(const char *path)
{
    if (g_pixels) { free(g_pixels); g_pixels = (void *)0; }
    g_img_w = g_img_h = 0;
    g_err_msg[0] = '\0';

    extract_basename(path, g_img_name, sizeof(g_img_name));

    int n = (int)strlen(path);
    int is_bmp  = ext_eq(path, n, "bmp",  3);
    int is_png  = ext_eq(path, n, "png",  3);
    int is_jpg  = ext_eq(path, n, "jpg",  3) || ext_eq(path, n, "jpeg", 4);

    FILE *fp = fopen(path, "r");
    if (!fp) {
        snprintf(g_err_msg, sizeof(g_err_msg), "Cannot open: %s", g_img_name);
        return;
    }

    int ret = -1;
    if      (is_bmp) ret = decode_bmp (fp, &g_pixels, &g_img_w, &g_img_h);
    else if (is_png) ret = decode_png (fp, &g_pixels, &g_img_w, &g_img_h);
    else if (is_jpg) ret = decode_jpeg(fp, &g_pixels, &g_img_w, &g_img_h);
    else             snprintf(g_err_msg, sizeof(g_err_msg), "Unsupported format");

    fclose(fp);

    if (ret < 0 && !g_err_msg[0])
        snprintf(g_err_msg, sizeof(g_err_msg), "Decode failed: %s", g_img_name);

    if (ret == 0)
        zoom_fit();
    else {
        g_zoom_idx = ZOOM_1TO1_IDX;
        g_zoom     = 1.0f;
        g_pan_x    = g_pan_y = 0;
    }
}

/* ── Zoom helpers ────────────────────────────────────────────────────────── */

static void zoom_fit(void)
{
    if (!g_pixels || g_img_w == 0 || g_img_h == 0) return;

    float zx = (float)(VIEW_W - 16) / (float)g_img_w;
    float zy = (float)(VIEW_H - 16) / (float)g_img_h;
    float z  = zx < zy ? zx : zy;
    if (z > 1.0f) z = 1.0f;  /* never upscale in fit mode */

    g_zoom = z;

    /* Find highest discrete zoom level that doesn't exceed z */
    g_zoom_idx = 0;
    for (int i = 0; i < ZOOM_COUNT; i++) {
        if (ZOOM_LEVELS[i] <= z + 0.001f) g_zoom_idx = i;
    }

    /* Centre the image */
    g_pan_x = (VIEW_W - (int)(g_zoom * (float)g_img_w)) / 2;
    g_pan_y = (VIEW_H - (int)(g_zoom * (float)g_img_h)) / 2;
}

/*
 * Zoom to ZOOM_LEVELS[idx] keeping the screen point (pivot_sx, pivot_sy)
 * fixed over the same image pixel before and after the zoom.
 */
static void zoom_set(int idx, int pivot_sx, int pivot_sy)
{
    if (idx < 0) idx = 0;
    if (idx >= ZOOM_COUNT) idx = ZOOM_COUNT - 1;

    float old_z = g_zoom;
    float new_z = ZOOM_LEVELS[idx];

    float img_x = (float)(pivot_sx - g_pan_x) / old_z;
    float img_y = (float)(pivot_sy - g_pan_y) / old_z;

    g_zoom_idx = idx;
    g_zoom     = new_z;
    g_pan_x    = pivot_sx - (int)(new_z * img_x);
    g_pan_y    = pivot_sy - (int)(new_z * img_y);

    widget_invalidate(&g_canvas);
    widget_invalidate(&g_toolbar);
}

/* ── Render image into g_view_buf, then blit ─────────────────────────────── */

static void render_image(void)
{
    /* Checkerboard background */
    for (int y = 0; y < VIEW_H; y++) {
        unsigned *row = g_view_buf + y * VIEW_W;
        for (int x = 0; x < VIEW_W; x++)
            row[x] = ((x >> 4) ^ (y >> 4)) & 1 ? C_CHECKER_A : C_CHECKER_B;
    }

    if (!g_pixels) return;

    int img_w = (int)g_img_w;
    int img_h = (int)g_img_h;
    float inv_zoom = 1.0f / g_zoom;

    for (int sy = 0; sy < VIEW_H; sy++) {
        float fy = (float)(sy - g_pan_y) * inv_zoom;
        if (fy < 0.0f || fy >= (float)img_h) continue;
        int iy = (int)fy;
        const unsigned *src_row = g_pixels + iy * img_w;
        unsigned       *dst_row = g_view_buf + sy * VIEW_W;

        for (int sx = 0; sx < VIEW_W; sx++) {
            float fx = (float)(sx - g_pan_x) * inv_zoom;
            if (fx < 0.0f || fx >= (float)img_w) continue;
            dst_row[sx] = src_row[(int)fx];
        }
    }
}

/* ── Info string for toolbar ─────────────────────────────────────────────── */

static void build_info(char *buf, int sz)
{
    if (!g_pixels) {
        if (g_err_msg[0])
            snprintf(buf, (size_t)sz, "  %s", g_err_msg);
        else
            snprintf(buf, (size_t)sz, "  No image loaded");
        return;
    }
    int pct = (int)(g_zoom * 100.0f + 0.5f);
    snprintf(buf, (size_t)sz, "  %u \xc3\x97 %u px    %d%%",
             g_img_w, g_img_h, pct);
}

/* ── Toolbar draw function ───────────────────────────────────────────────── */

static void draw_toolbar(widget_t *w, int ax, int ay)
{
    (void)w;
    g_tb_ax = ax; g_tb_ay = ay;

    /* Glass strip */
    gfx_fill((unsigned)ax, (unsigned)ay, WIN_W, TOOLBAR_H, C_TOOLBAR);
    gfx_hline((unsigned)ax, (unsigned)ay, WIN_W, C_GLASS_SPEC);
    gfx_fill((unsigned)ax, (unsigned)(ay + 1), WIN_W, 2u, C_GLASS_HIGH);
    gfx_hline((unsigned)ax, (unsigned)(ay + TOOLBAR_H - 1), WIN_W, C_GLASS_EDGE);

    int by = ay + (TOOLBAR_H - BTN_H) / 2;

    /* Helper: draw one glass toolbar button */
    struct { int x; int bw; const char *lbl; } btns[] = {
        { BTN_ZOOM_M_X, BTN_ZOOM_M_W, "- Zoom" },
        { BTN_ZOOM_P_X, BTN_ZOOM_P_W, "+ Zoom" },
        { BTN_FIT_X,    BTN_FIT_W,    " Fit " },
        { BTN_1TO1_X,   BTN_1TO1_W,   " 1:1 " },
    };
    for (int i = 0; i < 4; i++) {
        int bx = ax + btns[i].x;
        gfx_fill((unsigned)bx, (unsigned)by, (unsigned)btns[i].bw, BTN_H, C_BTN_BG);
        gfx_hline((unsigned)bx, (unsigned)by, (unsigned)btns[i].bw, C_GLASS_HIGH);
        gfx_rect((unsigned)bx, (unsigned)by, (unsigned)btns[i].bw, BTN_H,
                 GFX_RGB(55, 48, 100));
        int lw = (int)strlen(btns[i].lbl) * 8;
        int tx = bx + (btns[i].bw - lw) / 2;
        int ty = by + (BTN_H - 8) / 2;
        gfx_text((unsigned)tx, (unsigned)ty, btns[i].lbl, C_TEXT, C_BTN_BG);
    }

    /* Info label */
    char info[80];
    build_info(info, sizeof(info));
    int iy = ay + (TOOLBAR_H - 8) / 2;
    gfx_text((unsigned)(ax + INFO_X), (unsigned)iy, info, C_TEXT_DIM, C_TOOLBAR);
}

/* ── Canvas draw function ────────────────────────────────────────────────── */

static void draw_canvas(widget_t *w, int ax, int ay)
{
    (void)w;
    g_cv_ax = ax; g_cv_ay = ay;

    render_image();
    gfx_raw_blit(g_view_buf, VIEW_W, ax, ay, VIEW_W, VIEW_H);

    /* Error overlay */
    if (g_err_msg[0]) {
        unsigned ew = (unsigned)(strlen(g_err_msg) * 8 + 32);
        unsigned eh = 32u;
        unsigned ex = (unsigned)ax + (VIEW_W - (int)ew) / 2;
        unsigned ey = (unsigned)ay + (VIEW_H - (int)eh) / 2;
        gfx_fill_rounded(ex, ey, ew, eh, 6u, C_ERR_BG);
        gfx_rect_rounded(ex, ey, ew, eh, 6u, C_ERR_BORDER);
        gfx_text(ex + 16u, ey + 12u, g_err_msg, C_ERR_TEXT, C_ERR_BG);
    }
}

/* ── Toolbar event function ──────────────────────────────────────────────── */

static int toolbar_event(widget_t *w, const widget_event_t *ev)
{
    (void)w;
    if (ev->type != WEV_MOUSE_DOWN) return 0;

    int lx = ev->mx - g_tb_ax;
    int ly = ev->my - g_tb_ay;
    int by_start = (TOOLBAR_H - BTN_H) / 2;
    if (ly < by_start || ly >= by_start + BTN_H) return 0;

    struct { int x; int bw; } btns[4] = {
        { BTN_ZOOM_M_X, BTN_ZOOM_M_W },
        { BTN_ZOOM_P_X, BTN_ZOOM_P_W },
        { BTN_FIT_X,    BTN_FIT_W    },
        { BTN_1TO1_X,   BTN_1TO1_W   },
    };
    int cx = VIEW_W / 2, cy = VIEW_H / 2;
    for (int i = 0; i < 4; i++) {
        if (lx >= btns[i].x && lx < btns[i].x + btns[i].bw) {
            switch (i) {
            case 0: zoom_set(g_zoom_idx - 1, cx, cy); break;
            case 1: zoom_set(g_zoom_idx + 1, cx, cy); break;
            case 2:
                zoom_fit();
                widget_invalidate(&g_canvas);
                widget_invalidate(&g_toolbar);
                break;
            case 3: zoom_set(ZOOM_1TO1_IDX, cx, cy); break;
            }
            return 1;
        }
    }
    return 0;
}

/* ── Canvas event function ───────────────────────────────────────────────── */

static int canvas_event(widget_t *w, const widget_event_t *ev)
{
    (void)w;

    switch (ev->type) {

    case WEV_MOUSE_DOWN:
        g_drag    = 1;
        g_drag_sx = ev->mx; g_drag_sy = ev->my;
        g_drag_px = g_pan_x; g_drag_py = g_pan_y;
        return 1;

    case WEV_MOUSE_UP:
        g_drag = 0;
        return 1;

    case WEV_MOUSE_MOVE:
        if (g_drag) {
            g_pan_x = g_drag_px + (ev->mx - g_drag_sx);
            g_pan_y = g_drag_py + (ev->my - g_drag_sy);
            widget_invalidate(w);
        }
        return 1;

    case WEV_KEY_DOWN: {
        keycode_t    kc   = ev->keycode;
        unsigned int mods = ev->modifiers;
        int step = (mods & MOD_SHIFT) ? 128 : 32;
        int cx = VIEW_W / 2, cy = VIEW_H / 2;

        switch (kc) {
        /* Zoom in */
        case KEY_EQUALS:
            zoom_set(g_zoom_idx + 1, cx, cy); return 1;
        /* Zoom out */
        case KEY_MINUS:
            zoom_set(g_zoom_idx - 1, cx, cy); return 1;
        /* Fit */
        case KEY_0:
            zoom_fit();
            widget_invalidate(w);
            widget_invalidate(&g_toolbar);
            return 1;
        /* 1:1 */
        case KEY_1:
            zoom_set(ZOOM_1TO1_IDX, cx, cy); return 1;
        /* Pan — arrow keys move the image (opposite to scroll direction) */
        case KEY_UP:    g_pan_y += step; widget_invalidate(w); return 1;
        case KEY_DOWN:  g_pan_y -= step; widget_invalidate(w); return 1;
        case KEY_LEFT:  g_pan_x += step; widget_invalidate(w); return 1;
        case KEY_RIGHT: g_pan_x -= step; widget_invalidate(w); return 1;
        /* Close */
        case KEY_W:
            if (mods & MOD_CTRL) { g_ctx.running = 0; return 1; }
            break;
        case KEY_ESC:
            g_ctx.running = 0; return 1;
        default: break;
        }
        break;
    }

    default: break;
    }
    return 0;
}

/* ── Window chrome ───────────────────────────────────────────────────────── */

static void draw_frame(void)
{
    char title[160];
    if (g_img_name[0])
        snprintf(title, sizeof(title), "AetherView  \xe2\x80\x94  %s", g_img_name);
    else
        snprintf(title, sizeof(title), "AetherView");
    gfx_glass_window_frame(g_win_x, g_win_y, WIN_W, WIN_H, TITLE_H, title, 0);
}

static void update_title(void)
{
    char title[160];
    if (g_img_name[0])
        snprintf(title, sizeof(title), "AetherView  \xe2\x80\x94  %s", g_img_name);
    else
        snprintf(title, sizeof(title), "AetherView");
    unsigned tx = (unsigned)(g_win_x + 34);
    unsigned tw = WIN_W - 44u;
    gfx_fill(tx, (unsigned)g_win_y, tw, TITLE_H, C_TITLEBAR);
    gfx_hline(tx, (unsigned)g_win_y, tw, GFX_RGB(90, 84, 148));
    gfx_fill(tx, (unsigned)(g_win_y + 1), tw, 2u, GFX_RGB(60, 56, 100));
    gfx_text_center_transparent((unsigned)g_win_x, WIN_W,
                                (unsigned)(g_win_y + (TITLE_H - 16) / 2),
                                title, C_TEXT);
}

static void on_reposition(void *ud)
{
    (void)ud;
    draw_frame();
}

/* ── Build widget tree ───────────────────────────────────────────────────── */

static void build_ui(void)
{
    widget_init_panel(&g_root, 0, 0, CONT_W, CONT_H, C_WIN_BG);

    widget_init_panel(&g_toolbar, 0, 0, WIN_W, TOOLBAR_H, C_TOOLBAR);
    g_toolbar.draw_fn  = draw_toolbar;
    g_toolbar.event_fn = toolbar_event;

    widget_init_panel(&g_canvas, 0, TOOLBAR_H, VIEW_W, VIEW_H, C_VIEW_BG);
    g_canvas.draw_fn   = draw_canvas;
    g_canvas.event_fn  = canvas_event;
    g_canvas.focusable = 1;

    widget_add_child(&g_root, &g_toolbar);
    widget_add_child(&g_root, &g_canvas);

    widget_set_focused(&g_canvas);
}

/* ── Main ────────────────────────────────────────────────────────────────── */

int main(int argc, const char *const *argv)
{
    g_view_buf = (unsigned *)malloc(VIEW_W * VIEW_H * sizeof(unsigned));
    if (!g_view_buf) return 1;

    gfx_init();

    g_win_id = sys_wm_register(g_win_x, g_win_y, WIN_W, WIN_H, "AetherView");
    sys_wm_focus_set(sys_getpid());

    draw_frame();
    build_ui();

    if (argc >= 2 && argv[1] && argv[1][0]) {
        load_image(argv[1]);
        update_title();
        widget_invalidate(&g_canvas);
        widget_invalidate(&g_toolbar);
    }

    /* Re-assert focus after image decode: the files app's event loop may have
     * reclaimed focus during the decode, pushing our window behind it. */
    sys_wm_focus_set(sys_getpid());

    g_ctx.win_x         = &g_win_x;
    g_ctx.win_y         = &g_win_y;
    g_ctx.content_dx    = 0;
    g_ctx.content_dy    = TITLE_H;
    g_ctx.win_id        = (int)g_win_id;
    g_ctx.win_w         = WIN_W;
    g_ctx.win_h         = WIN_H;
    g_ctx.on_reposition = on_reposition;
    g_ctx.userdata      = (void *)0;
    g_ctx.running       = 1;

    widget_run(&g_root, &g_ctx);

    if (g_pixels) free(g_pixels);
    free(g_view_buf);
    sys_wm_request_close(g_win_id);
    return 0;
}
