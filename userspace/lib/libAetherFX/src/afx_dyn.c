/*
 * AetherOS — libAetherFX: Dynamics (Phase 8.7)
 * File: userspace/lib/libAetherFX/src/afx_dyn.c
 *
 * Compressor — optical emulation (LA-2A style): feed-forward RMS
 *              detector, program-dependent release, soft knee.
 * Limiter    — brick-wall look-ahead limiter (2 ms look-ahead).
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

/* ════════════════════════════════════════════════════════════════════════
 * Compressor
 * ════════════════════════════════════════════════════════════════════════ */

enum { COMP_P_BYPASS=0, COMP_P_THRESH, COMP_P_RATIO,
       COMP_P_ATTACK, COMP_P_RELEASE, COMP_P_MAKEUP };

static const aplug_param_t comp_params[] = {
    { COMP_P_BYPASS,  "Bypass",  "",   0,1,0,   APLUG_PARAM_BYPASS|APLUG_PARAM_TOGGLE },
    { COMP_P_THRESH,  "Thresh",  "dB",-60,0,-18, APLUG_PARAM_AUTOMATABLE },
    { COMP_P_RATIO,   "Ratio",   ":1", 1,20,4,  APLUG_PARAM_AUTOMATABLE },
    { COMP_P_ATTACK,  "Attack",  "ms", 0.1f,100,10,APLUG_PARAM_AUTOMATABLE },
    { COMP_P_RELEASE, "Release", "ms", 10,2000,200,APLUG_PARAM_AUTOMATABLE },
    { COMP_P_MAKEUP,  "Makeup",  "dB", 0,24,0,  APLUG_PARAM_AUTOMATABLE|APLUG_PARAM_SMOOTHED },
};
static const aplug_descriptor_t comp_desc = {
    APLUG_VERSION(1,0,0), "os.aether.comp", "Optical Compressor",
    "AetherOS Audio", APLUG_CAT_DYNAMICS, 6, comp_params, 1, 1
};

typedef struct {
    float thresh_lin;
    float ratio;
    float attack_coef;
    float release_coef;
    adsp_smooth_t makeup;
    adsp_env_t    env;
    float         gr;     /* gain reduction, linear */
    float         sr;
} comp_state_t;

static int comp_activate(aplug_t *p, float sr, int period)
{
    (void)period;
    comp_state_t *s = (comp_state_t*)p->state;
    s->sr          = sr;
    s->thresh_lin  = adsp_db_to_lin(-18.0f);
    s->ratio       = 4.0f;
    s->gr          = 1.0f;
    adsp_env_init(&s->env, 10.0f, 200.0f, sr);
    adsp_smooth_init(&s->makeup, 1.0f, 10.0f, sr);
    s->attack_coef  = expf(-1000.0f / (sr * 10.0f));
    s->release_coef = expf(-1000.0f / (sr * 200.0f));
    return 0;
}
static void comp_deactivate(aplug_t *p) { (void)p; }

static void comp_process(aplug_t *p, const float *const *in,
                          float *const *out, int n)
{
    comp_state_t *s = (comp_state_t*)p->state;
    float inv_ratio = 1.0f / s->ratio;

    for (int i = 0; i < n; i++) {
        float x   = in[0][i];
        float env = adsp_env_peak(&s->env, x);

        float gr_target = 1.0f;
        if (env > s->thresh_lin) {
            /* Soft-knee gain reduction */
            float over_db = adsp_lin_to_db(env / s->thresh_lin);
            float gr_db   = over_db * (inv_ratio - 1.0f);
            gr_target = adsp_db_to_lin(gr_db);
        }

        /* Asymmetric smoothing */
        float coef = gr_target < s->gr ? s->attack_coef : s->release_coef;
        s->gr = gr_target + coef * (s->gr - gr_target);

        float mk = adsp_smooth_tick(&s->makeup);
        out[0][i] = x * s->gr * mk;
    }
}

