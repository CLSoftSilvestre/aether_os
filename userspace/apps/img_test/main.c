/*
 * img_test — Phase 7.1 integration test (Task 7.1.4)
 *
 * Decodes a PNG and JPEG from the FAT32 disk (/images/) using libpng and
 * libjpeg, verifies the decoded pixel values, and prints pass/fail.
 *
 * Test images (64×64, 4-quadrant pattern):
 *   TL = red (255,0,0)   TR = green (0,255,0)
 *   BL = blue (0,0,255)  BR = yellow (255,255,0)
 *
 * Generate on host: python3 scripts/gen_test_images.py
 * Rebuild disk:     scripts/make_disk.sh  (copies assets/images/ → /images/)
 * Run in AetherOS:  img_test
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <png.h>
#include <jpeglib.h>

#define PNG_PATH  "/images/test.png"
#define JPEG_PATH "/images/test.jpg"
#define IMG_W     64
#define IMG_H     64
#define TOL       22   /* JPEG lossy tolerance — ±22 per channel */

static int within(int a, int b) { return (a - b) >= -TOL && (a - b) <= TOL; }

/* ── PNG callbacks — avoids PNG_STDIO_SUPPORTED / png_jmpbuf dependency ─── */

static jmp_buf _png_err;

static void _png_error_fn(png_structp p, png_const_charp msg)
{
    (void)p;
    fprintf(stderr, "[png] error: %s\n", msg);
    longjmp(_png_err, 1);
}

static void _png_warn_fn(png_structp p, png_const_charp msg)
{
    (void)p;
    fprintf(stderr, "[png] warning: %s\n", msg);
}

static void _png_read_fn(png_structp p, png_bytep data, size_t len)
{
    FILE *fp = (FILE *)png_get_io_ptr(p);
    if (fread(data, 1, len, fp) != len)
        png_error(p, "fread short read");
}

/* ── PNG decode ────────────────────────────────────────────────────────── */

static int test_png(void)
{
    printf("[png] Opening %s ...\n", PNG_PATH);
    FILE *fp = fopen(PNG_PATH, "r");
    if (!fp) { fprintf(stderr, "[png] Cannot open %s\n", PNG_PATH); return -1; }

    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING,
                          NULL, _png_error_fn, _png_warn_fn);
    if (!png) { fclose(fp); return -1; }

    png_infop info = png_create_info_struct(png);
    if (!info) { png_destroy_read_struct(&png, NULL, NULL); fclose(fp); return -1; }

    if (setjmp(_png_err)) {
        fprintf(stderr, "[png] decode failed\n");
        png_destroy_read_struct(&png, &info, NULL);
        fclose(fp);
        return -1;
    }

    /* Explicit read callback — no PNG_STDIO_SUPPORTED needed */
    png_set_read_fn(png, fp, _png_read_fn);
    png_read_info(png, info);

    png_uint_32 w = png_get_image_width(png, info);
    png_uint_32 h = png_get_image_height(png, info);
    printf("[png] %ux%u  color_type=%d  bit_depth=%d\n",
           (unsigned)w, (unsigned)h,
           png_get_color_type(png, info),
           png_get_bit_depth(png, info));

    /* Normalise to 8-bit RGB */
    png_set_strip_16(png);
    png_set_packing(png);
    png_set_expand(png);
    png_set_strip_alpha(png);
    png_read_update_info(png, info);

    png_bytep *rows = malloc(h * sizeof(png_bytep));
    if (!rows) { png_destroy_read_struct(&png, &info, NULL); fclose(fp); return -1; }
    for (png_uint_32 y = 0; y < h; y++) rows[y] = malloc(w * 3);

    png_read_image(png, rows);
    png_read_end(png, NULL);
    png_destroy_read_struct(&png, &info, NULL);
    fclose(fp);

    if (w != IMG_W || h != IMG_H) {
        fprintf(stderr, "[png] Expected %dx%d, got %ux%u\n", IMG_W, IMG_H, (unsigned)w, (unsigned)h);
        for (png_uint_32 y = 0; y < h; y++) free(rows[y]);
        free(rows);
        return -1;
    }

    /* Check one pixel from each quadrant */
    struct { int y, x; int er, eg, eb; const char *name; } chk[] = {
        { 0,        0,        255,   0,   0, "TL red"    },
        { 0,        IMG_W/2,    0, 255,   0, "TR green"  },
        { IMG_H/2,  0,          0,   0, 255, "BL blue"   },
        { IMG_H/2,  IMG_W/2,  255, 255,   0, "BR yellow" },
    };
    int ok = 1;
    for (int i = 0; i < 4; i++) {
        unsigned char *p = &rows[chk[i].y][chk[i].x * 3];
        if (!within(p[0], chk[i].er) || !within(p[1], chk[i].eg) || !within(p[2], chk[i].eb)) {
            fprintf(stderr, "[png] %s: RGB(%d,%d,%d) != expected (~%d,~%d,~%d)\n",
                    chk[i].name, p[0], p[1], p[2], chk[i].er, chk[i].eg, chk[i].eb);
            ok = 0;
        } else {
            printf("[png] %s: RGB(%d,%d,%d) OK\n", chk[i].name, p[0], p[1], p[2]);
        }
    }

    for (png_uint_32 y = 0; y < h; y++) free(rows[y]);
    free(rows);
    printf("[png] %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : -1;
}

