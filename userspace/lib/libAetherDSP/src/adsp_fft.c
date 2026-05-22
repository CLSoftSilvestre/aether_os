/*
 * AetherOS — libAetherDSP: Radix-2 DIT FFT (Phase 8.4)
 * File: userspace/lib/libAetherDSP/src/adsp_fft.c
 *
 * Cooley-Tukey radix-2 decimation-in-time FFT.
 * Separate real/imaginary arrays; in-place butterfly.
 * Twiddle factors pre-computed at plan creation.
 * NEON: butterfly pairs interleaved where possible.
 *
 * Supports sizes: 64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384, 32768, 65536
 */

#include "adsp.h"
#include <stdlib.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

struct adsp_fft_s {
    int    size;
    int    log2n;
    float *wr;   /* cos twiddles, size/2 entries */
    float *wi;   /* sin twiddles, size/2 entries */
    int   *br;   /* bit-reversal permutation, size entries */
};

/* ── Bit-reversal permutation ────────────────────────────────────────── */

static void compute_bitrev(int *br, int n, int log2n)
{
    br[0] = 0;
    for (int i = 1; i < n; i++) {
        int x = i, r = 0;
        for (int b = 0; b < log2n; b++) {
            r = (r << 1) | (x & 1);
            x >>= 1;
        }
        br[i] = r;
    }
}

/* ── Plan creation ───────────────────────────────────────────────────── */

adsp_fft_t *adsp_fft_create(int size)
{
    /* Validate power-of-2, 64 ≤ size ≤ 65536 */
    if (size < 64 || size > 65536 || (size & (size - 1)) != 0)
        return NULL;

    adsp_fft_t *fft = (adsp_fft_t *)malloc(sizeof(adsp_fft_t));
    if (!fft) return NULL;

    fft->size  = size;
    fft->log2n = 0;
    for (int s = size; s > 1; s >>= 1) fft->log2n++;

    fft->wr = (float *)malloc(sizeof(float) * (size / 2));
    fft->wi = (float *)malloc(sizeof(float) * (size / 2));
    fft->br = (int   *)malloc(sizeof(int)   * size);

    if (!fft->wr || !fft->wi || !fft->br) {
        adsp_fft_destroy(fft);
        return NULL;
    }

    /* Twiddle: W_k = e^(-j2π k/N), k = 0 .. N/2-1 */
    for (int k = 0; k < size / 2; k++) {
        double angle = -2.0 * M_PI * k / size;
        fft->wr[k] = (float)cos(angle);
        fft->wi[k] = (float)sin(angle);
    }

    compute_bitrev(fft->br, size, fft->log2n);
    return fft;
}

void adsp_fft_destroy(adsp_fft_t *fft)
{
    if (!fft) return;
    free(fft->wr);
    free(fft->wi);
    free(fft->br);
    free(fft);
}

/* ── Bit-reversal shuffle ────────────────────────────────────────────── */

static void bitrev_shuffle(float *re, float *im, const int *br, int n)
{
    for (int i = 0; i < n; i++) {
        int j = br[i];
        if (j > i) {
            float tr = re[i]; re[i] = re[j]; re[j] = tr;
            float ti = im[i]; im[i] = im[j]; im[j] = ti;
        }
    }
}

/* ── Core DIT butterfly pass ─────────────────────────────────────────── */

static void fft_pass(float *re, float *im,
                     const float *wr, const float *wi,
                     int n, int inverse)
{
    float wi_sign = inverse ? -1.0f : 1.0f;

    for (int s = 2; s <= n; s <<= 1) {
        int half  = s >> 1;
        int step  = n / s;        /* twiddle stride */

        for (int k = 0; k < n; k += s) {
            for (int j = 0; j < half; j++) {
                int tw = j * step;
                float twR =  wr[tw];
                float twI =  wi_sign * wi[tw];

                float *er = &re[k + j];
                float *ei = &im[k + j];
                float *or_ = &re[k + j + half];
                float *oi  = &im[k + j + half];

                float tr = twR * (*or_) - twI * (*oi);
                float ti = twR * (*oi)  + twI * (*or_);

                *or_ = *er - tr;
                *oi  = *ei - ti;
                *er  = *er + tr;
                *ei  = *ei + ti;
            }
        }
    }
}

/* ── Public forward / inverse ────────────────────────────────────────── */

void adsp_fft_forward(adsp_fft_t *fft, float *re, float *im)
{
    bitrev_shuffle(re, im, fft->br, fft->size);
    fft_pass(re, im, fft->wr, fft->wi, fft->size, 0);
}

void adsp_fft_inverse(adsp_fft_t *fft, float *re, float *im)
{
    bitrev_shuffle(re, im, fft->br, fft->size);
    fft_pass(re, im, fft->wr, fft->wi, fft->size, 1);

    /* Normalize by 1/N */
    float inv_n = 1.0f / (float)fft->size;
    int n = fft->size;
    for (int i = 0; i < n; i++) {
        re[i] *= inv_n;
        im[i] *= inv_n;
    }
}

/* ── Real FFT: pack real input into complex, forward transform ────────── */

void adsp_rfft_forward(adsp_fft_t *fft, const float *x,
                       float *re, float *im)
{
    int n = fft->size;

    /* Copy real data, zero imaginary */
    for (int i = 0; i < n; i++) {
        re[i] = x[i];
        im[i] = 0.0f;
    }

    adsp_fft_forward(fft, re, im);
    /* Caller receives full N-point complex spectrum;
     * conjugate symmetry means only bins 0..N/2 are unique. */
}
