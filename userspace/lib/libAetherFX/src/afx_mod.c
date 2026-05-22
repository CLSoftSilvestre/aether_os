/*
 * AetherOS — libAetherFX: Modulation effects (Phase 8.7)
 * File: userspace/lib/libAetherFX/src/afx_mod.c
 *
 * Chorus   — BBD-style chorus: LFO-modulated delay line, wet/dry mix
 * Flanger  — Short-range modulated delay (0.5..15 ms), feedback
 * Phaser   — 4-stage all-pass phase shifter (MXR Phase 90 style)
 * Tremolo  — Amplitude modulation LFO (sine/triangle/square)
 * Vibrato  — Pitch modulation via delay LFO
 * Rotary   — Leslie speaker sim: horn + drum LFOs with crossfade
 */

#include "aplug.h"
#include "adsp.h"
#include "afx.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

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

/* ── Delay line helpers (circular buffer) ────────────────────────────── */

#define DELAY_MAX 8192   /* ~170ms at 48kHz */

static inline void delay_write(float *buf, int *pos, float x)
{
    buf[(*pos)++ & (DELAY_MAX - 1)] = x;
}

static inline float delay_read_lin(const float *buf, int pos, float delay_samps)
{
    int d   = (int)delay_samps;
    float f = delay_samps - (float)d;
    float a = buf[(pos - d - 1) & (DELAY_MAX - 1)];
    float b = buf[(pos - d - 2) & (DELAY_MAX - 1)];
    return a + f * (b - a);
}

/* ── LFO (sine) ──────────────────────────────────────────────────────── */

static inline float lfo_sine(float *phase, float rate, float sr)
{
    *phase += rate / sr;
    if (*phase > 1.0f) *phase -= 1.0f;
    return sinf(*phase * 2.0f * (float)M_PI);
}

/* ════════════════════════════════════════════════════════════════════════
 * Chorus
 * ════════════════════════════════════════════════════════════════════════ */

enum { CHO_P_BYPASS=0, CHO_P_RATE, CHO_P_DEPTH, CHO_P_MIX };
static const aplug_param_t cho_params[] = {
    { CHO_P_BYPASS,"Bypass","",0,1,0,APLUG_PARAM_BYPASS|APLUG_PARAM_TOGGLE },
    { CHO_P_RATE,  "Rate",  "Hz",0.1f,5,0.5f,APLUG_PARAM_AUTOMATABLE|APLUG_PARAM_SMOOTHED },
    { CHO_P_DEPTH, "Depth", "ms",0.5f,15,7.0f,APLUG_PARAM_AUTOMATABLE|APLUG_PARAM_SMOOTHED },
    { CHO_P_MIX,   "Mix",   "%",0,1,0.5f,APLUG_PARAM_AUTOMATABLE|APLUG_PARAM_SMOOTHED },
};
static const aplug_descriptor_t cho_desc = {
    APLUG_VERSION(1,0,0),"os.aether.chorus","Chorus",
    "AetherOS Audio",APLUG_CAT_MODULATION,4,cho_params,1,2
};
typedef struct {
    float buf[DELAY_MAX];
    int   pos;
    float phase;
    adsp_smooth_t rate, depth, mix;
    float sr;
} cho_state_t;

static int cho_activate(aplug_t *p, float sr, int period)
{
    (void)period;
    cho_state_t *s = (cho_state_t*)p->state;
    s->sr = sr;
    adsp_smooth_init(&s->rate,  0.5f, 10.0f, sr);
    adsp_smooth_init(&s->depth, 7.0f, 10.0f, sr);
    adsp_smooth_init(&s->mix,   0.5f, 10.0f, sr);
    return 0;
}
static void cho_deactivate(aplug_t *p) { (void)p; }

