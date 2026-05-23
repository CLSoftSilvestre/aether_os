/*
 * AetherOS — libAetherFX: Time-based effects (Phase 8.7)
 * File: userspace/lib/libAetherFX/src/afx_time.c
 *
 * Delay      — digital delay with tone and tap-tempo support
 * PP Delay   — ping-pong stereo delay
 * Plate      — Schroeder plate reverb (4 comb + 2 all-pass)
 * Spring     — spring reverb emulation (FDN + metallic resonance)
 */

#include "aplug.h"
#include "adsp.h"
#include "afx.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define DELAY_MAX 131072   /* ~2.7s at 48kHz */
#define REV_MAX    8192

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

static inline void dw(float *b, int *pos, float x, int mask)
    { b[(*pos)++ & mask] = x; }
static inline float dr(const float *b, int pos, int d, int mask)
    { return b[(pos - d) & mask]; }

/* ════════════════════════════════════════════════════════════════════════
 * Delay
 * ════════════════════════════════════════════════════════════════════════ */

enum { DEL_P_BYPASS=0, DEL_P_TIME, DEL_P_FEEDBACK, DEL_P_MIX, DEL_P_TONE };
static const aplug_param_t del_params[] = {
    { DEL_P_BYPASS,  "Bypass",  "",0,1,0,APLUG_PARAM_BYPASS|APLUG_PARAM_TOGGLE },
    { DEL_P_TIME,    "Time",    "ms",10,2000,300,APLUG_PARAM_AUTOMATABLE|APLUG_PARAM_SMOOTHED },
    { DEL_P_FEEDBACK,"Feedback","",0,0.95f,0.4f,APLUG_PARAM_AUTOMATABLE|APLUG_PARAM_SMOOTHED },
    { DEL_P_MIX,     "Mix",     "",0,1,0.3f,APLUG_PARAM_AUTOMATABLE|APLUG_PARAM_SMOOTHED },
    { DEL_P_TONE,    "Tone",    "",0,1,0.7f,APLUG_PARAM_AUTOMATABLE },
};
static const aplug_descriptor_t del_desc = {
    APLUG_VERSION(1,0,0),"os.aether.delay","Digital Delay",
    "AetherOS Audio",APLUG_CAT_DELAY,5,del_params,1,2
};
typedef struct {
    float buf[DELAY_MAX];
    int   pos;
    adsp_biquad_t tone_lp;
    adsp_smooth_t time_ms, feedback, mix;
    float tone, sr;
} del_state_t;

static int del_activate(aplug_t *p, float sr, int period)
{
    (void)period;
    del_state_t *s = (del_state_t*)p->state;
    s->sr = sr;
    adsp_smooth_init(&s->time_ms,  300.0f, 50.0f, sr);
    adsp_smooth_init(&s->feedback, 0.4f,   10.0f, sr);
    adsp_smooth_init(&s->mix,      0.3f,   10.0f, sr);
    adsp_biquad_lpf(&s->tone_lp, 4000.0f, 0.7f, sr);
    s->tone = 0.7f;
    return 0;
}
static void del_deactivate(aplug_t *p) { (void)p; }

static void del_process(aplug_t *p, const float *const *in,
                         float *const *out, int n)
{
    del_state_t *s = (del_state_t*)p->state;
    for (int i = 0; i < n; i++) {
        float t_ms = adsp_smooth_tick(&s->time_ms);
        float fb   = adsp_smooth_tick(&s->feedback);
        float mix  = adsp_smooth_tick(&s->mix);

        int d = (int)(t_ms * 0.001f * s->sr);
        if (d >= DELAY_MAX) d = DELAY_MAX - 1;
        if (d < 1) d = 1;

        float wet = dr(s->buf, s->pos, d, DELAY_MAX - 1);
        float tone_wet; adsp_biquad_process(&s->tone_lp, &tone_wet, &wet, 1);
        wet = wet * s->tone + tone_wet * (1.0f - s->tone);

        dw(s->buf, &s->pos, in[0][i] + wet * fb, DELAY_MAX - 1);

        out[0][i] = in[0][i] * (1.0f - mix) + wet * mix;
        out[1][i] = out[0][i];
    }
}