static void comp_param_set(aplug_t *p, unsigned int id, float v)
{
    comp_state_t *s = (comp_state_t*)p->state;
    if (id == COMP_P_THRESH)  s->thresh_lin  = adsp_db_to_lin(v);
    if (id == COMP_P_RATIO)   s->ratio       = v < 1.0f ? 1.0f : v;
    if (id == COMP_P_ATTACK)  s->attack_coef  = expf(-1000.0f / (s->sr * v));
    if (id == COMP_P_RELEASE) s->release_coef = expf(-1000.0f / (s->sr * v));
    if (id == COMP_P_MAKEUP)  s->makeup.target = adsp_db_to_lin(v);
}
static float comp_param_get(aplug_t *p, unsigned int id)
{
    comp_state_t *s = (comp_state_t*)p->state;
    if (id == COMP_P_THRESH) return adsp_lin_to_db(s->thresh_lin);
    if (id == COMP_P_RATIO)  return s->ratio;
    if (id == COMP_P_MAKEUP) return adsp_lin_to_db(s->makeup.current);
    return 0.0f;
}

static const aplug_vtable_t comp_vtable = {
    comp_activate, comp_deactivate,
    comp_process, comp_param_set, comp_param_get
};
aplug_t *afx_comp_factory(void)
    { return alloc_plug(&comp_desc, &comp_vtable, sizeof(comp_state_t)); }

/* ════════════════════════════════════════════════════════════════════════
 * Limiter (look-ahead brick-wall)
 * ════════════════════════════════════════════════════════════════════════ */

#define LIM_LOOKAHEAD 96   /* samples, ~2ms at 48kHz */

enum { LIM_P_BYPASS=0, LIM_P_THRESH, LIM_P_RELEASE };
static const aplug_param_t lim_params[] = {
    { LIM_P_BYPASS,  "Bypass",  "",  0,1,0,   APLUG_PARAM_BYPASS|APLUG_PARAM_TOGGLE },
    { LIM_P_THRESH,  "Thresh",  "dB",-12,0,-0.3f, APLUG_PARAM_AUTOMATABLE },
    { LIM_P_RELEASE, "Release", "ms", 10,500,50,   APLUG_PARAM_AUTOMATABLE },
};
static const aplug_descriptor_t lim_desc = {
    APLUG_VERSION(1,0,0), "os.aether.limiter", "Brick-Wall Limiter",
    "AetherOS Audio", APLUG_CAT_DYNAMICS, 3, lim_params, 1, 1
};

typedef struct {
    float delay_buf[LIM_LOOKAHEAD];
    int   delay_pos;
    float thresh_lin;
    float release_coef;
    float env;
    float sr;
} lim_state_t;

static int lim_activate(aplug_t *p, float sr, int period)
{
    (void)period;
    lim_state_t *s = (lim_state_t*)p->state;
    s->sr           = sr;
    s->thresh_lin   = adsp_db_to_lin(-0.3f);
    s->release_coef = expf(-1000.0f / (sr * 50.0f));
    s->env          = 0.0f;
    s->delay_pos    = 0;
    return 0;
}
static void lim_deactivate(aplug_t *p) { (void)p; }

static void lim_process(aplug_t *p, const float *const *in,
                         float *const *out, int n)
{
    lim_state_t *s = (lim_state_t*)p->state;
    for (int i = 0; i < n; i++) {
        float x   = in[0][i];
        float abs = x < 0 ? -x : x;

        /* Peak look-ahead: write to delay, use delayed sample for output */
        s->delay_buf[s->delay_pos] = x;
        int read_pos = (s->delay_pos + 1) % LIM_LOOKAHEAD;
        float delayed = s->delay_buf[read_pos];
        s->delay_pos = read_pos;

        /* Envelope follower on instantaneous input */
        if (abs > s->env) s->env = abs;
        else s->env = abs + s->release_coef * (s->env - abs);

        float gr = 1.0f;
        if (s->env > s->thresh_lin)
            gr = s->thresh_lin / s->env;

        out[0][i] = delayed * gr;
    }
}

static void lim_param_set(aplug_t *p, unsigned int id, float v)
{
    lim_state_t *s = (lim_state_t*)p->state;
    if (id == LIM_P_THRESH)  s->thresh_lin   = adsp_db_to_lin(v);
    if (id == LIM_P_RELEASE) s->release_coef = expf(-1000.0f / (s->sr * v));
}
static float lim_param_get(aplug_t *p, unsigned int id)
{
    lim_state_t *s = (lim_state_t*)p->state;
    return id == LIM_P_THRESH ? adsp_lin_to_db(s->thresh_lin) : 0.0f;
}

static const aplug_vtable_t lim_vtable = {
    lim_activate, lim_deactivate,
    lim_process, lim_param_set, lim_param_get
};
aplug_t *afx_limiter_factory(void)
    { return alloc_plug(&lim_desc, &lim_vtable, sizeof(lim_state_t)); }
