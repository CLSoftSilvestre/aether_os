/*
 * font_test — Phase 7.2 integration test (Task 7.2.5)
 *
 * Loads NotoSans-Regular.ttf from /fonts/ on the FAT32 disk, renders
 * "Hello, AetherOS!" at three sizes (12, 18, 24px) plus a non-Latin
 * sample (Greek: "Γεια!") into an off-screen pixel buffer, then verifies
 * that enough non-zero pixels were produced.
 *
 * Prerequisites on disk:
 *   /fonts/NotoSans-Regular.ttf   (scripts/fetch_fonts.sh + make_disk.sh)
 *
 * Run in AetherOS:  font_test
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "aether_font.h"

#define FONT_PATH  "/fonts/NotoSans-Regular.ttf"
#define BUF_W      512
#define BUF_H      200

/* Minimum non-zero pixels expected per rendered line (loose sanity check). */
#define MIN_NZ_PER_LINE  20

int main(void)
{
    printf("=== AetherOS Phase 7.2 FreeType font rendering test ===\n\n");

    if (aether_font_init() != 0) {
        fprintf(stderr, "[font] FreeType init failed\n");
        return 1;
    }
    printf("[font] FreeType initialized OK\n");

    aether_font_t *sans = NULL;
    if (aether_font_load(FONT_PATH, &sans) != 0) {
        fprintf(stderr, "[font] Cannot load %s\n", FONT_PATH);
        return 1;
    }
    printf("[font] Loaded %s\n", FONT_PATH);

    uint32_t *buf = calloc(BUF_W * BUF_H, sizeof(uint32_t));
    if (!buf) { fprintf(stderr, "[font] OOM\n"); return 1; }

    int ok = 1;

    /* Render "Hello, AetherOS!" at 12, 18, 24 px */
    static const int sizes[] = { 12, 18, 24 };
    int y = 30;
    for (int i = 0; i < 3; i++) {
        int adv = aether_font_draw(sans, "Hello, AetherOS!",
                                   sizes[i], 0xFFFFFF,
                                   buf, BUF_W, BUF_W, BUF_H, 10, y);
        if (adv <= 0) {
            fprintf(stderr, "[font] Render failed at size %dpx\n", sizes[i]);
            ok = 0;
        } else {
            printf("[font] %2dpx 'Hello, AetherOS!' advance=%d OK\n",
                   sizes[i], adv);
        }
        y += sizes[i] + 8;
    }

    /* Render Greek sample */
    int adv = aether_font_draw(sans, "\xCE\x93\xCE\xB5\xCE\xB9\xCE\xB1!", /* Γεια! */
                               18, 0xFFFFFF,
                               buf, BUF_W, BUF_W, BUF_H, 10, y);
    if (adv <= 0) {
        fprintf(stderr, "[font] Greek render failed\n");
        ok = 0;
    } else {
        printf("[font] 18px Greek 'Γεια!' advance=%d OK\n", adv);
    }

    /* Pixel coverage sanity check */
    int nz = 0;
    for (int i = 0; i < BUF_W * BUF_H; i++)
        if (buf[i]) nz++;

    printf("[font] Non-zero pixels: %d\n", nz);
    if (nz < MIN_NZ_PER_LINE * 4) {
        fprintf(stderr, "[font] Too few pixels rendered (expected >=%d)\n",
                MIN_NZ_PER_LINE * 4);
        ok = 0;
    }

    /* Measure width (should be > 0 and reasonable) */
    int w12 = aether_font_measure_width(sans, "Hello, AetherOS!", 12);
    int w24 = aether_font_measure_width(sans, "Hello, AetherOS!", 24);
    printf("[font] measure 12px=%d  24px=%d\n", w12, w24);
    if (w12 <= 0 || w24 <= w12) {
        fprintf(stderr, "[font] measure_width sanity failed\n");
        ok = 0;
    }

    free(buf);
    aether_font_free(sans);

    if (ok) printf("\n=== All tests PASSED ===\n");
    else    printf("\n=== Some tests FAILED ===\n");
    return ok ? 0 : 1;
}