static void del_param_set(aplug_t *p, unsigned int id, float v)
{
    del_state_t *s = (del_state_t*)p->state;
    if (id == DEL_P_TIME)     s->time_ms.target  = v;
    if (id == DEL_P_FEEDBACK) s->feedback.target = v;
    if (id == DEL_P_MIX)      s->mix.target      = v;
    if (id == DEL_P_TONE)     s->tone            = v;
}
static float del_param_get(aplug_t *p, unsigned int id)
{
    del_state_t *s = (del_state_t*)p->state;
    if (id == DEL_P_TIME) return s->time_ms.current;
    return 0.0f;
}
static const aplug_vtable_t del_vtable = {
    del_activate,del_deactivate,del_process,del_param_set,del_param_get
};
aplug_t *afx_delay_factory(void)
    { return alloc_plug(&del_desc,&del_vtable,sizeof(del_state_t)); }

/* ════════════════════════════════════════════════════════════════════════
 * Ping-Pong Delay
 * ════════════════════════════════════════════════════════════════════════ */

enum { PPD_P_BYPASS=0, PPD_P_TIME, PPD_P_FEEDBACK, PPD_P_MIX };
static const aplug_param_t ppd_params[] = {
    { PPD_P_BYPASS,  "Bypass",  "",0,1,0,APLUG_PARAM_BYPASS|APLUG_PARAM_TOGGLE },
    { PPD_P_TIME,    "Time",    "ms",10,1000,250,APLUG_PARAM_AUTOMATABLE|APLUG_PARAM_SMOOTHED },
    { PPD_P_FEEDBACK,"Feedback","",0,0.95f,0.4f,APLUG_PARAM_AUTOMATABLE|APLUG_PARAM_SMOOTHED },
    { PPD_P_MIX,     "Mix",     "",0,1,0.3f,APLUG_PARAM_AUTOMATABLE|APLUG_PARAM_SMOOTHED },
};
static const aplug_descriptor_t ppd_desc = {
    APLUG_VERSION(1,0,0),"os.aether.ppdelay","Ping-Pong Delay",
    "AetherOS Audio",APLUG_CAT_DELAY,4,ppd_params,1,2
};
typedef struct {
    float buf_l[DELAY_MAX], buf_r[DELAY_MAX];
    int   pos_l, pos_r;
    adsp_smooth_t time_ms, feedback, mix;
    float sr;
} ppd_state_t;

static int ppd_activate(aplug_t *p, float sr, int period)
{
    (void)period;
    ppd_state_t *s = (ppd_state_t*)p->state;
    s->sr = sr;
    adsp_smooth_init(&s->time_ms,  250.0f, 50.0f, sr);
    adsp_smooth_init(&s->feedback, 0.4f,   10.0f, sr);
    adsp_smooth_init(&s->mix,      0.3f,   10.0f, sr);
    return 0;
}
static void ppd_deactivate(aplug_t *p) { (void)p; }

static void ppd_process(aplug_t *p, const float *const *in,
                         float *const *out, int n)
{
    ppd_state_t *s = (ppd_state_t*)p->state;
    for (int i = 0; i < n; i++) {
        float t_ms = adsp_smooth_tick(&s->time_ms);
        float fb   = adsp_smooth_tick(&s->feedback);
        float mix  = adsp_smooth_tick(&s->mix);
        int   d    = (int)(t_ms * 0.001f * s->sr);
        if (d >= DELAY_MAX) d = DELAY_MAX - 1;
        if (d < 1) d = 1;

        float r_wet = dr(s->buf_r, s->pos_r, d, DELAY_MAX - 1);
        float l_wet = dr(s->buf_l, s->pos_l, d, DELAY_MAX - 1);

        dw(s->buf_l, &s->pos_l, in[0][i] + r_wet * fb, DELAY_MAX - 1);
        dw(s->buf_r, &s->pos_r, l_wet * fb,             DELAY_MAX - 1);

        out[0][i] = in[0][i] * (1.0f - mix) + l_wet * mix;
        out[1][i] = in[0][i] * (1.0f - mix) + r_wet * mix;
    }
}

