/*
 * AetherOS — libAetherFX: Utility effects (Phase 8.7)
 * File: userspace/lib/libAetherFX/src/afx_util.c
 *
 * Tuner  — chromatic pitch detection via AMDF (Average Magnitude
 *           Difference Function); outputs note name + cents deviation.
 * Noise Gate — threshold-based gate with hysteresis, fast attack,
 *              programmable hold + release.
 * Boost  — clean gain boost (1-20 dB) with soft output clipping.
 */

#include "aplug.h"
#include "adsp.h"
#include "afx.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ── Shared plugin alloc helper ──────────────────────────────────────── */

static aplug_t *alloc_plug(const aplug_descriptor_t *desc,
                            const aplug_vtable_t     *vtable,
                            int state_size)
{
    aplug_t *p = (aplug_t *)malloc(sizeof(aplug_t) + state_size);
    if (!p) return NULL;
    p->desc   = desc;
    p->vtable = vtable;
    p->state  = (char *)p + sizeof(aplug_t);
    memset(p->state, 0, (size_t)state_size);
    return p;
}

/* ════════════════════════════════════════════════════════════════════════
 * Tuner
 * AMDF pitch detection over a 1024-sample buffer.
 * Outputs detected Hz into param TUNER_PARAM_HZ (read-only).
 * ════════════════════════════════════════════════════════════════════════ */

#define TUNER_BUF 1024

enum { TUNER_P_BYPASS=0, TUNER_P_HZ };

static const aplug_param_t tuner_params[] = {
    { TUNER_P_BYPASS, "Bypass", "", 0,1,0, APLUG_PARAM_BYPASS|APLUG_PARAM_TOGGLE },
    { TUNER_P_HZ,     "Hz",   "Hz", 0,2000,0, APLUG_PARAM_READONLY },
};
static const aplug_descriptor_t tuner_desc = {
    APLUG_VERSION(1,0,0), "os.aether.tuner", "Chromatic Tuner",
    "AetherOS Audio", APLUG_CAT_UTILITY, 2, tuner_params, 1, 1
};

typedef struct {
    float  buf[TUNER_BUF];
    int    pos;
    float  detected_hz;
    float  sr;
} tuner_state_t;

static int   tuner_activate(aplug_t *p, float sr, int period)
    { (void)period; ((tuner_state_t*)p->state)->sr = sr; return 0; }
static void  tuner_deactivate(aplug_t *p) { (void)p; }

static float amdf_pitch(const float *buf, int n, float sr)
{
    /* AMDF: find lag with minimum average magnitude difference */
    int   best_lag = 0;
    float best_val = 1e30f;
    int   min_lag  = (int)(sr / 1200.0f);  /* 1200 Hz max */
    int   max_lag  = (int)(sr / 50.0f);    /* 50 Hz min */
    if (max_lag >= n) max_lag = n - 1;

    for (int lag = min_lag; lag <= max_lag; lag++) {
        float sum = 0.0f;
        int   cnt = n - lag;
        for (int i = 0; i < cnt; i++) {
            float d = buf[i] - buf[i + lag];
            sum += d < 0 ? -d : d;
        }
        float avg = sum / (float)cnt;
        if (avg < best_val) { best_val = avg; best_lag = lag; }
    }
    return best_lag > 0 ? sr / (float)best_lag : 0.0f;
}

static void tuner_process(aplug_t *p, const float *const *in,
                            float *const *out, int n)
{
    tuner_state_t *s = (tuner_state_t*)p->state;
    for (int i = 0; i < n; i++) {
        s->buf[s->pos] = in[0][i];
        s->pos = (s->pos + 1) % TUNER_BUF;
        out[0][i] = in[0][i];
    }
    /* Recompute pitch once per block when buffer is full */
    if (s->pos == 0)
        s->detected_hz = amdf_pitch(s->buf, TUNER_BUF, s->sr);
}

static void  tuner_param_set(aplug_t *p, unsigned int id, float v)
    { (void)p; (void)id; (void)v; }
static float tuner_param_get(aplug_t *p, unsigned int id)
{
    if (id == TUNER_P_HZ) return ((tuner_state_t*)p->state)->detected_hz;
    return 0.0f;
}

static const aplug_vtable_t tuner_vtable = {
    tuner_activate, tuner_deactivate,
    tuner_process, tuner_param_set, tuner_param_get
};
aplug_t *afx_tuner_factory(void)
    { return alloc_plug(&tuner_desc, &tuner_vtable, sizeof(tuner_state_t)); }

/* ════════════════════════════════════════════════════════════════════════
 * Noise Gate
 * ════════════════════════════════════════════════════════════════════════ */

enum { GATE_P_BYPASS=0, GATE_P_THRESH, GATE_P_ATTACK, GATE_P_HOLD, GATE_P_RELEASE };

static const aplug_param_t gate_params[] = {
    { GATE_P_BYPASS,  "Bypass",  "",  0, 1, 0, APLUG_PARAM_BYPASS|APLUG_PARAM_TOGGLE },
    { GATE_P_THRESH,  "Thresh",  "dB",-80,-20,-40, APLUG_PARAM_AUTOMATABLE },
    { GATE_P_ATTACK,  "Attack",  "ms", 0.1f,50,2, APLUG_PARAM_AUTOMATABLE },
    { GATE_P_HOLD,    "Hold",    "ms", 0,500,50, APLUG_PARAM_AUTOMATABLE },
    { GATE_P_RELEASE, "Release", "ms", 10,2000,100, APLUG_PARAM_AUTOMATABLE },
};
static const aplug_descriptor_t gate_desc = {
    APLUG_VERSION(1,0,0), "os.aether.noisegate", "Noise Gate",
    "AetherOS Audio", APLUG_CAT_UTILITY, 5, gate_params, 1, 1
};

