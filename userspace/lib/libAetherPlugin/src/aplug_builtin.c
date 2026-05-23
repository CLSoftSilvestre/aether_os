/*
 * AetherOS — libAetherPlugin: Built-in plugins (Phase 8.5)
 * File: userspace/lib/libAetherPlugin/src/aplug_builtin.c
 *
 * Two reference plugins registered at startup:
 *
 *  "os.aether.gain"   — stereo gain + bypass (APLUG_CAT_UTILITY)
 *  "os.aether.noop"   — pass-through (identity), for chain testing
 *
 * These also serve as the canonical example of how to implement a plugin.
 */

#include "aplug.h"
#include "adsp.h"   /* adsp_smooth_t, adsp_scale_f32 */
#include <stdlib.h>
#include <string.h>

/* ══════════════════════════════════════════════════════════════════════
 * Gain plugin
 * ══════════════════════════════════════════════════════════════════════ */

#define GAIN_PARAM_GAIN   0
#define GAIN_PARAM_BYPASS 1

static const aplug_param_t gain_params[] = {
    { GAIN_PARAM_GAIN,   "Gain",   "dB",
      -60.0f, +24.0f, 0.0f,
      APLUG_PARAM_AUTOMATABLE | APLUG_PARAM_SMOOTHED },
    { GAIN_PARAM_BYPASS, "Bypass", "",
      0.0f, 1.0f, 0.0f,
      APLUG_PARAM_BYPASS | APLUG_PARAM_TOGGLE },
};

static const aplug_descriptor_t gain_desc = {
    .version     = APLUG_VERSION(1, 0, 0),
    .id          = "os.aether.gain",
    .name        = "Gain",
    .vendor      = "AetherOS Audio",
    .category    = APLUG_CAT_UTILITY,
    .num_params  = 2,
    .params      = gain_params,
    .num_inputs  = 2,
    .num_outputs = 2,
};

typedef struct {
    adsp_smooth_t smooth;
    float         bypass;
} gain_state_t;

static int gain_activate(aplug_t *p, float sr, int period)
{
    (void)period;
    gain_state_t *s = (gain_state_t *)p->state;
    adsp_smooth_init(&s->smooth, 1.0f, 10.0f, sr);   /* 10 ms smoothing */
    s->bypass = 0.0f;
    return 0;
}

static void gain_deactivate(aplug_t *p)
{
    (void)p;
}

static void gain_process(aplug_t *p,
                          const float *const *in,
                          float       *const *out,
                          int n)
{
    gain_state_t *s = (gain_state_t *)p->state;

    if (s->bypass >= 0.5f) {
        /* Bypass: copy in → out */
        for (int i = 0; i < n; i++) out[0][i] = in[0][i];
        for (int i = 0; i < n; i++) out[1][i] = in[1][i];
        return;
    }

    for (int i = 0; i < n; i++) {
        float g = adsp_smooth_tick(&s->smooth);
        out[0][i] = in[0][i] * g;
        out[1][i] = in[1][i] * g;
    }
}

static void gain_param_set(aplug_t *p, unsigned int id, float v)
{
    gain_state_t *s = (gain_state_t *)p->state;
    if (id == GAIN_PARAM_GAIN) {
        s->smooth.target = adsp_db_to_lin(v);
    } else if (id == GAIN_PARAM_BYPASS) {
        s->bypass = v;
    }
}

static float gain_param_get(aplug_t *p, unsigned int id)
{
    gain_state_t *s = (gain_state_t *)p->state;
    if (id == GAIN_PARAM_GAIN)
        return adsp_lin_to_db(s->smooth.current);
    if (id == GAIN_PARAM_BYPASS)
        return s->bypass;
    return 0.0f;
}

static const aplug_vtable_t gain_vtable = {
    gain_activate, gain_deactivate,
    gain_process, gain_param_set, gain_param_get
};

aplug_t *aplug_gain_factory(void)
{
    aplug_t *p = (aplug_t *)malloc(sizeof(aplug_t) + sizeof(gain_state_t));
    if (!p) return NULL;
    p->desc   = &gain_desc;
    p->vtable = &gain_vtable;
    p->state  = (char *)p + sizeof(aplug_t);
    memset(p->state, 0, sizeof(gain_state_t));
    return p;
}

/* ══════════════════════════════════════════════════════════════════════
 * No-op pass-through plugin (for chain wiring tests)
 * ══════════════════════════════════════════════════════════════════════ */

static const aplug_descriptor_t noop_desc = {
    .version     = APLUG_VERSION(1, 0, 0),
    .id          = "os.aether.noop",
    .name        = "Pass-Through",
    .vendor      = "AetherOS Audio",
    .category    = APLUG_CAT_UTILITY,
    .num_params  = 0,
    .params      = NULL,
    .num_inputs  = 2,
    .num_outputs = 2,
};

static int  noop_activate(aplug_t *p, float sr, int period)
    { (void)p; (void)sr; (void)period; return 0; }
static void noop_deactivate(aplug_t *p) { (void)p; }
static void noop_process(aplug_t *p, const float *const *in,
                          float *const *out, int n)
{
    (void)p;
    for (int i = 0; i < n; i++) out[0][i] = in[0][i];
    for (int i = 0; i < n; i++) out[1][i] = in[1][i];
}
static void  noop_param_set(aplug_t *p, unsigned int id, float v)
    { (void)p; (void)id; (void)v; }
static float noop_param_get(aplug_t *p, unsigned int id)
    { (void)p; (void)id; return 0.0f; }

static const aplug_vtable_t noop_vtable = {
    noop_activate, noop_deactivate,
    noop_process, noop_param_set, noop_param_get
};

aplug_t *aplug_noop_factory(void)
{
    aplug_t *p = (aplug_t *)malloc(sizeof(aplug_t));
    if (!p) return NULL;
    p->desc   = &noop_desc;
    p->vtable = &noop_vtable;
    p->state  = NULL;
    return p;
}

/* ── Register all builtins ───────────────────────────────────────────── */

void aplug_register_builtins(void)
{
    aplug_registry_register(aplug_gain_factory);
    aplug_registry_register(aplug_noop_factory);
}