static void ppd_param_set(aplug_t *p, unsigned int id, float v)
{
    ppd_state_t *s = (ppd_state_t*)p->state;
    if (id == PPD_P_TIME)     s->time_ms.target  = v;
    if (id == PPD_P_FEEDBACK) s->feedback.target = v;
    if (id == PPD_P_MIX)      s->mix.target      = v;
}
static float ppd_param_get(aplug_t *p, unsigned int id)
{
    ppd_state_t *s = (ppd_state_t*)p->state;
    if (id == PPD_P_TIME) return s->time_ms.current;
    return 0.0f;
}
static const aplug_vtable_t ppd_vtable = {
    ppd_activate,ppd_deactivate,ppd_process,ppd_param_set,ppd_param_get
};
aplug_t *afx_ppdelay_factory(void)
    { return alloc_plug(&ppd_desc,&ppd_vtable,sizeof(ppd_state_t)); }

/* ════════════════════════════════════════════════════════════════════════
 * Plate Reverb (Schroeder: 4 parallel comb + 2 serial all-pass)
 * ════════════════════════════════════════════════════════════════════════ */

#define PLATE_COMB_LEN0 1687
#define PLATE_COMB_LEN1 1601
#define PLATE_COMB_LEN2 2053
#define PLATE_COMB_LEN3 2251
#define PLATE_AP_LEN0    211
#define PLATE_AP_LEN1     97

enum { PLT_P_BYPASS=0, PLT_P_SIZE, PLT_P_DAMP, PLT_P_MIX };
static const aplug_param_t plt_params[] = {
    { PLT_P_BYPASS,"Bypass","",0,1,0,APLUG_PARAM_BYPASS|APLUG_PARAM_TOGGLE },
    { PLT_P_SIZE,  "Size",  "",0,1,0.5f,APLUG_PARAM_AUTOMATABLE|APLUG_PARAM_SMOOTHED },
    { PLT_P_DAMP,  "Damp",  "",0,1,0.5f,APLUG_PARAM_AUTOMATABLE|APLUG_PARAM_SMOOTHED },
    { PLT_P_MIX,   "Mix",   "",0,1,0.3f,APLUG_PARAM_AUTOMATABLE|APLUG_PARAM_SMOOTHED },
};
static const aplug_descriptor_t plt_desc = {
    APLUG_VERSION(1,0,0),"os.aether.plate","Plate Reverb",
    "AetherOS Audio",APLUG_CAT_REVERB,4,plt_params,1,2
};

typedef struct {
    float comb0[PLATE_COMB_LEN0], comb1[PLATE_COMB_LEN1];
    float comb2[PLATE_COMB_LEN2], comb3[PLATE_COMB_LEN3];
    float ap0[PLATE_AP_LEN0], ap1[PLATE_AP_LEN1];
    int   cp0, cp1, cp2, cp3, ap0p, ap1p;
    float cf0, cf1, cf2, cf3;   /* comb feedback */
    float lpf0, lpf1, lpf2, lpf3;   /* damping state */
    adsp_smooth_t size, damp, mix;
} plt_state_t;

static int plt_activate(aplug_t *p, float sr, int period)
{
    (void)sr; (void)period;
    plt_state_t *s = (plt_state_t*)p->state;
    adsp_smooth_init(&s->size, 0.5f, 20.0f, 48000.0f);
    adsp_smooth_init(&s->damp, 0.5f, 20.0f, 48000.0f);
    adsp_smooth_init(&s->mix,  0.3f, 10.0f, 48000.0f);
    s->cf0 = s->cf1 = s->cf2 = s->cf3 = 0.84f;
    return 0;
}
static void plt_deactivate(aplug_t *p) { (void)p; }

