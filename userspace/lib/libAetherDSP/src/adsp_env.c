/*
 * AetherOS — libAetherDSP: Envelope Follower + dBFS Meter (Phase 8.4)
 * File: userspace/lib/libAetherDSP/src/adsp_env.c
 *
 * adsp_env_t  — per-sample peak/RMS envelope with asymmetric attack/release
 * adsp_meter_t — dBFS peak meter with hold + decay, block-level processing
 */

#include "adsp.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ── Envelope follower ───────────────────────────────────────────────── */

void adsp_env_init(adsp_env_t *e,
                   float attack_ms, float release_ms, float sr)
{
    /* Coefficients: 1-pole RC smoothing, time constant = -ln(0.001)/ms */
    e->attack_coef  = expf(-1000.0f / (sr * attack_ms));
    e->release_coef = expf(-1000.0f / (sr * release_ms));
    e->state        = 0.0f;
}

float adsp_env_peak(adsp_env_t *e, float x)
{
    float env = e->state;
    float abs_x = x < 0.0f ? -x : x;
    float coef = abs_x > env ? e->attack_coef : e->release_coef;
    env = abs_x + coef * (env - abs_x);
    e->state = env;
    return env;
}

float adsp_env_rms(adsp_env_t *e, float x_sq)
{
    float env = e->state;
    float coef = x_sq > env ? e->attack_coef : e->release_coef;
    env = x_sq + coef * (env - x_sq);
    e->state = env;
    return env;
}

/* ── dBFS Peak Meter with hold ───────────────────────────────────────── */

struct adsp_meter_s {
    float   peak_lin;        /* running peak, linear */
    float   rms_sum;         /* accumulated squared samples */
    int     rms_count;
    float   rms_lin;         /* last computed RMS */
    float   held_peak_lin;   /* peak hold */
    int     hold_samples;    /* hold time in samples */
    int     hold_counter;
    float   decay_coef;      /* per-sample decay multiplier */
    float   sr;
};

adsp_meter_t *adsp_meter_create(float hold_sec,
                                 float decay_db_per_sec,
                                 float sr)
{
    adsp_meter_t *m = (adsp_meter_t *)malloc(sizeof(adsp_meter_t));
    if (!m) return NULL;

    m->peak_lin      = 0.0f;
    m->rms_sum       = 0.0f;
    m->rms_count     = 0;
    m->rms_lin       = 0.0f;
    m->held_peak_lin = 0.0f;
    m->hold_samples  = (int)(hold_sec * sr);
    m->hold_counter  = 0;
    m->sr            = sr;
    /* Decay: -X dB/s → multiplier per sample */
    m->decay_coef    = adsp_db_to_lin(-decay_db_per_sec / sr);
    return m;
}

void adsp_meter_destroy(adsp_meter_t *m) { free(m); }

void adsp_meter_process(adsp_meter_t *m, const float *x, int n)
{
    float block_peak = adsp_peak_f32(x, n);

    /* RMS accumulation */
    for (int i = 0; i < n; i++) m->rms_sum += x[i] * x[i];
    m->rms_count += n;

    /* Update peak */
    if (block_peak >= m->held_peak_lin) {
        m->held_peak_lin = block_peak;
        m->hold_counter  = m->hold_samples;
    } else {
        if (m->hold_counter > 0) {
            m->hold_counter -= n;
        } else {
            m->held_peak_lin *= m->decay_coef;
            if (m->held_peak_lin < 1e-7f) m->held_peak_lin = 0.0f;
        }
    }

    if (block_peak > m->peak_lin)
        m->peak_lin = block_peak;
    else {
        for (int i = 0; i < n; i++) m->peak_lin *= m->decay_coef;
        if (m->peak_lin < 1e-7f) m->peak_lin = 0.0f;
    }

    /* Flush RMS every 400 ms worth of samples */
    if (m->rms_count >= (int)(m->sr * 0.4f)) {
        float mean = m->rms_sum / (float)m->rms_count;
        m->rms_lin   = mean > 0.0f ? sqrtf(mean) : 0.0f;
        m->rms_sum   = 0.0f;
        m->rms_count = 0;
    }
}

float adsp_meter_peak_dbfs(adsp_meter_t *m)
{
    return adsp_lin_to_db(m->peak_lin);
}

float adsp_meter_rms_dbfs(adsp_meter_t *m)
{
    return adsp_lin_to_db(m->rms_lin);
}

float adsp_meter_held_peak_dbfs(adsp_meter_t *m)
{
    return adsp_lin_to_db(m->held_peak_lin);
}

void adsp_meter_reset(adsp_meter_t *m)
{
    m->peak_lin      = 0.0f;
    m->rms_sum       = 0.0f;
    m->rms_count     = 0;
    m->rms_lin       = 0.0f;
    m->held_peak_lin = 0.0f;
    m->hold_counter  = 0;
}
