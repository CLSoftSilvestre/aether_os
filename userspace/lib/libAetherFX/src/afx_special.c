/*
 * AetherOS — libAetherFX: Special effects (Phase 8.7)
 * File: userspace/lib/libAetherFX/src/afx_special.c
 *
 * Pitch Shift — granular pitch shifter (2-grain overlap-add)
 * Octave      — octave-up (full-wave rectify + HP) + octave-down (1/2-wave)
 * Ring Mod    — ring modulator with sine carrier
 * Looper      — basic phrase looper (record/play/overdub, up to 30s)
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

/* ════════════════════════════════════════════════════════════════════════
 * Pitch Shift (granular, 2-grain)
 * ════════════════════════════════════════════════════════════════════════ */

#define PS_BUF  8192   /* grain buffer */
#define PS_GSIZ  512   /* grain size in samples */

enum { PS_P_BYPASS=0, PS_P_SEMITONES, PS_P_MIX };
static const aplug_param_t ps_params[] = {
    { PS_P_BYPASS,    "Bypass","",0,1,0,APLUG_PARAM_BYPASS|APLUG_PARAM_TOGGLE },
    { PS_P_SEMITONES, "Pitch","semi",-12,12,0,APLUG_PARAM_AUTOMATABLE|APLUG_PARAM_SMOOTHED },
    { PS_P_MIX,       "Mix",  "",0,1,1.0f,APLUG_PARAM_AUTOMATABLE|APLUG_PARAM_SMOOTHED },
};
static const aplug_descriptor_t ps_desc = {
    APLUG_VERSION(1,0,0),"os.aether.pitchshift","Pitch Shift",
    "AetherOS Audio",APLUG_CAT_PITCH,3,ps_params,1,1
};

typedef struct {
    float buf[PS_BUF];
    int   write_pos;
    float grain_pos[2];   /* 2 grain read heads */
    float grain_phase[2]; /* grain window phase 0..1 */
    adsp_smooth_t semitones, mix;
    float sr;
} ps_state_t;

static int ps_activate(aplug_t *p, float sr, int period)
{
    (void)period;
    ps_state_t *s = (ps_state_t*)p->state;
    s->sr = sr;
    adsp_smooth_init(&s->semitones, 0.0f, 20.0f, sr);
    adsp_smooth_init(&s->mix,       1.0f, 10.0f, sr);
    s->grain_pos[0] = 0; s->grain_pos[1] = PS_GSIZ / 2.0f;
    return 0;
}
static void ps_deactivate(aplug_t *p) { (void)p; }

static void ps_process(aplug_t *p, const float *const *in,
                        float *const *out, int n)
{
    ps_state_t *s = (ps_state_t*)p->state;
    for (int i = 0; i < n; i++) {
        float semi = adsp_smooth_tick(&s->semitones);
        float mix  = adsp_smooth_tick(&s->mix);
        float ratio = powf(2.0f, semi / 12.0f);  /* pitch ratio */

        /* Write to ring buffer */
        s->buf[s->write_pos & (PS_BUF-1)] = in[0][i];
        s->write_pos++;

        /* 2-grain overlap-add */
        float wet = 0.0f;
        for (int g = 0; g < 2; g++) {
            /* Hann window */
            float win = 0.5f * (1.0f - cosf(s->grain_phase[g] * 2.0f * (float)M_PI));

            /* Read from buffer at grain read position */
            int ri   = (int)s->grain_pos[g];
            float fr = s->grain_pos[g] - ri;
            float a  = s->buf[(s->write_pos - ri - 1) & (PS_BUF-1)];
            float b  = s->buf[(s->write_pos - ri - 2) & (PS_BUF-1)];
            wet += (a + fr * (b - a)) * win;

            /* Advance grain: read at ratio relative to write */
            s->grain_pos[g]   += ratio;
            s->grain_phase[g] += 1.0f / PS_GSIZ;
            if (s->grain_phase[g] >= 1.0f) {
                s->grain_phase[g] -= 1.0f;
                s->grain_pos[g]    = (float)(g * PS_GSIZ / 2) + 1.0f;
            }
        }
        wet *= 0.5f;   /* normalize 2 grains */

        out[0][i] = in[0][i] * (1.0f - mix) + wet * mix;
    }
}

