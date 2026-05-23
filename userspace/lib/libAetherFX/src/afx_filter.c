/*
 * AetherOS — libAetherFX: Filter effects (Phase 8.7)
 * File: userspace/lib/libAetherFX/src/afx_filter.c
 *
 * EQ4       — 4-band parametric equalizer (lo-shelf, 2×peaking, hi-shelf)
 * Wah       — expression-pedal controlled bandpass sweep (Crybaby model)
 * Auto-Wah  — envelope-follower-driven wah
 */

#include "aplug.h"
#include "adsp.h"
#include "afx.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

static aplug_t *alloc_plug(const aplug_descriptor_t *d,
                            const aplug_vtable_t *v, int sz)
{
    aplug_t *p = (aplug_t*)malloc(sizeof(aplug_t) + sz);
    if (!p) return NULL;
    p->desc = d; p->vtable = v;
    p->state = (char*)p + sizeof(aplug_t);
    memset(p->state, 0, (size_t)sz);
    return p;
}

/* ════════════════════════════════════════════════════════════════════════
 * 4-Band Parametric EQ
 * Band 0: low shelf  100 Hz  ±12 dB
 * Band 1: peaking    400 Hz  ±12 dB  Q=0.7
 * Band 2: peaking   2500 Hz  ±12 dB  Q=0.7
 * Band 3: high shelf 8000 Hz ±12 dB
 * ════════════════════════════════════════════════════════════════════════ */

enum { EQ_P_BYPASS=0,
       EQ_P_LO_GAIN, EQ_P_LO_FREQ,
       EQ_P_LMI_GAIN, EQ_P_LMI_FREQ, EQ_P_LMI_Q,
       EQ_P_HMI_GAIN, EQ_P_HMI_FREQ, EQ_P_HMI_Q,
       EQ_P_HI_GAIN, EQ_P_HI_FREQ };
static const aplug_param_t eq4_params[] = {
    { EQ_P_BYPASS, "Bypass","",0,1,0,APLUG_PARAM_BYPASS|APLUG_PARAM_TOGGLE },
    { EQ_P_LO_GAIN, "Lo Gain","dB",-12,12,0,APLUG_PARAM_AUTOMATABLE },
    { EQ_P_LO_FREQ, "Lo Freq","Hz",40,500,100,APLUG_PARAM_AUTOMATABLE|APLUG_PARAM_LOG },
    { EQ_P_LMI_GAIN,"LMid Gain","dB",-12,12,0,APLUG_PARAM_AUTOMATABLE },
    { EQ_P_LMI_FREQ,"LMid Freq","Hz",100,2000,400,APLUG_PARAM_AUTOMATABLE|APLUG_PARAM_LOG },
    { EQ_P_LMI_Q,   "LMid Q","",0.3f,4,0.7f,APLUG_PARAM_AUTOMATABLE },
    { EQ_P_HMI_GAIN,"HMid Gain","dB",-12,12,0,APLUG_PARAM_AUTOMATABLE },
    { EQ_P_HMI_FREQ,"HMid Freq","Hz",800,8000,2500,APLUG_PARAM_AUTOMATABLE|APLUG_PARAM_LOG },
    { EQ_P_HMI_Q,   "HMid Q","",0.3f,4,0.7f,APLUG_PARAM_AUTOMATABLE },
    { EQ_P_HI_GAIN, "Hi Gain","dB",-12,12,0,APLUG_PARAM_AUTOMATABLE },
    { EQ_P_HI_FREQ, "Hi Freq","Hz",2000,16000,8000,APLUG_PARAM_AUTOMATABLE|APLUG_PARAM_LOG },
};
static const aplug_descriptor_t eq4_desc = {
    APLUG_VERSION(1,0,0), "os.aether.eq4", "4-Band Parametric EQ",
    "AetherOS Audio", APLUG_CAT_FILTER, 11, eq4_params, 1, 1
};

typedef struct {
    adsp_biquad_t lo, lmid, hmid, hi;
    float lo_gain, lo_freq;
    float lmi_gain, lmi_freq, lmi_q;
    float hmi_gain, hmi_freq, hmi_q;
    float hi_gain, hi_freq;
    float sr;
    int   dirty;
} eq4_state_t;

static void eq4_rebuild(eq4_state_t *s)
{
    adsp_biquad_loshelf(&s->lo,   s->lo_freq, s->lo_gain,  s->sr);
    adsp_biquad_peaking(&s->lmid, s->lmi_freq, s->lmi_q, s->lmi_gain, s->sr);
    adsp_biquad_peaking(&s->hmid, s->hmi_freq, s->hmi_q, s->hmi_gain, s->sr);
    adsp_biquad_hishelf(&s->hi,   s->hi_freq,  s->hi_gain,  s->sr);
    s->dirty = 0;
}