static void plt_process(aplug_t *p, const float *const *in,
                         float *const *out, int n)
{
    plt_state_t *s = (plt_state_t*)p->state;
    for (int i = 0; i < n; i++) {
        float size = adsp_smooth_tick(&s->size);
        float damp = adsp_smooth_tick(&s->damp);
        float mix  = adsp_smooth_tick(&s->mix);
        float g    = 0.5f + size * 0.45f;   /* comb feedback 0.5..0.95 */
        float x    = in[0][i];

        /* 4 parallel feedback comb filters */
#define COMB(buf, len, pos, lf) \
    { float o = buf[pos]; lf = o*(1.0f-damp*0.5f) + lf*damp*0.5f; \
      buf[pos] = x + lf*g; pos = (pos+1)%(len); o = lf; (void)o; }

        float o0 = s->comb0[s->cp0]; s->lpf0 = o0*(1.0f-damp*0.5f)+s->lpf0*damp*0.5f;
        s->comb0[s->cp0] = x + s->lpf0*g; s->cp0 = (s->cp0+1)%PLATE_COMB_LEN0;

        float o1 = s->comb1[s->cp1]; s->lpf1 = o1*(1.0f-damp*0.5f)+s->lpf1*damp*0.5f;
        s->comb1[s->cp1] = x + s->lpf1*g; s->cp1 = (s->cp1+1)%PLATE_COMB_LEN1;

        float o2 = s->comb2[s->cp2]; s->lpf2 = o2*(1.0f-damp*0.5f)+s->lpf2*damp*0.5f;
        s->comb2[s->cp2] = x + s->lpf2*g; s->cp2 = (s->cp2+1)%PLATE_COMB_LEN2;

        float o3 = s->comb3[s->cp3]; s->lpf3 = o3*(1.0f-damp*0.5f)+s->lpf3*damp*0.5f;
        s->comb3[s->cp3] = x + s->lpf3*g; s->cp3 = (s->cp3+1)%PLATE_COMB_LEN3;

        float y = (o0 + o1 + o2 + o3) * 0.25f;

        /* 2 serial all-pass */
        float ap_g = 0.7f;
        float v0 = s->ap0[s->ap0p];
        s->ap0[s->ap0p] = y + v0 * ap_g;
        y = v0 - ap_g * s->ap0[s->ap0p];
        s->ap0p = (s->ap0p + 1) % PLATE_AP_LEN0;

        float v1 = s->ap1[s->ap1p];
        s->ap1[s->ap1p] = y + v1 * ap_g;
        y = v1 - ap_g * s->ap1[s->ap1p];
        s->ap1p = (s->ap1p + 1) % PLATE_AP_LEN1;

        out[0][i] = x * (1.0f - mix) + y * mix;
        out[1][i] = x * (1.0f - mix) + y * mix;   /* mono reverb */
    }
}

static void plt_param_set(aplug_t *p, unsigned int id, float v)
{
    plt_state_t *s = (plt_state_t*)p->state;
    if (id == PLT_P_SIZE) s->size.target = v;
    if (id == PLT_P_DAMP) s->damp.target = v;
    if (id == PLT_P_MIX)  s->mix.target  = v;
}
static float plt_param_get(aplug_t *p, unsigned int id)
{
    plt_state_t *s = (plt_state_t*)p->state;
    if (id == PLT_P_MIX) return s->mix.current;
    return 0.0f;
}
static const aplug_vtable_t plt_vtable = {
    plt_activate,plt_deactivate,plt_process,plt_param_set,plt_param_get
};
aplug_t *afx_plate_factory(void)
    { return alloc_plug(&plt_desc,&plt_vtable,sizeof(plt_state_t)); }

/* ════════════════════════════════════════════════════════════════════════
 * Spring Reverb (metallic FDN-2 + resonant filters)
 * ════════════════════════════════════════════════════════════════════════ */

