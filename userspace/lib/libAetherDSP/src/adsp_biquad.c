/*
 * AetherOS — libAetherDSP: Biquad IIR Filter (Phase 8.4)
 * File: userspace/lib/libAetherDSP/src/adsp_biquad.c
 *
 * Direct Form II Transposed biquad — numerically stable and NEON-friendly.
 * Coefficient formulae from Robert Bristow-Johnson's Audio EQ Cookbook.
 *
 * Transfer function: H(z) = (b0 + b1·z⁻¹ + b2·z⁻²)
 *                          / (1  + a1·z⁻¹ + a2·z⁻²)
 *
 * State update (DFT2):
 *   y[n]  = b0·x[n] + w1
 *   w1    = b1·x[n] - a1·y[n] + w2
 *   w2    = b2·x[n] - a2·y[n]
 */

#include "adsp.h"
#include <math.h>

/* ── Math helpers ────────────────────────────────────────────────────── */

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

static float cosf_approx(float x)
{
    /* Paganini cosine approximation valid for [-π, π] */
    float x2 = x * x;
    return 1.0f - x2 * (0.5f - x2 * (0.0416666f - x2 * 0.001388f));
}

static float sinf_approx(float x)
{
    float x2 = x * x;
    return x * (1.0f - x2 * (0.1666667f - x2 * (0.0083333f - x2 * 0.000198f)));
}

static float sinhf_approx(float x)
{
    float e = x < 0.0f ? -x : x;
    /* For small x: sinh(x) ≈ x + x³/6 */
    if (e < 1.0f)
        return x + x * x * x * 0.16667f;
    /* For larger x use identity: sinh(x) = (e^x - e^-x) / 2 */
    float ex = adsp_db_to_lin(x * 8.6859f);   /* e^x = 10^(x*log10(e)) */
    return (ex - 1.0f / ex) * 0.5f;
}

/* ── Cookbook coefficient calculators ───────────────────────────────── */

void adsp_biquad_lpf(adsp_biquad_t *b, float fc, float q, float sr)
{
    float w0 = 2.0f * M_PI * fc / sr;
    float cw = cosf_approx(w0);
    float sw = sinf_approx(w0);
    float alpha = sw / (2.0f * q);
    float a0r = 1.0f / (1.0f + alpha);
    b->b0 = (1.0f - cw) * 0.5f * a0r;
    b->b1 = (1.0f - cw) * a0r;
    b->b2 = b->b0;
    b->a1 = -2.0f * cw * a0r;
    b->a2 = (1.0f - alpha) * a0r;
}

void adsp_biquad_hpf(adsp_biquad_t *b, float fc, float q, float sr)
{
    float w0 = 2.0f * M_PI * fc / sr;
    float cw = cosf_approx(w0);
    float sw = sinf_approx(w0);
    float alpha = sw / (2.0f * q);
    float a0r = 1.0f / (1.0f + alpha);
    b->b0 = (1.0f + cw) * 0.5f * a0r;
    b->b1 = -(1.0f + cw) * a0r;
    b->b2 = b->b0;
    b->a1 = -2.0f * cw * a0r;
    b->a2 = (1.0f - alpha) * a0r;
}

void adsp_biquad_bpf(adsp_biquad_t *b, float fc, float q, float sr)
{
    float w0 = 2.0f * M_PI * fc / sr;
    float sw = sinf_approx(w0);
    float cw = cosf_approx(w0);
    float alpha = sw / (2.0f * q);
    float a0r = 1.0f / (1.0f + alpha);
    b->b0 =  sw * 0.5f * a0r;
    b->b1 =  0.0f;
    b->b2 = -b->b0;
    b->a1 = -2.0f * cw * a0r;
    b->a2 = (1.0f - alpha) * a0r;
}

void adsp_biquad_notch(adsp_biquad_t *b, float fc, float q, float sr)
{
    float w0 = 2.0f * M_PI * fc / sr;
    float cw = cosf_approx(w0);
    float sw = sinf_approx(w0);
    float alpha = sw / (2.0f * q);
    float a0r = 1.0f / (1.0f + alpha);
    b->b0 =  1.0f * a0r;
    b->b1 = -2.0f * cw * a0r;
    b->b2 =  1.0f * a0r;
    b->a1 = b->b1;
    b->a2 = (1.0f - alpha) * a0r;
}

void adsp_biquad_allpass(adsp_biquad_t *b, float fc, float q, float sr)
{
    float w0 = 2.0f * M_PI * fc / sr;
    float cw = cosf_approx(w0);
    float sw = sinf_approx(w0);
    float alpha = sw / (2.0f * q);
    float a0r = 1.0f / (1.0f + alpha);
    b->b0 = (1.0f - alpha) * a0r;
    b->b1 = -2.0f * cw * a0r;
    b->b2 =  1.0f;
    b->a1 = b->b1;
    b->a2 = b->b0;
}