static int eq4_activate(aplug_t *p, float sr, int period)
{
    (void)period;
    eq4_state_t *s = (eq4_state_t*)p->state;
    s->sr = sr;
    s->lo_freq = 100; s->lo_gain = 0;
    s->lmi_freq = 400; s->lmi_gain = 0; s->lmi_q = 0.7f;
    s->hmi_freq = 2500; s->hmi_gain = 0; s->hmi_q = 0.7f;
    s->hi_freq = 8000; s->hi_gain = 0;
    s->dirty = 1; eq4_rebuild(s);
    return 0;
}
static void eq4_deactivate(aplug_t *p) { (void)p; }

static void eq4_process(aplug_t *p, const float *const *in,
                         float *const *out, int n)
{
    eq4_state_t *s = (eq4_state_t*)p->state;
    if (s->dirty) eq4_rebuild(s);
    adsp_biquad_t chain[4] = {s->lo, s->lmid, s->hmid, s->hi};
    adsp_biquad_process_x4(chain, out[0], in[0], n);
    s->lo = chain[0]; s->lmid = chain[1]; s->hmid = chain[2]; s->hi = chain[3];
}

static void eq4_param_set(aplug_t *p, unsigned int id, float v)
{
    eq4_state_t *s = (eq4_state_t*)p->state;
    switch (id) {
    case EQ_P_LO_GAIN:  s->lo_gain  = v; break;
    case EQ_P_LO_FREQ:  s->lo_freq  = v; break;
    case EQ_P_LMI_GAIN: s->lmi_gain = v; break;
    case EQ_P_LMI_FREQ: s->lmi_freq = v; break;
    case EQ_P_LMI_Q:    s->lmi_q    = v; break;
    case EQ_P_HMI_GAIN: s->hmi_gain = v; break;
    case EQ_P_HMI_FREQ: s->hmi_freq = v; break;
    case EQ_P_HMI_Q:    s->hmi_q    = v; break;
    case EQ_P_HI_GAIN:  s->hi_gain  = v; break;
    case EQ_P_HI_FREQ:  s->hi_freq  = v; break;
    }
    s->dirty = 1;
}
static float eq4_param_get(aplug_t *p, unsigned int id)
{
    eq4_state_t *s = (eq4_state_t*)p->state;
    switch(id) {
    case EQ_P_LO_GAIN:  return s->lo_gain;
    case EQ_P_LO_FREQ:  return s->lo_freq;
    case EQ_P_LMI_GAIN: return s->lmi_gain;
    default: return 0.0f;
    }
}

static const aplug_vtable_t eq4_vtable = {
    eq4_activate, eq4_deactivate, eq4_process, eq4_param_set, eq4_param_get
};
aplug_t *afx_eq4_factory(void)
    { return alloc_plug(&eq4_desc, &eq4_vtable, sizeof(eq4_state_t)); }

/* ════════════════════════════════════════════════════════════════════════
 * Wah (Crybaby: Q-peaked bandpass from ~400 Hz to ~2.2 kHz)
 * ════════════════════════════════════════════════════════════════════════ */

enum { WAH_P_BYPASS=0, WAH_P_PEDAL, WAH_P_Q };
static const aplug_param_t wah_params[] = {
    { WAH_P_BYPASS,"Bypass","",0,1,0,APLUG_PARAM_BYPASS|APLUG_PARAM_TOGGLE },
    { WAH_P_PEDAL, "Pedal", "",0,1,0.5f,APLUG_PARAM_AUTOMATABLE|APLUG_PARAM_SMOOTHED },
    { WAH_P_Q,     "Q",     "",1,8,3.0f,APLUG_PARAM_AUTOMATABLE },
};
static const aplug_descriptor_t wah_desc = {
    APLUG_VERSION(1,0,0), "os.aether.wah", "Wah Pedal",
    "AetherOS Audio", APLUG_CAT_FILTER, 3, wah_params, 1, 1
};

typedef struct {
    adsp_smooth_t pedal;
    adsp_biquad_t bpf;
    float q, sr;
} wah_state_t;

static int wah_activate(aplug_t *p, float sr, int period)
{
    (void)period;
    wah_state_t *s = (wah_state_t*)p->state;
    s->sr = sr; s->q = 3.0f;
    adsp_smooth_init(&s->pedal, 0.5f, 5.0f, sr);
    adsp_biquad_bpf(&s->bpf, 800.0f, s->q, sr);
    return 0;
}
static void wah_deactivate(aplug_t *p) { (void)p; }

