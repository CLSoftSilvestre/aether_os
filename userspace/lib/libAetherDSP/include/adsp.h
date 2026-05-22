/*
 * AetherOS — libAetherDSP master include (Phase 8.4)
 * File: userspace/lib/libAetherDSP/include/adsp.h
 *
 * High-performance DSP primitives for the AetherOS audio pipeline.
 * All processing in float32.  NEON SIMD where available (AArch64).
 *
 * Design rules:
 *   - All functions process in blocks of N samples (N multiple of 4)
 *   - Zero dynamic allocation in the processing path
 *   - Denormal protection: call adsp_neon_init() once at startup
 *     (sets FPCR FZ=1 to flush denormals to zero)
 *   - No system calls, no printk, no global mutable state in hot paths
 */

#ifndef AETHER_ADSP_H
#define AETHER_ADSP_H

/* ── NEON utilities and math ─────────────────────────────────────────── */

/* Set FPCR FZ=1 (flush-to-zero) and DAZ=1 to prevent denormal stalls.
 * MUST be called once by the audio thread before any DSP processing.   */
void adsp_neon_init(void);

/* Vectorized block operations (n must be > 0; NEON when n multiple of 4) */
void  adsp_add_f32(float *dst, const float *a, const float *b, int n);
void  adsp_mul_f32(float *dst, const float *a, const float *b, int n);
void  adsp_mad_f32(float *dst, const float *a, float gain, const float *b, int n);
void  adsp_clamp_f32(float *dst, const float *src, float lo, float hi, int n);
void  adsp_scale_f32(float *dst, const float *src, float gain, int n);
float adsp_rms_f32(const float *x, int n);
float adsp_peak_f32(const float *x, int n);

/* Channel conversion */
void adsp_interleave_f32(float *dst, const float *l, const float *r, int frames);
void adsp_deinterleave_f32(float *l, float *r, const float *src, int frames);

/* dB ↔ linear */
float adsp_db_to_lin(float db);
float adsp_lin_to_db(float lin);

/* ── Biquad IIR Filter ───────────────────────────────────────────────── */

typedef struct {
    float b0, b1, b2, a1, a2;   /* coefficients (a0 normalised to 1) */
    float w1, w2;                /* state (direct form II transposed)  */
} adsp_biquad_t;

/* Robert Bristow-Johnson cookbook formulae */
void adsp_biquad_lpf    (adsp_biquad_t *b, float fc, float q, float sr);
void adsp_biquad_hpf    (adsp_biquad_t *b, float fc, float q, float sr);
void adsp_biquad_bpf    (adsp_biquad_t *b, float fc, float q, float sr);
void adsp_biquad_notch  (adsp_biquad_t *b, float fc, float q, float sr);
void adsp_biquad_allpass(adsp_biquad_t *b, float fc, float q, float sr);
void adsp_biquad_peaking(adsp_biquad_t *b, float fc, float q, float db, float sr);
void adsp_biquad_loshelf(adsp_biquad_t *b, float fc, float db, float sr);
void adsp_biquad_hishelf(adsp_biquad_t *b, float fc, float db, float sr);

/* Single biquad: processes n samples (can be NEON-vectorized internally) */
void adsp_biquad_process(adsp_biquad_t *b, float *y, const float *x, int n);

/* 4-biquad bank: processes n samples through all 4 filters in series.
 * Used for EQ (4 bands in one call), parameter smoothing across bands. */
void adsp_biquad_process_x4(adsp_biquad_t b[4], float *y, const float *x, int n);

/* Reset biquad state (clear z1, z2) */
static inline void adsp_biquad_reset(adsp_biquad_t *b)
{
    b->w1 = b->w2 = 0.0f;
}

/* ── FFT Engine ──────────────────────────────────────────────────────── */

typedef struct adsp_fft_s adsp_fft_t;

/* Create FFT plan. size must be a power of 2 (64 ≤ size ≤ 65536).    */
adsp_fft_t *adsp_fft_create(int size);
void        adsp_fft_destroy(adsp_fft_t *fft);

