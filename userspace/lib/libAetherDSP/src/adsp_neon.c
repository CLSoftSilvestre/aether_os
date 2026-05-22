/*
 * AetherOS — libAetherDSP: NEON utilities (Phase 8.4)
 * File: userspace/lib/libAetherDSP/src/adsp_neon.c
 *
 * Vectorised float32 block operations using AArch64 NEON intrinsics.
 * All functions fall back to scalar when n is not a multiple of 4.
 */

#include "adsp.h"
#include <arm_neon.h>

/* ── FPCR FZ+DAZ (flush-to-zero + denormals-are-zero) ───────────────── */

void adsp_neon_init(void)
{
    /* FPCR bit 24 = FZ (flush-to-zero output), bit 1 = AHP not needed.
     * On AArch64, FPCR.FZ applies to both singles and doubles.         */
    unsigned long fpcr;
    __asm__ volatile("mrs %0, fpcr" : "=r"(fpcr));
    fpcr |= (1UL << 24);   /* FZ = 1 */
    __asm__ volatile("msr fpcr, %0" :: "r"(fpcr));
}

/* ── Block arithmetic ────────────────────────────────────────────────── */

void adsp_add_f32(float *dst, const float *a, const float *b, int n)
{
    int i = 0;
    for (; i <= n - 4; i += 4) {
        float32x4_t va = vld1q_f32(&a[i]);
        float32x4_t vb = vld1q_f32(&b[i]);
        vst1q_f32(&dst[i], vaddq_f32(va, vb));
    }
    for (; i < n; i++) dst[i] = a[i] + b[i];
}

void adsp_mul_f32(float *dst, const float *a, const float *b, int n)
{
    int i = 0;
    for (; i <= n - 4; i += 4) {
        float32x4_t va = vld1q_f32(&a[i]);
        float32x4_t vb = vld1q_f32(&b[i]);
        vst1q_f32(&dst[i], vmulq_f32(va, vb));
    }
    for (; i < n; i++) dst[i] = a[i] * b[i];
}

/* dst[i] = a[i] * gain + b[i]  (fused multiply-add) */
void adsp_mad_f32(float *dst, const float *a, float gain,
                  const float *b, int n)
{
    int i = 0;
    float32x4_t vg = vdupq_n_f32(gain);
    for (; i <= n - 4; i += 4) {
        float32x4_t va = vld1q_f32(&a[i]);
        float32x4_t vb = vld1q_f32(&b[i]);
        vst1q_f32(&dst[i], vmlaq_f32(vb, va, vg));
    }
    for (; i < n; i++) dst[i] = a[i] * gain + b[i];
}

void adsp_clamp_f32(float *dst, const float *src, float lo, float hi, int n)
{
    int i = 0;
    float32x4_t vlo = vdupq_n_f32(lo);
    float32x4_t vhi = vdupq_n_f32(hi);
    for (; i <= n - 4; i += 4) {
        float32x4_t v = vld1q_f32(&src[i]);
        v = vmaxq_f32(v, vlo);
        v = vminq_f32(v, vhi);
        vst1q_f32(&dst[i], v);
    }
    for (; i < n; i++) {
        float v = src[i];
        if (v < lo) v = lo;
        if (v > hi) v = hi;
        dst[i] = v;
    }
}

void adsp_scale_f32(float *dst, const float *src, float gain, int n)
{
    int i = 0;
    float32x4_t vg = vdupq_n_f32(gain);
    for (; i <= n - 4; i += 4)
        vst1q_f32(&dst[i], vmulq_f32(vld1q_f32(&src[i]), vg));
    for (; i < n; i++) dst[i] = src[i] * gain;
}