static void cho_process(aplug_t *p, const float *const *in,
                         float *const *out, int n)
{
    cho_state_t *s = (cho_state_t*)p->state;
    for (int i = 0; i < n; i++) {
        float rate  = adsp_smooth_tick(&s->rate);
        float depth = adsp_smooth_tick(&s->depth);
        float mix   = adsp_smooth_tick(&s->mix);
        float x     = in[0][i];

        delay_write(s->buf, &s->pos, x);
        float lfo  = lfo_sine(&s->phase, rate, s->sr);
        float dsmp = (depth * 0.001f * s->sr) * (0.5f + 0.5f * lfo)
                     + s->sr * 0.005f;   /* min 5ms base delay */
        float wet  = delay_read_lin(s->buf, s->pos, dsmp);

        out[0][i] = x * (1.0f - mix) + wet * mix;
        out[1][i] = x * (1.0f - mix) - wet * mix;  /* stereo spread */
    }
}

static void cho_param_set(aplug_t *p, unsigned int id, float v)
{
    cho_state_t *s = (cho_state_t*)p->state;
    if (id == CHO_P_RATE)  s->rate.target  = v;
    if (id == CHO_P_DEPTH) s->depth.target = v;
    if (id == CHO_P_MIX)   s->mix.target   = v;
}
static float cho_param_get(aplug_t *p, unsigned int id)
{
    cho_state_t *s = (cho_state_t*)p->state;
    if (id == CHO_P_RATE)  return s->rate.current;
    if (id == CHO_P_DEPTH) return s->depth.current;
    if (id == CHO_P_MIX)   return s->mix.current;
    return 0.0f;
}
static const aplug_vtable_t cho_vtable = {
    cho_activate,cho_deactivate,cho_process,cho_param_set,cho_param_get
};
aplug_t *afx_chorus_factory(void)
    { return alloc_plug(&cho_desc,&cho_vtable,sizeof(cho_state_t)); }

/* ════════════════════════════════════════════════════════════════════════
 * Flanger
 * ════════════════════════════════════════════════════════════════════════ */

enum { FLA_P_BYPASS=0, FLA_P_RATE, FLA_P_DEPTH, FLA_P_FEEDBACK, FLA_P_MIX };
static const aplug_param_t fla_params[] = {
    { FLA_P_BYPASS,   "Bypass",  "",0,1,0,APLUG_PARAM_BYPASS|APLUG_PARAM_TOGGLE },
    { FLA_P_RATE,     "Rate",    "Hz",0.05f,5,0.3f,APLUG_PARAM_AUTOMATABLE|APLUG_PARAM_SMOOTHED },
    { FLA_P_DEPTH,    "Depth",   "ms",0.1f,7,3.5f,APLUG_PARAM_AUTOMATABLE|APLUG_PARAM_SMOOTHED },
    { FLA_P_FEEDBACK, "Feedback","%",-0.95f,0.95f,0.5f,APLUG_PARAM_AUTOMATABLE|APLUG_PARAM_SMOOTHED },
    { FLA_P_MIX,      "Mix",     "",0,1,0.5f,APLUG_PARAM_AUTOMATABLE|APLUG_PARAM_SMOOTHED },
};
static const aplug_descriptor_t fla_desc = {
    APLUG_VERSION(1,0,0),"os.aether.flanger","Flanger",
    "AetherOS Audio",APLUG_CAT_MODULATION,5,fla_params,1,1
};
typedef struct {
    float buf[DELAY_MAX];
    int   pos;
    float phase, fb_samp;
    adsp_smooth_t rate, depth, fb, mix;
    float sr;
} fla_state_t;

static int fla_activate(aplug_t *p, float sr, int period)
{
    (void)period;
    fla_state_t *s = (fla_state_t*)p->state;
    s->sr = sr;
    adsp_smooth_init(&s->rate,  0.3f,  10.0f, sr);
    adsp_smooth_init(&s->depth, 3.5f,  10.0f, sr);
    adsp_smooth_init(&s->fb,    0.5f,  10.0f, sr);
    adsp_smooth_init(&s->mix,   0.5f,  10.0f, sr);
    return 0;
}
static void fla_deactivate(aplug_t *p) { (void)p; }