static void wah_process(aplug_t *p, const float *const *in,
                         float *const *out, int n)
{
    wah_state_t *s = (wah_state_t*)p->state;
    for (int i = 0; i < n; i++) {
        float ped = adsp_smooth_tick(&s->pedal);
        /* Pedal 0..1 → fc 400..2200 Hz (log sweep) */
        float fc = 400.0f * powf(5.5f, ped);
        adsp_biquad_bpf(&s->bpf, fc, s->q, s->sr);
        float y;
        adsp_biquad_process(&s->bpf, &y, &in[0][i], 1);
        out[0][i] = y * 2.0f;   /* compensate for BPF insertion loss */
    }
}

static void wah_param_set(aplug_t *p, unsigned int id, float v)
{
    wah_state_t *s = (wah_state_t*)p->state;
    if (id == WAH_P_PEDAL) s->pedal.target = v;
    if (id == WAH_P_Q)     s->q = v;
}
static float wah_param_get(aplug_t *p, unsigned int id)
{
    wah_state_t *s = (wah_state_t*)p->state;
    return id == WAH_P_PEDAL ? s->pedal.current : (id == WAH_P_Q ? s->q : 0);
}

static const aplug_vtable_t wah_vtable = {
    wah_activate, wah_deactivate, wah_process, wah_param_set, wah_param_get
};
aplug_t *afx_wah_factory(void)
    { return alloc_plug(&wah_desc, &wah_vtable, sizeof(wah_state_t)); }

/* ════════════════════════════════════════════════════════════════════════
 * Auto-Wah (envelope-controlled)
 * ════════════════════════════════════════════════════════════════════════ */

enum { AWH_P_BYPASS=0, AWH_P_SENS, AWH_P_Q, AWH_P_SPEED };
static const aplug_param_t awh_params[] = {
    { AWH_P_BYPASS,"Bypass","",0,1,0,APLUG_PARAM_BYPASS|APLUG_PARAM_TOGGLE },
    { AWH_P_SENS,  "Sens",  "",0,1,0.5f,APLUG_PARAM_AUTOMATABLE },
    { AWH_P_Q,     "Q",     "",1,8,3.0f,APLUG_PARAM_AUTOMATABLE },
    { AWH_P_SPEED, "Speed", "ms",10,500,100,APLUG_PARAM_AUTOMATABLE },
};
static const aplug_descriptor_t awh_desc = {
    APLUG_VERSION(1,0,0), "os.aether.autowah", "Auto-Wah",
    "AetherOS Audio", APLUG_CAT_FILTER, 4, awh_params, 1, 1
};

typedef struct {
    adsp_env_t    env;
    adsp_smooth_t fc_smooth;
    adsp_biquad_t bpf;
    float sens, q, sr;
} awh_state_t;

static int awh_activate(aplug_t *p, float sr, int period)
{
    (void)period;
    awh_state_t *s = (awh_state_t*)p->state;
    s->sr = sr; s->sens = 0.5f; s->q = 3.0f;
    adsp_env_init(&s->env, 5.0f, 100.0f, sr);
    adsp_smooth_init(&s->fc_smooth, 800.0f, 30.0f, sr);
    adsp_biquad_bpf(&s->bpf, 800.0f, 3.0f, sr);
    return 0;
}
static void awh_deactivate(aplug_t *p) { (void)p; }

static void awh_process(aplug_t *p, const float *const *in,
                         float *const *out, int n)
{
    awh_state_t *s = (awh_state_t*)p->state;
    for (int i = 0; i < n; i++) {
        float env = adsp_env_peak(&s->env, in[0][i]);
        float ped = env * s->sens * 4.0f;
        if (ped > 1.0f) ped = 1.0f;
        s->fc_smooth.target = 400.0f * powf(5.5f, ped);
        float fc = adsp_smooth_tick(&s->fc_smooth);
        adsp_biquad_bpf(&s->bpf, fc, s->q, s->sr);
        float y;
        adsp_biquad_process(&s->bpf, &y, &in[0][i], 1);
        out[0][i] = y * 2.0f;
    }
}

static void awh_param_set(aplug_t *p, unsigned int id, float v)
{
    awh_state_t *s = (awh_state_t*)p->state;
    if (id == AWH_P_SENS)  s->sens = v;
    if (id == AWH_P_Q)     s->q    = v;
    if (id == AWH_P_SPEED) { adsp_env_init(&s->env, 5.0f, v, s->sr);
                              adsp_smooth_init(&s->fc_smooth, s->fc_smooth.current, v*0.3f, s->sr); }
}
static float awh_param_get(aplug_t *p, unsigned int id)
{
    awh_state_t *s = (awh_state_t*)p->state;
    if (id == AWH_P_SENS) return s->sens;
    if (id == AWH_P_Q)    return s->q;
    return 0.0f;
}

static const aplug_vtable_t awh_vtable = {
    awh_activate, awh_deactivate, awh_process, awh_param_set, awh_param_get
};
aplug_t *afx_autowah_factory(void)
    { return alloc_plug(&awh_desc, &awh_vtable, sizeof(awh_state_t)); }
