/*
 * AetherOS — libAetherDSP: Waveshaper (Phase 8.4)
 * File: userspace/lib/libAetherDSP/src/adsp_waveshaper.c
 *
 * Lookup-table waveshaper with linear interpolation.
 * Input range: [-1, +1] mapped to table indices [0, table_size-1].
 * Drive scales input before lookup; output is post-table, [-1, +1].
 *
 * Built-in shapes: soft-clip (tanh), hard-clip, asymmetric,
 *                  fuzz (BJT), tube (triode approximation).
 */

#include "adsp.h"
#include <stdlib.h>
#include <math.h>

struct adsp_waveshaper_s {
    float *table;
    int    size;
};

/* ── Shape functions ─────────────────────────────────────────────────── */

float adsp_shape_soft_clip(float x)
{
    /* tanh approximation: fast Padé [3/3] */
    if (x >  3.0f) return  1.0f;
    if (x < -3.0f) return -1.0f;
    float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

float adsp_shape_hard_clip(float x)
{
    if (x >  1.0f) return  1.0f;
    if (x < -1.0f) return -1.0f;
    return x;
}

float adsp_shape_asymmetric(float x)
{
    /* Diode bridge: positive half → soft-clip, negative half → harder */
    if (x >= 0.0f)
        return adsp_shape_soft_clip(x * 1.5f) * 0.667f;
    else {
        float a = -x;
        return -(a / (1.0f + a));   /* -x/(1+|x|) for negative half */
    }
}

float adsp_shape_fuzz(float x)
{
    /* BJT saturation: strong positive compression, mild negative */
    if (x >= 0.0f) {
        float g = 1.0f - expf(-x * 3.0f);
        return g < 1.0f ? g : 1.0f;
    } else {
        return adsp_shape_soft_clip(x * 0.8f);
    }
}

float adsp_shape_tube(float x)
{
    /* Triode: asymmetric even-harmonic distortion */
    float q = 0.1f;   /* quiescent bias */
    float y = x + q;
    float abs_y = y < 0.0f ? -y : y;
    /* Triode transfer: -1/(1 + e^(-gain*y)) mapped to [-1,1] */
    float g = expf(-6.0f * y);
    float t = 1.0f / (1.0f + g);   /* sigmoid, 0..1 */
    (void)abs_y;
    return (t * 2.0f - 1.0f);      /* remap to [-1, 1] */
}

/* ── Table builder ───────────────────────────────────────────────────── */

adsp_waveshaper_t *adsp_waveshaper_create(float (*fn)(float), int table_size)
{
    if (!fn || table_size < 2) return NULL;

    adsp_waveshaper_t *ws = (adsp_waveshaper_t *)malloc(sizeof(*ws));
    if (!ws) return NULL;

    ws->table = (float *)malloc(sizeof(float) * table_size);
    ws->size  = table_size;

    if (!ws->table) {
        free(ws);
        return NULL;
    }

    /* Map index 0..table_size-1 → input -1..+1 */
    float inv = 2.0f / (float)(table_size - 1);
    for (int i = 0; i < table_size; i++) {
        float x = -1.0f + i * inv;
        ws->table[i] = fn(x);
    }

    return ws;
}

void adsp_waveshaper_destroy(adsp_waveshaper_t *ws)
{
    if (!ws) return;
    free(ws->table);
    free(ws);
}

/* ── Processing ──────────────────────────────────────────────────────── */

void adsp_waveshaper_process(adsp_waveshaper_t *ws,
                              float *y, const float *x,
                              float drive, int n)
{
    float inv   = (float)(ws->size - 1) * 0.5f;   /* maps [-1,1] → index */
    int   tmax  = ws->size - 1;

    for (int i = 0; i < n; i++) {
        float xi   = x[i] * drive;

        /* Clamp before lookup */
        if (xi >  1.0f) xi =  1.0f;
        if (xi < -1.0f) xi = -1.0f;

        float fidx = (xi + 1.0f) * inv;
        int   idx  = (int)fidx;
        if (idx >= tmax) idx = tmax - 1;

        float frac = fidx - (float)idx;
        y[i] = ws->table[idx] + frac * (ws->table[idx + 1] - ws->table[idx]);
    }
}