/* Complex in-place forward / inverse FFT (separate re/im arrays).     */
void adsp_fft_forward(adsp_fft_t *fft, float *re, float *im);
void adsp_fft_inverse(adsp_fft_t *fft, float *re, float *im);

/* Real input → complex spectrum (n/2+1 bins).                         */
void adsp_rfft_forward(adsp_fft_t *fft, const float *x,
                       float *re, float *im);

/* ── Overlap-Add Convolution ────────────────────────────────────────── */

typedef struct adsp_conv_s adsp_conv_t;

/* Create convolver.  ir_len samples, block_size = period_frames.      */
adsp_conv_t *adsp_conv_create(const float *ir, int ir_len, int block_size);
void         adsp_conv_destroy(adsp_conv_t *conv);
void         adsp_conv_process(adsp_conv_t *conv,
                                float *out, const float *in, int n);

/* ── Envelope Follower ──────────────────────────────────────────────── */

typedef struct {
    float attack_coef;
    float release_coef;
    float state;
} adsp_env_t;

void  adsp_env_init(adsp_env_t *e, float attack_ms, float release_ms, float sr);
float adsp_env_peak(adsp_env_t *e, float x);
float adsp_env_rms (adsp_env_t *e, float x_sq);   /* pass x*x */

/* ── dBFS Peak Meter with hold ──────────────────────────────────────── */

typedef struct adsp_meter_s adsp_meter_t;

adsp_meter_t *adsp_meter_create(float hold_sec, float decay_db_per_sec, float sr);
void          adsp_meter_destroy(adsp_meter_t *m);
void          adsp_meter_process(adsp_meter_t *m, const float *x, int n);
float         adsp_meter_peak_dbfs(adsp_meter_t *m);
float         adsp_meter_rms_dbfs (adsp_meter_t *m);
float         adsp_meter_held_peak_dbfs(adsp_meter_t *m);
void          adsp_meter_reset(adsp_meter_t *m);

/* ── Waveshaper (non-linear distortion) ─────────────────────────────── */

typedef struct adsp_waveshaper_s adsp_waveshaper_t;

/* Create with a shape function; table_size determines resolution.     */
adsp_waveshaper_t *adsp_waveshaper_create(float (*fn)(float), int table_size);
void               adsp_waveshaper_destroy(adsp_waveshaper_t *ws);
void               adsp_waveshaper_process(adsp_waveshaper_t *ws,
                                            float *y, const float *x,
                                            float drive, int n);

/* Built-in shape functions */
float adsp_shape_soft_clip  (float x);   /* tanh approximation           */
float adsp_shape_hard_clip  (float x);   /* clamp(-1, +1)                */
float adsp_shape_asymmetric (float x);   /* diode bridge model           */
float adsp_shape_fuzz        (float x);  /* BJT saturation model         */
float adsp_shape_tube        (float x);  /* triode approximation         */

/* ── Parameter Smoother ─────────────────────────────────────────────── */

typedef struct {
    float target;
    float current;
    float coef;    /* e^(-2π·10Hz/sr) — ~10ms smoothing time           */
} adsp_smooth_t;

void adsp_smooth_init(adsp_smooth_t *s, float initial, float time_ms, float sr);

static inline float adsp_smooth_tick(adsp_smooth_t *s)
{
    s->current += s->coef * (s->target - s->current);
    return s->current;
}

/* ── Resampler ───────────────────────────────────────────────────────── */

typedef struct adsp_resampler_s adsp_resampler_t;

/* quality: 0 = linear (fast, aliased), 1 = sinc (slow, clean)         */
adsp_resampler_t *adsp_resampler_create(float ratio, int quality);
void              adsp_resampler_destroy(adsp_resampler_t *r);
void              adsp_resampler_process(adsp_resampler_t *r,
                                          const float *in,  int in_frames,
                                          float       *out, int *out_frames);

#endif /* AETHER_ADSP_H */