void adsp_biquad_peaking(adsp_biquad_t *b, float fc, float q,
                          float db, float sr)
{
    float A  = adsp_db_to_lin(db * 0.5f);   /* sqrt(10^(db/20)) */
    float w0 = 2.0f * M_PI * fc / sr;
    float cw = cosf_approx(w0);
    float sw = sinf_approx(w0);
    float alpha = sw / (2.0f * q);
    float a0r = 1.0f / (1.0f + alpha / A);
    b->b0 = (1.0f + alpha * A) * a0r;
    b->b1 = -2.0f * cw * a0r;
    b->b2 = (1.0f - alpha * A) * a0r;
    b->a1 = b->b1;
    b->a2 = (1.0f - alpha / A) * a0r;
}

void adsp_biquad_loshelf(adsp_biquad_t *b, float fc, float db, float sr)
{
    float A  = adsp_db_to_lin(db * 0.5f);
    float w0 = 2.0f * M_PI * fc / sr;
    float cw = cosf_approx(w0);
    float sw = sinf_approx(w0);
    float sqA = adsp_db_to_lin(db * 0.25f);   /* A^0.5 */
    float alpha = sw * 0.5f * (sqA + 1.0f / sqA);
    float a0r = 1.0f / ((A+1.0f) + (A-1.0f)*cw + 2.0f*sqA*alpha);
    b->b0 = A * ((A+1.0f) - (A-1.0f)*cw + 2.0f*sqA*alpha) * a0r;
    b->b1 = 2.0f * A * ((A-1.0f) - (A+1.0f)*cw) * a0r;
    b->b2 = A * ((A+1.0f) - (A-1.0f)*cw - 2.0f*sqA*alpha) * a0r;
    b->a1 = -2.0f * ((A-1.0f) + (A+1.0f)*cw) * a0r;
    b->a2 = ((A+1.0f) + (A-1.0f)*cw - 2.0f*sqA*alpha) * a0r;
    (void)sinhf_approx; (void)sqA;
}

void adsp_biquad_hishelf(adsp_biquad_t *b, float fc, float db, float sr)
{
    float A  = adsp_db_to_lin(db * 0.5f);
    float w0 = 2.0f * M_PI * fc / sr;
    float cw = cosf_approx(w0);
    float sw = sinf_approx(w0);
    float sqA = adsp_db_to_lin(db * 0.25f);
    float alpha = sw * 0.5f * (sqA + 1.0f / sqA);
    float a0r = 1.0f / ((A+1.0f) - (A-1.0f)*cw + 2.0f*sqA*alpha);
    b->b0 = A * ((A+1.0f) + (A-1.0f)*cw + 2.0f*sqA*alpha) * a0r;
    b->b1 = -2.0f * A * ((A-1.0f) + (A+1.0f)*cw) * a0r;
    b->b2 = A * ((A+1.0f) + (A-1.0f)*cw - 2.0f*sqA*alpha) * a0r;
    b->a1 = 2.0f * ((A-1.0f) - (A+1.0f)*cw) * a0r;
    b->a2 = ((A+1.0f) - (A-1.0f)*cw - 2.0f*sqA*alpha) * a0r;
}

/* ── Processing ──────────────────────────────────────────────────────── */

void adsp_biquad_process(adsp_biquad_t *b, float *y, const float *x, int n)
{
    float b0 = b->b0, b1 = b->b1, b2 = b->b2;
    float a1 = b->a1, a2 = b->a2;
    float w1 = b->w1, w2 = b->w2;

    for (int i = 0; i < n; i++) {
        float xi = x[i];
        float yi = b0 * xi + w1;
        w1 = b1 * xi - a1 * yi + w2;
        w2 = b2 * xi - a2 * yi;
        y[i] = yi;
    }

    b->w1 = w1;
    b->w2 = w2;
}

void adsp_biquad_process_x4(adsp_biquad_t b[4], float *y, const float *x, int n)
{
    /* Temp buffer to chain 4 biquads — avoids extra alloc by using static */
    static float tmp[4096];
    if (n > 4096) n = 4096;
    adsp_biquad_process(&b[0], tmp, x,   n);
    adsp_biquad_process(&b[1], tmp, tmp, n);
    adsp_biquad_process(&b[2], tmp, tmp, n);
    adsp_biquad_process(&b[3], y,   tmp, n);
}