static void ps_param_set(aplug_t *p, unsigned int id, float v)
{
    ps_state_t *s = (ps_state_t*)p->state;
    if (id == PS_P_SEMITONES) s->semitones.target = v;
    if (id == PS_P_MIX)       s->mix.target       = v;
}
static float ps_param_get(aplug_t *p, unsigned int id)
{
    ps_state_t *s = (ps_state_t*)p->state;
    if (id == PS_P_SEMITONES) return s->semitones.current;
    return 0.0f;
}
static const aplug_vtable_t ps_vtable = {
    ps_activate,ps_deactivate,ps_process,ps_param_set,ps_param_get
};
aplug_t *afx_pitchshift_factory(void)
    { return alloc_plug(&ps_desc,&ps_vtable,sizeof(ps_state_t)); }

/* ════════════════════════════════════════════════════════════════════════
 * Octave (Green Ringer + Sub-octave)
 * ════════════════════════════════════════════════════════════════════════ */

enum { OCT_P_BYPASS=0, OCT_P_OCT_UP, OCT_P_OCT_DOWN, OCT_P_DRY };
static const aplug_param_t oct_params[] = {
    { OCT_P_BYPASS,   "Bypass",  "",0,1,0,APLUG_PARAM_BYPASS|APLUG_PARAM_TOGGLE },
    { OCT_P_OCT_UP,   "Oct Up",  "",0,1,0.5f,APLUG_PARAM_AUTOMATABLE|APLUG_PARAM_SMOOTHED },
    { OCT_P_OCT_DOWN, "Oct Down","",0,1,0.0f,APLUG_PARAM_AUTOMATABLE|APLUG_PARAM_SMOOTHED },
    { OCT_P_DRY,      "Dry",    "",0,1,1.0f,APLUG_PARAM_AUTOMATABLE|APLUG_PARAM_SMOOTHED },
};
static const aplug_descriptor_t oct_desc = {
    APLUG_VERSION(1,0,0),"os.aether.octave","Octave",
    "AetherOS Audio",APLUG_CAT_PITCH,4,oct_params,1,1
};
typedef struct {
    adsp_smooth_t up, down, dry;
    adsp_biquad_t hp_up;    /* HP to remove DC from rectified signal */
    float prev_sign;        /* for sub-octave flip-flop */
} oct_state_t;

static int oct_activate(aplug_t *p, float sr, int period)
{
    (void)period;
    oct_state_t *s = (oct_state_t*)p->state;
    adsp_smooth_init(&s->up,   0.5f, 5.0f, sr);
    adsp_smooth_init(&s->down, 0.0f, 5.0f, sr);
    adsp_smooth_init(&s->dry,  1.0f, 5.0f, sr);
    adsp_biquad_hpf(&s->hp_up, 80.0f, 0.5f, sr);
    s->prev_sign = 1.0f;
    return 0;
}
static void oct_deactivate(aplug_t *p) { (void)p; }

static void oct_process(aplug_t *p, const float *const *in,
                         float *const *out, int n)
{
    oct_state_t *s = (oct_state_t*)p->state;
    for (int i = 0; i < n; i++) {
        float up   = adsp_smooth_tick(&s->up);
        float down = adsp_smooth_tick(&s->down);
        float dry  = adsp_smooth_tick(&s->dry);
        float x    = in[0][i];

        /* Oct up: full-wave rectify → doubles frequency */
        float rect = x < 0 ? -x : x;
        float hp_rect;
        adsp_biquad_process(&s->hp_up, &hp_rect, &rect, 1);

        /* Sub-octave: toggle polarity on each zero-crossing */
        float sign = x >= 0 ? 1.0f : -1.0f;
        float sub  = x;
        if (sign != s->prev_sign) {
            s->prev_sign = sign;
            sub = -sub;
        }

        out[0][i] = x * dry + hp_rect * up + sub * down;
    }
}