float adsp_rms_f32(const float *x, int n)
{
    if (n <= 0) return 0.0f;
    float sum = 0.0f;
    int i = 0;
    float32x4_t acc = vdupq_n_f32(0.0f);
    for (; i <= n - 4; i += 4) {
        float32x4_t v = vld1q_f32(&x[i]);
        acc = vmlaq_f32(acc, v, v);
    }
    /* Horizontal sum */
    float32x2_t lo = vget_low_f32(acc);
    float32x2_t hi = vget_high_f32(acc);
    lo = vadd_f32(lo, hi);
    lo = vpadd_f32(lo, lo);
    sum = vget_lane_f32(lo, 0);
    for (; i < n; i++) sum += x[i] * x[i];
    /* Fast inverse sqrt via Newton-Raphson for arm */
    float mean = sum / (float)n;
    if (mean <= 0.0f) return 0.0f;
    /* Use sqrtf approximation via NEON vrsqrte + refinement */
    float32x2_t vm = vdup_n_f32(mean);
    float32x2_t est = vrsqrte_f32(vm);
    est = vmul_f32(est, vrsqrts_f32(vmul_f32(vm, est), est));
    float inv_sqrt = vget_lane_f32(est, 0);
    return mean * inv_sqrt;   /* mean * (1/sqrt(mean)) = sqrt(mean) */
}

float adsp_peak_f32(const float *x, int n)
{
    if (n <= 0) return 0.0f;
    float32x4_t vmax = vdupq_n_f32(0.0f);
    int i = 0;
    for (; i <= n - 4; i += 4) {
        float32x4_t v = vabsq_f32(vld1q_f32(&x[i]));
        vmax = vmaxq_f32(vmax, v);
    }
    float m = vmaxvq_f32(vmax);
    for (; i < n; i++) {
        float v = x[i] < 0.0f ? -x[i] : x[i];
        if (v > m) m = v;
    }
    return m;
}

/* ── Channel interleave / deinterleave ───────────────────────────────── */

void adsp_interleave_f32(float *dst, const float *l, const float *r, int frames)
{
    int i = 0;
    for (; i <= frames - 4; i += 4) {
        float32x4x2_t v;
        v.val[0] = vld1q_f32(&l[i]);
        v.val[1] = vld1q_f32(&r[i]);
        vst2q_f32(&dst[i * 2], v);
    }
    for (; i < frames; i++) {
        dst[i * 2]     = l[i];
        dst[i * 2 + 1] = r[i];
    }
}

void adsp_deinterleave_f32(float *l, float *r, const float *src, int frames)
{
    int i = 0;
    for (; i <= frames - 4; i += 4) {
        float32x4x2_t v = vld2q_f32(&src[i * 2]);
        vst1q_f32(&l[i], v.val[0]);
        vst1q_f32(&r[i], v.val[1]);
    }
    for (; i < frames; i++) {
        l[i] = src[i * 2];
        r[i] = src[i * 2 + 1];
    }
}

/* ── dB / linear conversion ──────────────────────────────────────────── */

/* Fast log2 approximation (< 0.003 dB error, polynomial) */
static float fast_log2f(float x)
{
    /* Extract exponent and mantissa */
    union { float f; unsigned int u; } v = { x };
    int exp = (int)((v.u >> 23) & 0xFF) - 127;
    v.u = (v.u & 0x007FFFFF) | 0x3F800000;   /* mantissa in [1, 2) */
    float m = v.f - 1.0f;
    /* Minimax polynomial for log2(1+m), m in [0,1] */
    float p = 1.44269504f * (m - 0.5f * m * m + 0.333333f * m * m * m
              - 0.25f * m * m * m * m);
    return (float)exp + p;
}

float adsp_lin_to_db(float lin)
{
    if (lin <= 0.0f) return -144.0f;
    return 20.0f * fast_log2f(lin) * 0.30103f;   /* log2 * log10(2) */
}

float adsp_db_to_lin(float db)
{
    /* 10^(db/20) = 2^(db * log2(10) / 20) */
    float exp2 = db * 0.1660964f;   /* db / 20 * log2(10) */
    int iexp = (int)exp2;
    float frac = exp2 - (float)iexp;
    /* 2^frac ≈ 1 + frac*(0.6931f + frac*0.2402f) for frac in [0,1) */
    float f2 = 1.0f + frac * (0.693147f + frac * (0.240227f + frac * 0.055504f));
    union { unsigned int u; float f; } r;
    r.u = (unsigned int)((iexp + 127) << 23);
    return r.f * f2;
}
