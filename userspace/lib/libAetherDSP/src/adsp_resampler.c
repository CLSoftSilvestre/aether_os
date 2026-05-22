/*
 * AetherOS — libAetherDSP: Resampler (Phase 8.4)
 * File: userspace/lib/libAetherDSP/src/adsp_resampler.c
 *
 * quality 0 — linear interpolation (fast, minor aliasing near Nyquist)
 * quality 1 — 6-tap windowed sinc (clean, ~60 dB stopband)
 *
 * Ratio = output_sample_rate / input_sample_rate.
 * Fixed-point phase accumulator for sub-sample accuracy.
 */

#include "adsp.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define SINC_TAPS 6   /* half-length; filter is 2*SINC_TAPS taps */

struct adsp_resampler_s {
    float ratio;     /* out/in rate ratio */
    int   quality;
    float phase;     /* sub-sample phase accumulator, 0..1 */
    float history[SINC_TAPS * 2];   /* circular history for sinc */
    int   hpos;      /* history write head */
};

adsp_resampler_t *adsp_resampler_create(float ratio, int quality)
{
    if (ratio <= 0.0f) return NULL;

    adsp_resampler_t *r = (adsp_resampler_t *)malloc(sizeof(*r));
    if (!r) return NULL;

    r->ratio   = ratio;
    r->quality = quality;
    r->phase   = 0.0f;
    r->hpos    = 0;
    memset(r->history, 0, sizeof(r->history));
    return r;
}

void adsp_resampler_destroy(adsp_resampler_t *r) { free(r); }

/* ── Linear resampler ────────────────────────────────────────────────── */

static void resample_linear(adsp_resampler_t *r,
                             const float *in,  int in_frames,
                             float       *out, int *out_frames)
{
    int   out_max = *out_frames;
    float phase   = r->phase;
    float ratio   = r->ratio;
    int   out_n   = 0;

    while (out_n < out_max) {
        int   i0  = (int)phase;
        float frac = phase - (float)i0;

        if (i0 + 1 >= in_frames) break;

        out[out_n++] = in[i0] + frac * (in[i0 + 1] - in[i0]);
        phase += 1.0f / ratio;
    }

    r->phase = phase - (int)phase;   /* keep fractional part only */
    *out_frames = out_n;
}

/* ── Windowed sinc (Hann window, 12-tap) ─────────────────────────────── */

static float sinc(float x)
{
    if (x == 0.0f) return 1.0f;
    float px = (float)M_PI * x;
    return sinf(px) / px;
}

static float hann(float n, int N)
{
    return 0.5f * (1.0f - cosf((float)(2.0 * M_PI) * n / (float)N));
}

static void resample_sinc(adsp_resampler_t *r,
                           const float *in,  int in_frames,
                           float       *out, int *out_frames)
{
    int   out_max = *out_frames;
    float phase   = r->phase;
    float ratio   = r->ratio;
    int   out_n   = 0;
    int   taps    = SINC_TAPS * 2;

    for (int fi = 0; fi < in_frames && out_n < out_max; ) {
        /* Push new input sample into history */
        r->history[r->hpos % taps] = in[fi];
        r->hpos++;

        /* Drain output samples for this input sample */
        while (phase < 1.0f && out_n < out_max) {
            float sum = 0.0f;
            for (int k = 0; k < taps; k++) {
                int   hist_i = (r->hpos - 1 - k + taps * 16) % taps;
                float offset = (float)(k - SINC_TAPS) + phase;
                float win    = hann((float)k, taps - 1);
                sum += r->history[hist_i] * sinc(offset) * win;
            }
            out[out_n++] = sum;
            phase += 1.0f / ratio;
        }
        phase -= 1.0f;
        fi++;
    }

    r->phase    = phase;
    *out_frames = out_n;
}

/* ── Public process ──────────────────────────────────────────────────── */

void adsp_resampler_process(adsp_resampler_t *r,
                             const float *in,  int in_frames,
                             float       *out, int *out_frames)
{
    if (r->quality == 0)
        resample_linear(r, in, in_frames, out, out_frames);
    else
        resample_sinc(r, in, in_frames, out, out_frames);
}