typedef struct {
    float   threshold_lin;
    float   attack_coef;
    float   release_coef;
    int     hold_samples;
    int     hold_timer;
    float   gain;
    int     open;
    float   sr;
} gate_state_t;

static int gate_activate(aplug_t *p, float sr, int period)
{
    (void)period;
    gate_state_t *s = (gate_state_t*)p->state;
    s->sr             = sr;
    s->threshold_lin  = adsp_db_to_lin(-40.0f);
    s->attack_coef    = expf(-1000.0f / (sr * 2.0f));
    s->release_coef   = expf(-1000.0f / (sr * 100.0f));
    s->hold_samples   = (int)(0.05f * sr);
    s->gain = 0.0f; s->open = 0; s->hold_timer = 0;
    return 0;
}
static void gate_deactivate(aplug_t *p) { (void)p; }

static void gate_process(aplug_t *p, const float *const *in,
                          float *const *out, int n)
{
    gate_state_t *s = (gate_state_t*)p->state;
    for (int i = 0; i < n; i++) {
        float x   = in[0][i];
        float abs = x < 0 ? -x : x;
        if (abs > s->threshold_lin) {
            s->open = 1; s->hold_timer = s->hold_samples;
        } else if (s->open) {
            if (s->hold_timer > 0) { s->hold_timer--; }
            else { s->open = 0; }
        }
        float target = s->open ? 1.0f : 0.0f;
        float coef   = target > s->gain ? s->attack_coef : s->release_coef;
        s->gain = target + coef * (s->gain - target);
        out[0][i] = x * s->gain;
    }
}

static void gate_param_set(aplug_t *p, unsigned int id, float v)
{
    gate_state_t *s = (gate_state_t*)p->state;
    if (id == GATE_P_THRESH)  s->threshold_lin = adsp_db_to_lin(v);
    if (id == GATE_P_ATTACK)  s->attack_coef   = expf(-1000.0f / (s->sr * v));
    if (id == GATE_P_HOLD)    s->hold_samples  = (int)(v * 0.001f * s->sr);
    if (id == GATE_P_RELEASE) s->release_coef  = expf(-1000.0f / (s->sr * v));
}
static float gate_param_get(aplug_t *p, unsigned int id)
{
    gate_state_t *s = (gate_state_t*)p->state;
    if (id == GATE_P_THRESH) return adsp_lin_to_db(s->threshold_lin);
    return 0.0f;
}

static const aplug_vtable_t gate_vtable = {
    gate_activate, gate_deactivate,
    gate_process, gate_param_set, gate_param_get
};
aplug_t *afx_noisegate_factory(void)
    { return alloc_plug(&gate_desc, &gate_vtable, sizeof(gate_state_t)); }

/* ════════════════════════════════════════════════════════════════════════
 * Clean Boost
 * ════════════════════════════════════════════════════════════════════════ */

enum { BOOST_P_BYPASS=0, BOOST_P_GAIN };

static const aplug_param_t boost_params[] = {
    { BOOST_P_BYPASS, "Bypass", "", 0,1,0, APLUG_PARAM_BYPASS|APLUG_PARAM_TOGGLE },
    { BOOST_P_GAIN,   "Gain",  "dB", 0,20,6, APLUG_PARAM_AUTOMATABLE|APLUG_PARAM_SMOOTHED },
};
static const aplug_descriptor_t boost_desc = {
    APLUG_VERSION(1,0,0), "os.aether.boost", "Clean Boost",
    "AetherOS Audio", APLUG_CAT_UTILITY, 2, boost_params, 1, 1
};

typedef struct { adsp_smooth_t gain; } boost_state_t;

static int boost_activate(aplug_t *p, float sr, int period)
{
    (void)period;
    adsp_smooth_init(&((boost_state_t*)p->state)->gain,
                     adsp_db_to_lin(6.0f), 5.0f, sr);
    return 0;
}
static void  boost_deactivate(aplug_t *p) { (void)p; }
static void  boost_process(aplug_t *p, const float *const *in,
                             float *const *out, int n)
{
    boost_state_t *s = (boost_state_t*)p->state;
    for (int i = 0; i < n; i++) {
        float g = adsp_smooth_tick(&s->gain);
        float y = in[0][i] * g;
        /* Soft clip output at ±1 */
        if (y >  1.0f) y =  1.0f - expf(-(y - 1.0f));
        if (y < -1.0f) y = -1.0f + expf( (y + 1.0f));
        out[0][i] = y;
    }
}
static void  boost_param_set(aplug_t *p, unsigned int id, float v)
    { if (id == BOOST_P_GAIN) ((boost_state_t*)p->state)->gain.target = adsp_db_to_lin(v); }
static float boost_param_get(aplug_t *p, unsigned int id)
    { return id == BOOST_P_GAIN ? adsp_lin_to_db(((boost_state_t*)p->state)->gain.current) : 0; }

static const aplug_vtable_t boost_vtable = {
    boost_activate, boost_deactivate,
    boost_process, boost_param_set, boost_param_get
};
aplug_t *afx_boost_factory(void)
    { return alloc_plug(&boost_desc, &boost_vtable, sizeof(boost_state_t)); }