static void fla_process(aplug_t *p, const float *const *in,
                         float *const *out, int n)
{
    fla_state_t *s = (fla_state_t*)p->state;
    for (int i = 0; i < n; i++) {
        float rate  = adsp_smooth_tick(&s->rate);
        float depth = adsp_smooth_tick(&s->depth);
        float fb    = adsp_smooth_tick(&s->fb);
        float mix   = adsp_smooth_tick(&s->mix);
        float x     = in[0][i];

        delay_write(s->buf, &s->pos, x + s->fb_samp * fb);
        float lfo  = lfo_sine(&s->phase, rate, s->sr);
        float dsmp = (depth * 0.001f * s->sr) * (0.5f + 0.5f * lfo) + 1.0f;
        float wet  = delay_read_lin(s->buf, s->pos, dsmp);
        s->fb_samp = wet;

        out[0][i] = x * (1.0f - mix) + wet * mix;
    }
}

static void fla_param_set(aplug_t *p, unsigned int id, float v)
{
    fla_state_t *s = (fla_state_t*)p->state;
    if (id == FLA_P_RATE)     s->rate.target  = v;
    if (id == FLA_P_DEPTH)    s->depth.target = v;
    if (id == FLA_P_FEEDBACK) s->fb.target    = v;
    if (id == FLA_P_MIX)      s->mix.target   = v;
}
static float fla_param_get(aplug_t *p, unsigned int id)
{
    fla_state_t *s = (fla_state_t*)p->state;
    if (id == FLA_P_RATE)  return s->rate.current;
    if (id == FLA_P_DEPTH) return s->depth.current;
    return 0.0f;
}
static const aplug_vtable_t fla_vtable = {
    fla_activate,fla_deactivate,fla_process,fla_param_set,fla_param_get
};
aplug_t *afx_flanger_factory(void)
    { return alloc_plug(&fla_desc,&fla_vtable,sizeof(fla_state_t)); }

/* ════════════════════════════════════════════════════════════════════════
 * Phaser (4-stage all-pass, MXR Phase 90)
 * ════════════════════════════════════════════════════════════════════════ */

enum { PHA_P_BYPASS=0, PHA_P_RATE, PHA_P_DEPTH, PHA_P_FEEDBACK, PHA_P_MIX };
static const aplug_param_t pha_params[] = {
    { PHA_P_BYPASS,  "Bypass",  "",0,1,0,APLUG_PARAM_BYPASS|APLUG_PARAM_TOGGLE },
    { PHA_P_RATE,    "Rate",    "Hz",0.05f,8,0.4f,APLUG_PARAM_AUTOMATABLE|APLUG_PARAM_SMOOTHED },
    { PHA_P_DEPTH,   "Depth",   "",0,1,0.8f,APLUG_PARAM_AUTOMATABLE|APLUG_PARAM_SMOOTHED },
    { PHA_P_FEEDBACK,"Feedback","",0,0.95f,0.7f,APLUG_PARAM_AUTOMATABLE|APLUG_PARAM_SMOOTHED },
    { PHA_P_MIX,     "Mix",     "",0,1,0.5f,APLUG_PARAM_AUTOMATABLE|APLUG_PARAM_SMOOTHED },
};
static const aplug_descriptor_t pha_desc = {
    APLUG_VERSION(1,0,0),"os.aether.phaser","Phaser",
    "AetherOS Audio",APLUG_CAT_MODULATION,5,pha_params,1,1
};
typedef struct {
    adsp_biquad_t ap[4];
    float phase, fb_samp;
    adsp_smooth_t rate, depth, fb, mix;
    float sr;
} pha_state_t;

static int pha_activate(aplug_t *p, float sr, int period)
{
    (void)period;
    pha_state_t *s = (pha_state_t*)p->state;
    s->sr = sr;
    adsp_smooth_init(&s->rate,  0.4f, 10.0f, sr);
    adsp_smooth_init(&s->depth, 0.8f, 10.0f, sr);
    adsp_smooth_init(&s->fb,    0.7f, 10.0f, sr);
    adsp_smooth_init(&s->mix,   0.5f, 10.0f, sr);
    for (int i = 0; i < 4; i++) adsp_biquad_allpass(&s->ap[i], 1000.0f, 0.5f, sr);
    return 0;
}
static void pha_deactivate(aplug_t *p) { (void)p; }