static void oct_param_set(aplug_t *p, unsigned int id, float v)
{
    oct_state_t *s = (oct_state_t*)p->state;
    if (id == OCT_P_OCT_UP)   s->up.target   = v;
    if (id == OCT_P_OCT_DOWN) s->down.target = v;
    if (id == OCT_P_DRY)      s->dry.target  = v;
}
static float oct_param_get(aplug_t *p, unsigned int id)
{
    oct_state_t *s = (oct_state_t*)p->state;
    if (id == OCT_P_OCT_UP) return s->up.current;
    return 0.0f;
}
static const aplug_vtable_t oct_vtable = {
    oct_activate,oct_deactivate,oct_process,oct_param_set,oct_param_get
};
aplug_t *afx_octave_factory(void)
    { return alloc_plug(&oct_desc,&oct_vtable,sizeof(oct_state_t)); }

/* ════════════════════════════════════════════════════════════════════════
 * Ring Modulator
 * ════════════════════════════════════════════════════════════════════════ */

enum { RM_P_BYPASS=0, RM_P_FREQ, RM_P_MIX };
static const aplug_param_t rm_params[] = {
    { RM_P_BYPASS,"Bypass","",0,1,0,APLUG_PARAM_BYPASS|APLUG_PARAM_TOGGLE },
    { RM_P_FREQ,  "Freq", "Hz",1,2000,440.0f,APLUG_PARAM_AUTOMATABLE|APLUG_PARAM_SMOOTHED|APLUG_PARAM_LOG },
    { RM_P_MIX,   "Mix",  "",0,1,1.0f,APLUG_PARAM_AUTOMATABLE|APLUG_PARAM_SMOOTHED },
};
static const aplug_descriptor_t rm_desc = {
    APLUG_VERSION(1,0,0),"os.aether.ringmod","Ring Modulator",
    "AetherOS Audio",APLUG_CAT_DISTORTION,3,rm_params,1,1
};
typedef struct {
    adsp_smooth_t freq, mix;
    float phase, sr;
} rm_state_t;

static int rm_activate(aplug_t *p, float sr, int period)
{
    (void)period;
    rm_state_t *s = (rm_state_t*)p->state;
    s->sr = sr;
    adsp_smooth_init(&s->freq, 440.0f, 10.0f, sr);
    adsp_smooth_init(&s->mix,  1.0f,   10.0f, sr);
    return 0;
}
static void rm_deactivate(aplug_t *p) { (void)p; }

static void rm_process(aplug_t *p, const float *const *in,
                        float *const *out, int n)
{
    rm_state_t *s = (rm_state_t*)p->state;
    for (int i = 0; i < n; i++) {
        float freq = adsp_smooth_tick(&s->freq);
        float mix  = adsp_smooth_tick(&s->mix);
        s->phase += freq / s->sr;
        if (s->phase > 1.0f) s->phase -= 1.0f;
        float carrier = sinf(s->phase * 2.0f * (float)M_PI);
        out[0][i] = in[0][i] * (1.0f - mix) + in[0][i] * carrier * mix;
    }
}

static void rm_param_set(aplug_t *p, unsigned int id, float v)
{
    rm_state_t *s = (rm_state_t*)p->state;
    if (id == RM_P_FREQ) s->freq.target = v;
    if (id == RM_P_MIX)  s->mix.target  = v;
}
static float rm_param_get(aplug_t *p, unsigned int id)
{
    rm_state_t *s = (rm_state_t*)p->state;
    if (id == RM_P_FREQ) return s->freq.current;
    return 0.0f;
}
static const aplug_vtable_t rm_vtable = {
    rm_activate,rm_deactivate,rm_process,rm_param_set,rm_param_get
};
aplug_t *afx_ringmod_factory(void)
    { return alloc_plug(&rm_desc,&rm_vtable,sizeof(rm_state_t)); }

/* ════════════════════════════════════════════════════════════════════════
 * Looper (up to 30s @ 48kHz mono)
 * States: IDLE → RECORD → PLAY → OVERDUB → PLAY
 * ════════════════════════════════════════════════════════════════════════ */

#define LOOP_MAX (48000 * 30)