enum { SPR_P_BYPASS=0, SPR_P_DECAY, SPR_P_TONE, SPR_P_MIX };
static const aplug_param_t spr_params[] = {
    { SPR_P_BYPASS,"Bypass","",0,1,0,APLUG_PARAM_BYPASS|APLUG_PARAM_TOGGLE },
    { SPR_P_DECAY, "Decay", "",0,1,0.5f,APLUG_PARAM_AUTOMATABLE|APLUG_PARAM_SMOOTHED },
    { SPR_P_TONE,  "Tone",  "",0,1,0.5f,APLUG_PARAM_AUTOMATABLE },
    { SPR_P_MIX,   "Mix",   "",0,1,0.3f,APLUG_PARAM_AUTOMATABLE|APLUG_PARAM_SMOOTHED },
};
static const aplug_descriptor_t spr_desc = {
    APLUG_VERSION(1,0,0),"os.aether.spring","Spring Reverb",
    "AetherOS Audio",APLUG_CAT_REVERB,4,spr_params,1,2
};

#define SPR_LEN 4096
typedef struct {
    float line0[SPR_LEN], line1[SPR_LEN];
    int   lp0, lp1;
    adsp_biquad_t res0, res1, tone_bq;
    adsp_smooth_t decay, mix;
    float tone;
} spr_state_t;

static int spr_activate(aplug_t *p, float sr, int period)
{
    (void)period;
    spr_state_t *s = (spr_state_t*)p->state;
    adsp_smooth_init(&s->decay, 0.5f, 20.0f, sr);
    adsp_smooth_init(&s->mix,   0.3f, 10.0f, sr);
    adsp_biquad_bpf(&s->res0, 800.0f,  8.0f, sr);
    adsp_biquad_bpf(&s->res1, 2200.0f, 6.0f, sr);
    adsp_biquad_lpf(&s->tone_bq, 3000.0f, 0.7f, sr);
    s->tone = 0.5f;
    return 0;
}
static void spr_deactivate(aplug_t *p) { (void)p; }

static void spr_process(aplug_t *p, const float *const *in,
                         float *const *out, int n)
{
    spr_state_t *s = (spr_state_t*)p->state;
    for (int i = 0; i < n; i++) {
        float decay = 0.5f + adsp_smooth_tick(&s->decay) * 0.45f;
        float mix   = adsp_smooth_tick(&s->mix);
        float x     = in[0][i];

        /* FDN-2 delay network */
        int d0 = (int)(48000 * 0.03f);
        int d1 = (int)(48000 * 0.053f);

        float o0 = s->line0[s->lp0];
        float o1 = s->line1[s->lp1];

        s->line0[s->lp0] = x + o1 * decay;
        s->line1[s->lp1] = x + o0 * decay;
        s->lp0 = (s->lp0 + 1) % (d0 < SPR_LEN ? d0 : SPR_LEN);
        s->lp1 = (s->lp1 + 1) % (d1 < SPR_LEN ? d1 : SPR_LEN);

        /* Add metallic resonances */
        float r0, r1;
        adsp_biquad_process(&s->res0, &r0, &o0, 1);
        adsp_biquad_process(&s->res1, &r1, &o1, 1);
        float wet = (o0 + o1 + r0 * 0.3f + r1 * 0.2f) * 0.25f;

        /* Tone: LP blend */
        float toned;
        adsp_biquad_process(&s->tone_bq, &toned, &wet, 1);
        wet = wet * s->tone + toned * (1.0f - s->tone);

        out[0][i] = x * (1.0f - mix) + wet * mix;
        out[1][i] = x * (1.0f - mix) - wet * mix;
    }
}

static void spr_param_set(aplug_t *p, unsigned int id, float v)
{
    spr_state_t *s = (spr_state_t*)p->state;
    if (id == SPR_P_DECAY) s->decay.target = v;
    if (id == SPR_P_TONE)  s->tone         = v;
    if (id == SPR_P_MIX)   s->mix.target   = v;
}
static float spr_param_get(aplug_t *p, unsigned int id)
{
    spr_state_t *s = (spr_state_t*)p->state;
    if (id == SPR_P_MIX) return s->mix.current;
    return 0.0f;
}
static const aplug_vtable_t spr_vtable = {
    spr_activate,spr_deactivate,spr_process,spr_param_set,spr_param_get
};
aplug_t *afx_spring_factory(void)
    { return alloc_plug(&spr_desc,&spr_vtable,sizeof(spr_state_t)); }