static void pha_process(aplug_t *p, const float *const *in,
                         float *const *out, int n)
{
    pha_state_t *s = (pha_state_t*)p->state;
    for (int i = 0; i < n; i++) {
        float rate  = adsp_smooth_tick(&s->rate);
        float depth = adsp_smooth_tick(&s->depth);
        float fb    = adsp_smooth_tick(&s->fb);
        float mix   = adsp_smooth_tick(&s->mix);

        float lfo = lfo_sine(&s->phase, rate, s->sr);
        float fc  = 400.0f + depth * (2000.0f * (0.5f + 0.5f * lfo));

        float y = in[0][i] + s->fb_samp * fb;
        for (int k = 0; k < 4; k++) {
            adsp_biquad_allpass(&s->ap[k], fc * (1.0f + k * 0.2f), 0.5f, s->sr);
            adsp_biquad_process(&s->ap[k], &y, &y, 1);
        }
        s->fb_samp = y;
        out[0][i] = in[0][i] * (1.0f - mix) + y * mix;
    }
}

static void pha_param_set(aplug_t *p, unsigned int id, float v)
{
    pha_state_t *s = (pha_state_t*)p->state;
    if (id == PHA_P_RATE)     s->rate.target  = v;
    if (id == PHA_P_DEPTH)    s->depth.target = v;
    if (id == PHA_P_FEEDBACK) s->fb.target    = v;
    if (id == PHA_P_MIX)      s->mix.target   = v;
}
static float pha_param_get(aplug_t *p, unsigned int id)
{
    pha_state_t *s = (pha_state_t*)p->state;
    if (id == PHA_P_RATE) return s->rate.current;
    return 0.0f;
}
static const aplug_vtable_t pha_vtable = {
    pha_activate,pha_deactivate,pha_process,pha_param_set,pha_param_get
};
aplug_t *afx_phaser_factory(void)
    { return alloc_plug(&pha_desc,&pha_vtable,sizeof(pha_state_t)); }

/* ════════════════════════════════════════════════════════════════════════
 * Tremolo
 * ════════════════════════════════════════════════════════════════════════ */

enum { TRE_P_BYPASS=0, TRE_P_RATE, TRE_P_DEPTH, TRE_P_SHAPE };
static const aplug_param_t tre_params[] = {
    { TRE_P_BYPASS,"Bypass","",0,1,0,APLUG_PARAM_BYPASS|APLUG_PARAM_TOGGLE },
    { TRE_P_RATE,  "Rate",  "Hz",0.5f,20,4.0f,APLUG_PARAM_AUTOMATABLE|APLUG_PARAM_SMOOTHED },
    { TRE_P_DEPTH, "Depth", "",0,1,0.7f,APLUG_PARAM_AUTOMATABLE|APLUG_PARAM_SMOOTHED },
    { TRE_P_SHAPE, "Shape", "",0,2,0,APLUG_PARAM_STEPPED },   /* 0=sine,1=tri,2=square */
};
static const aplug_descriptor_t tre_desc = {
    APLUG_VERSION(1,0,0),"os.aether.tremolo","Tremolo",
    "AetherOS Audio",APLUG_CAT_MODULATION,4,tre_params,1,1
};
typedef struct {
    adsp_smooth_t rate, depth;
    float phase, shape, sr;
} tre_state_t;

static int tre_activate(aplug_t *p, float sr, int period)
{
    (void)period;
    tre_state_t *s = (tre_state_t*)p->state;
    s->sr = sr;
    adsp_smooth_init(&s->rate,  4.0f, 10.0f, sr);
    adsp_smooth_init(&s->depth, 0.7f, 10.0f, sr);
    return 0;
}
static void tre_deactivate(aplug_t *p) { (void)p; }

static void tre_process(aplug_t *p, const float *const *in,
                         float *const *out, int n)
{
    tre_state_t *s = (tre_state_t*)p->state;
    for (int i = 0; i < n; i++) {
        float rate  = adsp_smooth_tick(&s->rate);
        float depth = adsp_smooth_tick(&s->depth);

        s->phase += rate / s->sr;
        if (s->phase > 1.0f) s->phase -= 1.0f;

        float lfo;
        int shape = (int)s->shape;
        if (shape == 1) {   /* triangle */
            lfo = 1.0f - 4.0f * fabsf(s->phase - 0.5f);
        } else if (shape == 2) {   /* square */
            lfo = s->phase < 0.5f ? 1.0f : -1.0f;
        } else {   /* sine */
            lfo = sinf(s->phase * 2.0f * (float)M_PI);
        }

        float gain = 1.0f - depth * 0.5f * (1.0f - lfo);
        out[0][i] = in[0][i] * gain;
    }
}