enum { LOOP_P_BYPASS=0, LOOP_P_MODE, LOOP_P_LEVEL };
static const aplug_param_t loop_params[] = {
    { LOOP_P_BYPASS,"Bypass","",0,1,0,APLUG_PARAM_BYPASS|APLUG_PARAM_TOGGLE },
    { LOOP_P_MODE,  "Mode", "",0,3,0,APLUG_PARAM_STEPPED },  /* 0=idle,1=rec,2=play,3=overdub */
    { LOOP_P_LEVEL, "Level","",0,1,1.0f,APLUG_PARAM_AUTOMATABLE|APLUG_PARAM_SMOOTHED },
};
static const aplug_descriptor_t loop_desc = {
    APLUG_VERSION(1,0,0),"os.aether.looper","Looper",
    "AetherOS Audio",APLUG_CAT_DELAY,3,loop_params,1,1
};
typedef struct {
    float *buf;      /* heap-allocated loop buffer */
    int    loop_len;
    int    pos;
    int    mode;     /* 0=idle,1=rec,2=play,3=overdub */
    adsp_smooth_t level;
} loop_state_t;

static int loop_activate(aplug_t *p, float sr, int period)
{
    (void)sr; (void)period;
    loop_state_t *s = (loop_state_t*)p->state;
    s->buf = (float*)malloc(sizeof(float) * LOOP_MAX);
    if (s->buf) memset(s->buf, 0, sizeof(float) * LOOP_MAX);
    adsp_smooth_init(&s->level, 1.0f, 10.0f, 48000.0f);
    s->loop_len = 0; s->pos = 0; s->mode = 0;
    return s->buf ? 0 : -1;
}

static void loop_deactivate(aplug_t *p)
{
    loop_state_t *s = (loop_state_t*)p->state;
    free(s->buf); s->buf = NULL;
}

static void loop_process(aplug_t *p, const float *const *in,
                          float *const *out, int n)
{
    loop_state_t *s = (loop_state_t*)p->state;
    if (!s->buf) { for (int i=0;i<n;i++) out[0][i]=in[0][i]; return; }

    for (int i = 0; i < n; i++) {
        float x     = in[0][i];
        float level = adsp_smooth_tick(&s->level);
        float loop_out = 0.0f;

        if (s->mode == 1) {   /* record */
            if (s->pos < LOOP_MAX) s->buf[s->pos++] = x;
            else { s->mode = 2; s->loop_len = LOOP_MAX; s->pos = 0; }
            loop_out = x;
        } else if (s->mode == 2) {   /* play */
            if (s->loop_len > 0) {
                loop_out = s->buf[s->pos++];
                if (s->pos >= s->loop_len) s->pos = 0;
            }
            loop_out = x + loop_out * level;
        } else if (s->mode == 3) {   /* overdub */
            if (s->loop_len > 0) {
                s->buf[s->pos] = s->buf[s->pos] * 0.95f + x;  /* mix in, slight decay */
                loop_out = s->buf[s->pos++] * level + x;
                if (s->pos >= s->loop_len) s->pos = 0;
            }
        } else {
            loop_out = x;
        }
        out[0][i] = loop_out;
    }
}

static void loop_param_set(aplug_t *p, unsigned int id, float v)
{
    loop_state_t *s = (loop_state_t*)p->state;
    if (id == LOOP_P_MODE) {
        int new_mode = (int)v;
        if (new_mode == 1) { /* start record */
            memset(s->buf, 0, sizeof(float) * LOOP_MAX);
            s->pos = 0; s->loop_len = 0;
        } else if (new_mode == 2 && s->mode == 1) { /* stop record → play */
            s->loop_len = s->pos;
            s->pos = 0;
        }
        s->mode = new_mode;
    }
    if (id == LOOP_P_LEVEL) s->level.target = v;
}
static float loop_param_get(aplug_t *p, unsigned int id)
{
    loop_state_t *s = (loop_state_t*)p->state;
    if (id == LOOP_P_MODE) return (float)s->mode;
    return 0.0f;
}
static const aplug_vtable_t loop_vtable = {
    loop_activate,loop_deactivate,loop_process,loop_param_set,loop_param_get
};
aplug_t *afx_looper_factory(void)
    { return alloc_plug(&loop_desc,&loop_vtable,sizeof(loop_state_t)); }
