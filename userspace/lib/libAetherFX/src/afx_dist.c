/*
 * AetherOS — libAetherFX: Distortion effects (Phase 8.7)
 * File: userspace/lib/libAetherFX/src/afx_dist.c
 *
 * Overdrive  — TS-808 Tube Screamer emulation:
 *              soft-clip waveshaper, 720Hz tone filter, input buffer.
 * Distortion — DS-1 style: hard/asymmetric clip + RC filter network.
 * Fuzz       — Big Muff π: two cascaded clipping stages + tone stack.
 */

#include "aplug.h"
#include "adsp.h"
#include "afx.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

static aplug_t *alloc_plug(const aplug_descriptor_t *desc,
                            const aplug_vtable_t     *vtable,
                            int state_size)
{
    aplug_t *p = (aplug_t *)malloc(sizeof(aplug_t) + state_size);
    if (!p) return NULL;
    p->desc = desc; p->vtable = vtable;
    p->state = (char *)p + sizeof(aplug_t);
    memset(p->state, 0, (size_t)state_size);
    return p;
}

/* ── Soft clip (symmetric tanh-based) ────────────────────────────────── */
static inline float soft_clip(float x)
{
    if (x >  3.0f) return  1.0f;
    if (x < -3.0f) return -1.0f;
    float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

/* ── Asymmetric clip (diode bridge) ──────────────────────────────────── */
static inline float asym_clip(float x)
{
    if (x >= 0.0f) { float g = 1.0f - expf(-x * 4.0f); return g > 1.0f ? 1.0f : g; }
    return -((-x) / (1.0f + (-x)));
}

/* ════════════════════════════════════════════════════════════════════════
 * Overdrive (TS-808)
 * ════════════════════════════════════════════════════════════════════════ */

enum { OD_P_BYPASS=0, OD_P_DRIVE, OD_P_TONE, OD_P_LEVEL };
static const aplug_param_t od_params[] = {
    { OD_P_BYPASS,"Bypass","",0,1,0, APLUG_PARAM_BYPASS|APLUG_PARAM_TOGGLE },
    { OD_P_DRIVE, "Drive", "", 0,1,0.5f, APLUG_PARAM_AUTOMATABLE|APLUG_PARAM_SMOOTHED },
    { OD_P_TONE,  "Tone",  "", 0,1,0.5f, APLUG_PARAM_AUTOMATABLE|APLUG_PARAM_SMOOTHED },
    { OD_P_LEVEL, "Level", "", 0,1,0.5f, APLUG_PARAM_AUTOMATABLE|APLUG_PARAM_SMOOTHED },
};
static const aplug_descriptor_t od_desc = {
    APLUG_VERSION(1,0,0), "os.aether.overdrive", "Tube Screamer",
    "AetherOS Audio", APLUG_CAT_DISTORTION, 4, od_params, 1, 1
};

typedef struct {
    adsp_smooth_t drive, tone_s, level;
    adsp_biquad_t hp_in;    /* 720 Hz HP input buffer */
    adsp_biquad_t lp_tone;  /* tone control LP */
    adsp_biquad_t hp_tone;  /* tone control HP */
    float sr;
} od_state_t;

static int od_activate(aplug_t *p, float sr, int period)
{
    (void)period;
    od_state_t *s = (od_state_t*)p->state;
    s->sr = sr;
    adsp_smooth_init(&s->drive, 0.5f, 5.0f, sr);
    adsp_smooth_init(&s->tone_s, 0.5f, 5.0f, sr);
    adsp_smooth_init(&s->level, adsp_db_to_lin(-6.0f), 5.0f, sr);
    adsp_biquad_hpf(&s->hp_in, 720.0f, 0.707f, sr);
    adsp_biquad_lpf(&s->lp_tone, 3200.0f, 0.707f, sr);
    adsp_biquad_hpf(&s->hp_tone, 720.0f, 0.707f, sr);
    return 0;
}
static void od_deactivate(aplug_t *p) { (void)p; }

static void od_process(aplug_t *p, const float *const *in,
                        float *const *out, int n)
{
    od_state_t *s = (od_state_t*)p->state;
    for (int i = 0; i < n; i++) {
        float x     = in[0][i];
        float drive = adsp_smooth_tick(&s->drive);
        float tone  = adsp_smooth_tick(&s->tone_s);
        float level = adsp_smooth_tick(&s->level);

        /* Input HP filter (removes very low frequencies before clipping) */
        float hp;
        adsp_biquad_process(&s->hp_in, &hp, &x, 1);

        /* Drive gain + soft clip */
        float gain = 1.0f + drive * 99.0f;   /* 1..100 */
        float clipped = soft_clip(hp * gain * 0.1f);

        /* Tone: blend LP (warm) and HP (bright) */
        float lp_out, hp_out;
        adsp_biquad_process(&s->lp_tone, &lp_out, &clipped, 1);
        adsp_biquad_process(&s->hp_tone, &hp_out, &clipped, 1);
        float toned = lp_out * (1.0f - tone) + hp_out * tone;

        out[0][i] = toned * level;
    }
}

static void od_param_set(aplug_t *p, unsigned int id, float v)
{
    od_state_t *s = (od_state_t*)p->state;
    if (id == OD_P_DRIVE) s->drive.target  = v;
    if (id == OD_P_TONE)  s->tone_s.target = v;
    if (id == OD_P_LEVEL) s->level.target  = v;
}
static float od_param_get(aplug_t *p, unsigned int id)
{
    od_state_t *s = (od_state_t*)p->state;
    if (id == OD_P_DRIVE) return s->drive.current;
    if (id == OD_P_TONE)  return s->tone_s.current;
    if (id == OD_P_LEVEL) return s->level.current;
    return 0.0f;
}

static const aplug_vtable_t od_vtable = {
    od_activate, od_deactivate, od_process, od_param_set, od_param_get
};
aplug_t *afx_overdrive_factory(void)
    { return alloc_plug(&od_desc, &od_vtable, sizeof(od_state_t)); }

/* ════════════════════════════════════════════════════════════════════════
 * Distortion (DS-1)
 * ════════════════════════════════════════════════════════════════════════ */

enum { DS_P_BYPASS=0, DS_P_DIST, DS_P_TONE, DS_P_LEVEL };
static const aplug_param_t ds_params[] = {
    { DS_P_BYPASS,"Bypass","",0,1,0, APLUG_PARAM_BYPASS|APLUG_PARAM_TOGGLE },
    { DS_P_DIST,  "Dist",  "", 0,1,0.5f, APLUG_PARAM_AUTOMATABLE|APLUG_PARAM_SMOOTHED },
    { DS_P_TONE,  "Tone",  "", 0,1,0.5f, APLUG_PARAM_AUTOMATABLE|APLUG_PARAM_SMOOTHED },
    { DS_P_LEVEL, "Level", "", 0,1,0.5f, APLUG_PARAM_AUTOMATABLE|APLUG_PARAM_SMOOTHED },
};
static const aplug_descriptor_t ds_desc = {
    APLUG_VERSION(1,0,0), "os.aether.distortion", "DS-1 Distortion",
    "AetherOS Audio", APLUG_CAT_DISTORTION, 4, ds_params, 1, 1
};

typedef struct {
    adsp_smooth_t dist, tone_s, level;
    adsp_biquad_t tone_bq;
    float sr;
} ds_state_t;

static int ds_activate(aplug_t *p, float sr, int period)
{
    (void)period;
    ds_state_t *s = (ds_state_t*)p->state;
    s->sr = sr;
    adsp_smooth_init(&s->dist,  0.5f, 5.0f, sr);
    adsp_smooth_init(&s->tone_s, 0.5f, 5.0f, sr);
    adsp_smooth_init(&s->level, 0.5f, 5.0f, sr);
    adsp_biquad_lpf(&s->tone_bq, 2500.0f, 0.707f, sr);
    return 0;
}
static void ds_deactivate(aplug_t *p) { (void)p; }

static void ds_process(aplug_t *p, const float *const *in,
                        float *const *out, int n)
{
    ds_state_t *s = (ds_state_t*)p->state;
    for (int i = 0; i < n; i++) {
        float dist  = adsp_smooth_tick(&s->dist);
        float tone  = adsp_smooth_tick(&s->tone_s);
        float level = adsp_smooth_tick(&s->level);

        float gain = 1.0f + dist * 300.0f;
        float clipped = asym_clip(in[0][i] * gain * 0.05f);

        float toned;
        /* Tone control: -3kHz LP blend */
        adsp_biquad_lpf(&s->tone_bq, 500.0f + tone * 6000.0f, 0.707f, s->sr);
        adsp_biquad_process(&s->tone_bq, &toned, &clipped, 1);

        out[0][i] = toned * level;
    }
}

static void ds_param_set(aplug_t *p, unsigned int id, float v)
{
    ds_state_t *s = (ds_state_t*)p->state;
    if (id == DS_P_DIST)  s->dist.target   = v;
    if (id == DS_P_TONE)  s->tone_s.target = v;
    if (id == DS_P_LEVEL) s->level.target  = v;
}
static float ds_param_get(aplug_t *p, unsigned int id)
{
    ds_state_t *s = (ds_state_t*)p->state;
    if (id == DS_P_DIST)  return s->dist.current;
    if (id == DS_P_TONE)  return s->tone_s.current;
    if (id == DS_P_LEVEL) return s->level.current;
    return 0.0f;
}

static const aplug_vtable_t ds_vtable = {
    ds_activate, ds_deactivate, ds_process, ds_param_set, ds_param_get
};
aplug_t *afx_distortion_factory(void)
    { return alloc_plug(&ds_desc, &ds_vtable, sizeof(ds_state_t)); }

/* ════════════════════════════════════════════════════════════════════════
 * Fuzz (Big Muff π)
 * ════════════════════════════════════════════════════════════════════════ */

enum { FUZZ_P_BYPASS=0, FUZZ_P_SUSTAIN, FUZZ_P_TONE, FUZZ_P_VOLUME };
static const aplug_param_t fuzz_params[] = {
    { FUZZ_P_BYPASS,  "Bypass",  "",0,1,0, APLUG_PARAM_BYPASS|APLUG_PARAM_TOGGLE },
    { FUZZ_P_SUSTAIN, "Sustain", "",0,1,0.7f, APLUG_PARAM_AUTOMATABLE|APLUG_PARAM_SMOOTHED },
    { FUZZ_P_TONE,    "Tone",    "",0,1,0.5f, APLUG_PARAM_AUTOMATABLE|APLUG_PARAM_SMOOTHED },
    { FUZZ_P_VOLUME,  "Volume",  "",0,1,0.5f, APLUG_PARAM_AUTOMATABLE|APLUG_PARAM_SMOOTHED },
};
static const aplug_descriptor_t fuzz_desc = {
    APLUG_VERSION(1,0,0), "os.aether.fuzz", "Big Muff Fuzz",
    "AetherOS Audio", APLUG_CAT_DISTORTION, 4, fuzz_params, 1, 1
};

typedef struct {
    adsp_smooth_t sustain, tone_s, volume;
    adsp_biquad_t lp1, hp1, tone_lp, tone_hp;
    float sr;
} fuzz_state_t;

static int fuzz_activate(aplug_t *p, float sr, int period)
{
    (void)period;
    fuzz_state_t *s = (fuzz_state_t*)p->state;
    s->sr = sr;
    adsp_smooth_init(&s->sustain, 0.7f, 5.0f, sr);
    adsp_smooth_init(&s->tone_s,  0.5f, 5.0f, sr);
    adsp_smooth_init(&s->volume,  0.5f, 5.0f, sr);
    adsp_biquad_hpf(&s->hp1,  300.0f, 0.5f, sr);   /* input HP */
    adsp_biquad_lpf(&s->lp1, 4000.0f, 0.5f, sr);   /* between stages */
    adsp_biquad_lpf(&s->tone_lp, 2200.0f, 0.707f, sr);
    adsp_biquad_hpf(&s->tone_hp,  700.0f, 0.707f, sr);
    return 0;
}
static void fuzz_deactivate(aplug_t *p) { (void)p; }

static void fuzz_process(aplug_t *p, const float *const *in,
                          float *const *out, int n)
{
    fuzz_state_t *s = (fuzz_state_t*)p->state;
    for (int i = 0; i < n; i++) {
        float sus  = adsp_smooth_tick(&s->sustain);
        float tone = adsp_smooth_tick(&s->tone_s);
        float vol  = adsp_smooth_tick(&s->volume);

        float gain = 1.0f + sus * 500.0f;
        float x = in[0][i];

        /* Stage 1: HP → clip */
        float hp1; adsp_biquad_process(&s->hp1, &hp1, &x, 1);
        float c1 = soft_clip(hp1 * gain * 0.02f);

        /* Interstage LP */
        float lp1; adsp_biquad_process(&s->lp1, &lp1, &c1, 1);

        /* Stage 2: second clip */
        float c2 = soft_clip(lp1 * gain * 0.1f);

        /* Tone stack: blend LP (warm) and HP (bright) */
        float lp_out, hp_out;
        adsp_biquad_process(&s->tone_lp, &lp_out, &c2, 1);
        adsp_biquad_process(&s->tone_hp, &hp_out, &c2, 1);

        out[0][i] = (lp_out * (1.0f - tone) + hp_out * tone) * vol;
    }
}

static void fuzz_param_set(aplug_t *p, unsigned int id, float v)
{
    fuzz_state_t *s = (fuzz_state_t*)p->state;
    if (id == FUZZ_P_SUSTAIN) s->sustain.target = v;
    if (id == FUZZ_P_TONE)    s->tone_s.target  = v;
    if (id == FUZZ_P_VOLUME)  s->volume.target  = v;
}
static float fuzz_param_get(aplug_t *p, unsigned int id)
{
    fuzz_state_t *s = (fuzz_state_t*)p->state;
    if (id == FUZZ_P_SUSTAIN) return s->sustain.current;
    if (id == FUZZ_P_TONE)    return s->tone_s.current;
    if (id == FUZZ_P_VOLUME)  return s->volume.current;
    return 0.0f;
}

static const aplug_vtable_t fuzz_vtable = {
    fuzz_activate, fuzz_deactivate, fuzz_process, fuzz_param_set, fuzz_param_get
};
aplug_t *afx_fuzz_factory(void)
    { return alloc_plug(&fuzz_desc, &fuzz_vtable, sizeof(fuzz_state_t)); }