static void tre_param_set(aplug_t *p, unsigned int id, float v)
{
    tre_state_t *s = (tre_state_t*)p->state;
    if (id == TRE_P_RATE)  s->rate.target  = v;
    if (id == TRE_P_DEPTH) s->depth.target = v;
    if (id == TRE_P_SHAPE) s->shape = v;
}
static float tre_param_get(aplug_t *p, unsigned int id)
{
    tre_state_t *s = (tre_state_t*)p->state;
    if (id == TRE_P_RATE) return s->rate.current;
    return 0.0f;
}
static const aplug_vtable_t tre_vtable = {
    tre_activate,tre_deactivate,tre_process,tre_param_set,tre_param_get
};
aplug_t *afx_tremolo_factory(void)
    { return alloc_plug(&tre_desc,&tre_vtable,sizeof(tre_state_t)); }

/* ════════════════════════════════════════════════════════════════════════
 * Vibrato (pitch LFO via delay modulation)
 * ════════════════════════════════════════════════════════════════════════ */

enum { VIB_P_BYPASS=0, VIB_P_RATE, VIB_P_DEPTH };
static const aplug_param_t vib_params[] = {
    { VIB_P_BYPASS,"Bypass","",0,1,0,APLUG_PARAM_BYPASS|APLUG_PARAM_TOGGLE },
    { VIB_P_RATE,  "Rate",  "Hz",0.1f,8,3.0f,APLUG_PARAM_AUTOMATABLE|APLUG_PARAM_SMOOTHED },
    { VIB_P_DEPTH, "Depth", "cents",0,100,30.0f,APLUG_PARAM_AUTOMATABLE|APLUG_PARAM_SMOOTHED },
};
static const aplug_descriptor_t vib_desc = {
    APLUG_VERSION(1,0,0),"os.aether.vibrato","Vibrato",
    "AetherOS Audio",APLUG_CAT_MODULATION,3,vib_params,1,1
};
typedef struct {
    float buf[DELAY_MAX];
    int   pos;
    float phase;
    adsp_smooth_t rate, depth;
    float sr;
} vib_state_t;

static int vib_activate(aplug_t *p, float sr, int period)
{
    (void)period;
    vib_state_t *s = (vib_state_t*)p->state;
    s->sr = sr;
    adsp_smooth_init(&s->rate,  3.0f,  10.0f, sr);
    adsp_smooth_init(&s->depth, 30.0f, 10.0f, sr);
    return 0;
}
static void vib_deactivate(aplug_t *p) { (void)p; }

static void vib_process(aplug_t *p, const float *const *in,
                         float *const *out, int n)
{
    vib_state_t *s = (vib_state_t*)p->state;
    for (int i = 0; i < n; i++) {
        float rate  = adsp_smooth_tick(&s->rate);
        float depth_cents = adsp_smooth_tick(&s->depth);

        delay_write(s->buf, &s->pos, in[0][i]);

        float lfo  = lfo_sine(&s->phase, rate, s->sr);
        /* depth in cents → delay in samples: Δt = (2^(c/1200)-1) / f0
         * approximation: Δsmp ≈ depth_cents / 1200 * sr / 220 */
        float dsmp = (depth_cents / 1200.0f) * (s->sr / 220.0f)
                     * (0.5f + 0.5f * lfo) + s->sr * 0.004f;
        out[0][i] = delay_read_lin(s->buf, s->pos, dsmp);
    }
}

static void vib_param_set(aplug_t *p, unsigned int id, float v)
{
    vib_state_t *s = (vib_state_t*)p->state;
    if (id == VIB_P_RATE)  s->rate.target  = v;
    if (id == VIB_P_DEPTH) s->depth.target = v;
}
static float vib_param_get(aplug_t *p, unsigned int id)
{
    vib_state_t *s = (vib_state_t*)p->state;
    if (id == VIB_P_RATE) return s->rate.current;
    return 0.0f;
}
static const aplug_vtable_t vib_vtable = {
    vib_activate,vib_deactivate,vib_process,vib_param_set,vib_param_get
};
aplug_t *afx_vibrato_factory(void)
    { return alloc_plug(&vib_desc,&vib_vtable,sizeof(vib_state_t)); }

