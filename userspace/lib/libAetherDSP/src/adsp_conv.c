/*
 * AetherOS — libAetherDSP: Overlap-Add Convolution (Phase 8.4)
 * File: userspace/lib/libAetherDSP/src/adsp_conv.c
 *
 * Frequency-domain convolution via Overlap-Add for long IRs.
 * FFT size = next power-of-2 ≥ (block_size + ir_len - 1).
 * IR spectrum pre-computed at creation time.
 *
 * Each process() call:
 *   1. Zero-pad input block to FFT size
 *   2. Forward FFT
 *   3. Complex multiply with IR spectrum
 *   4. Inverse FFT
 *   5. Overlap-add tail from previous block
 */

#include "adsp.h"
#include <stdlib.h>
#include <string.h>

struct adsp_conv_s {
    adsp_fft_t *fft;
    int         fft_size;
    int         block_size;
    int         ir_len;
    float      *ir_re;     /* pre-computed IR spectrum, fft_size floats */
    float      *ir_im;
    float      *buf_re;    /* working buffer re, fft_size floats         */
    float      *buf_im;    /* working buffer im, fft_size floats         */
    float      *overlap;   /* overlap-add accumulator, fft_size floats   */
};

/* ── Helpers ─────────────────────────────────────────────────────────── */

static int next_pow2(int x)
{
    int p = 1;
    while (p < x) p <<= 1;
    return p;
}

/* ── Creation / destruction ──────────────────────────────────────────── */

adsp_conv_t *adsp_conv_create(const float *ir, int ir_len, int block_size)
{
    if (!ir || ir_len <= 0 || block_size <= 0) return NULL;

    adsp_conv_t *c = (adsp_conv_t *)malloc(sizeof(adsp_conv_t));
    if (!c) return NULL;
    memset(c, 0, sizeof(*c));

    int min_size  = block_size + ir_len - 1;
    int fft_size  = next_pow2(min_size);
    if (fft_size < 64) fft_size = 64;

    c->fft        = adsp_fft_create(fft_size);
    c->fft_size   = fft_size;
    c->block_size = block_size;
    c->ir_len     = ir_len;

    c->ir_re  = (float *)malloc(sizeof(float) * fft_size);
    c->ir_im  = (float *)malloc(sizeof(float) * fft_size);
    c->buf_re = (float *)malloc(sizeof(float) * fft_size);
    c->buf_im = (float *)malloc(sizeof(float) * fft_size);
    c->overlap = (float *)malloc(sizeof(float) * fft_size);

    if (!c->fft || !c->ir_re || !c->ir_im ||
        !c->buf_re || !c->buf_im || !c->overlap) {
        adsp_conv_destroy(c);
        return NULL;
    }

    /* Pre-compute IR spectrum */
    memset(c->ir_re, 0, sizeof(float) * fft_size);
    memset(c->ir_im, 0, sizeof(float) * fft_size);
    memset(c->overlap, 0, sizeof(float) * fft_size);

    int copy_len = ir_len < fft_size ? ir_len : fft_size;
    for (int i = 0; i < copy_len; i++) c->ir_re[i] = ir[i];

    adsp_fft_forward(c->fft, c->ir_re, c->ir_im);
    return c;
}

void adsp_conv_destroy(adsp_conv_t *c)
{
    if (!c) return;
    adsp_fft_destroy(c->fft);
    free(c->ir_re);
    free(c->ir_im);
    free(c->buf_re);
    free(c->buf_im);
    free(c->overlap);
    free(c);
}

/* ── Processing ──────────────────────────────────────────────────────── */

void adsp_conv_process(adsp_conv_t *c,
                       float *out, const float *in, int n)
{
    int fft_size = c->fft_size;

    /* Clamp n to block_size */
    if (n > c->block_size) n = c->block_size;

    /* Load input into re, zero-pad im and tail */
    for (int i = 0; i < n; i++)         c->buf_re[i] = in[i];
    for (int i = n; i < fft_size; i++)  c->buf_re[i] = 0.0f;
    for (int i = 0; i < fft_size; i++) c->buf_im[i] = 0.0f;

    /* Forward FFT of input block */
    adsp_fft_forward(c->fft, c->buf_re, c->buf_im);

    /* Complex multiply: buf *= ir_spectrum */
    for (int i = 0; i < fft_size; i++) {
        float ar = c->buf_re[i], ai = c->buf_im[i];
        float br = c->ir_re[i],  bi = c->ir_im[i];
        c->buf_re[i] = ar * br - ai * bi;
        c->buf_im[i] = ar * bi + ai * br;
    }

    /* Inverse FFT */
    adsp_fft_inverse(c->fft, c->buf_re, c->buf_im);

    /* Overlap-add */
    for (int i = 0; i < fft_size; i++)
        c->overlap[i] += c->buf_re[i];

    /* Output first n samples */
    for (int i = 0; i < n; i++) out[i] = c->overlap[i];

    /* Shift overlap buffer left by n */
    int tail = fft_size - n;
    for (int i = 0; i < tail; i++) c->overlap[i] = c->overlap[i + n];
    for (int i = tail; i < fft_size; i++) c->overlap[i] = 0.0f;
}