/* ── JPEG decode ───────────────────────────────────────────────────────── */

static int test_jpeg(void)
{
    printf("[jpeg] Opening %s ...\n", JPEG_PATH);
    FILE *fp = fopen(JPEG_PATH, "r");
    if (!fp) { fprintf(stderr, "[jpeg] Cannot open %s (skipping)\n", JPEG_PATH); return 0; }

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
    printf("[jpeg] %ux%u  components=%d\n", w, h, cinfo.output_components);

    if (w != IMG_W || h != IMG_H) {
        fprintf(stderr, "[jpeg] Expected %dx%d, got %ux%u\n", IMG_W, IMG_H, w, h);
        jpeg_destroy_decompress(&cinfo);
        fclose(fp);
        return -1;
    }

    JSAMPARRAY buf = (*cinfo.mem->alloc_sarray)(
        (j_common_ptr)&cinfo, JPOOL_IMAGE, w * 3, 1);

    unsigned char tl[3]={0}, tr[3]={0}, bl[3]={0}, br[3]={0};
    for (unsigned y = 0; y < h; y++) {
        jpeg_read_scanlines(&cinfo, buf, 1);
        if (y == 0)    { memcpy(tl, buf[0],             3); memcpy(tr, buf[0]+(w/2)*3, 3); }
        if (y == h/2)  { memcpy(bl, buf[0],             3); memcpy(br, buf[0]+(w/2)*3, 3); }
    }
    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    fclose(fp);

    struct { unsigned char *px; int er,eg,eb; const char *name; } chk[] = {
        { tl, 255,   0,   0, "TL red"    },
        { tr,   0, 255,   0, "TR green"  },
        { bl,   0,   0, 255, "BL blue"   },
        { br, 255, 255,   0, "BR yellow" },
    };
    int ok = 1;
    for (int i = 0; i < 4; i++) {
        int r=chk[i].px[0], g=chk[i].px[1], b=chk[i].px[2];
        if (!within(r,chk[i].er) || !within(g,chk[i].eg) || !within(b,chk[i].eb)) {
            fprintf(stderr, "[jpeg] %s: RGB(%d,%d,%d) != expected (~%d,~%d,~%d)\n",
                    chk[i].name, r, g, b, chk[i].er, chk[i].eg, chk[i].eb);
            ok = 0;
        } else {
            printf("[jpeg] %s: RGB(%d,%d,%d) OK\n", chk[i].name, r, g, b);
        }
    }

    printf("[jpeg] %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : -1;
}

/* ── main ─────────────────────────────────────────────────────────────── */

int main(void)
{
    printf("=== AetherOS Phase 7.1 image library integration test ===\n\n");
    int fail = 0;
    fail |= test_png();
    fail |= test_jpeg();
    if (fail == 0) printf("\n=== All tests PASSED ===\n");
    else           printf("\n=== Some tests FAILED ===\n");
    return fail ? 1 : 0;
}