/* ════════════════════════════════════════════════════════════════════════
 * Rotary (Leslie speaker: horn LFO ~6 Hz, drum LFO ~1.6 Hz)
 * ════════════════════════════════════════════════════════════════════════ */

enum { ROT_P_BYPASS=0, ROT_P_SPEED, ROT_P_MIX };
static const aplug_param_t rot_params[] = {
    { ROT_P_BYPASS,"Bypass","",0,1,0,APLUG_PARAM_BYPASS|APLUG_PARAM_TOGGLE },
    { ROT_P_SPEED, "Speed", "Hz",0.2f,10,0.8f,APLUG_PARAM_AUTOMATABLE|APLUG_PARAM_SMOOTHED },
    { ROT_P_MIX,   "Mix",   "",0,1,0.7f,APLUG_PARAM_AUTOMATABLE|APLUG_PARAM_SMOOTHED },
};
static const aplug_descriptor_t rot_desc = {
    APLUG_VERSION(1,0,0),"os.aether.rotary","Rotary Speaker",
    "AetherOS Audio",APLUG_CAT_MODULATION,3,rot_params,1,2
};
typedef struct {
    float horn_buf[DELAY_MAX], drum_buf[DELAY_MAX];
    int   horn_pos, drum_pos;
    float horn_phase, drum_phase;
    adsp_smooth_t speed, mix;
    float sr;
} rot_state_t;

static int rot_activate(aplug_t *p, float sr, int period)
{
    (void)period;
    rot_state_t *s = (rot_state_t*)p->state;
    s->sr = sr;
    adsp_smooth_init(&s->speed, 0.8f, 200.0f, sr);  /* slow ramp */
    adsp_smooth_init(&s->mix,   0.7f, 10.0f, sr);
    return 0;
}
static void rot_deactivate(aplug_t *p) { (void)p; }

static void rot_process(aplug_t *p, const float *const *in,
                         float *const *out, int n)
{
    rot_state_t *s = (rot_state_t*)p->state;
    for (int i = 0; i < n; i++) {
        float spd = adsp_smooth_tick(&s->speed);
        float mix = adsp_smooth_tick(&s->mix);
        float x   = in[0][i];

        delay_write(s->horn_buf, &s->horn_pos, x);
        delay_write(s->drum_buf, &s->drum_pos, x);

        float horn_lfo = lfo_sine(&s->horn_phase, spd * 4.5f, s->sr);
        float drum_lfo = lfo_sine(&s->drum_phase, spd * 1.2f, s->sr);

        float horn_dsmp = s->sr * 0.003f * (1.0f + horn_lfo * 0.8f);
        float drum_dsmp = s->sr * 0.008f * (1.0f + drum_lfo * 0.4f);

        float horn = delay_read_lin(s->horn_buf, s->horn_pos, horn_dsmp);
        float drum = delay_read_lin(s->drum_buf, s->drum_pos, drum_dsmp);

        float wet = (horn + drum) * 0.5f;
        out[0][i] = x * (1.0f - mix) + wet * mix;
        out[1][i] = x * (1.0f - mix) - wet * mix;
    }
}

static void rot_param_set(aplug_t *p, unsigned int id, float v)
{
    rot_state_t *s = (rot_state_t*)p->state;
    if (id == ROT_P_SPEED) s->speed.target = v;
    if (id == ROT_P_MIX)   s->mix.target   = v;
}
static float rot_param_get(aplug_t *p, unsigned int id)
{
    rot_state_t *s = (rot_state_t*)p->state;
    if (id == ROT_P_SPEED) return s->speed.current;
    return 0.0f;
}
static const aplug_vtable_t rot_vtable = {
    rot_activate,rot_deactivate,rot_process,rot_param_set,rot_param_get
};
aplug_t *afx_rotary_factory(void)
    { return alloc_plug(&rot_desc,&rot_vtable,sizeof(rot_state_t)); }
