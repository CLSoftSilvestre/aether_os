/*
 * AetherOS — libAetherPlugin: Plugin API (Phase 8.5)
 * File: userspace/lib/libAetherPlugin/include/aplug.h
 *
 * CLAP-inspired internal plugin interface for the AetherOS audio pipeline.
 * All plugins are statically linked (no dlopen); factories are registered at
 * startup via aplug_registry_register().
 *
 * Design rules:
 *   - Zero allocation on the process() hot path
 *   - All state owned by the plugin instance (opaque aplug_t)
 *   - Parameters are float32; smoothing is the plugin's responsibility
 *     (use adsp_smooth_t from libAetherDSP)
 *   - process() is always called with exactly the period_frames given at
 *     activate() time; plugins may assert n == period_frames
 *
 * Parameter flags (bitfield in aplug_param_t.flags):
 *   APLUG_PARAM_AUTOMATABLE  — can be modulated per-block
 *   APLUG_PARAM_SMOOTHED     — plugin smooths internally (signal chain hints)
 *   APLUG_PARAM_BYPASS       — this param is the plugin bypass toggle
 *   APLUG_PARAM_READONLY     — metering / read-only output
 */

#ifndef AETHER_APLUG_H
#define AETHER_APLUG_H

/* ── Plugin categories ───────────────────────────────────────────────── */

#define APLUG_CAT_UTILITY     0   /* tuner, noise gate, volume */
#define APLUG_CAT_DYNAMICS    1   /* compressor, limiter, gate */
#define APLUG_CAT_FILTER      2   /* EQ, wah, tone stack */
#define APLUG_CAT_DISTORTION  3   /* overdrive, distortion, fuzz, amp */
#define APLUG_CAT_MODULATION  4   /* chorus, flanger, phaser, tremolo */
#define APLUG_CAT_DELAY       5   /* delay, echo, looper */
#define APLUG_CAT_REVERB      6   /* spring, hall, plate, shimmer */
#define APLUG_CAT_PITCH       7   /* pitch shift, harmonizer, whammy */
#define APLUG_CAT_AMP         8   /* amp head model, cabinet IR */
#define APLUG_CAT_ANALYZER    9   /* spectrum, oscilloscope, meter */

/* ── Parameter flags ─────────────────────────────────────────────────── */

#define APLUG_PARAM_AUTOMATABLE  (1u << 0)
#define APLUG_PARAM_SMOOTHED     (1u << 1)
#define APLUG_PARAM_BYPASS       (1u << 2)
#define APLUG_PARAM_READONLY     (1u << 3)
#define APLUG_PARAM_LOG          (1u << 4)   /* log-scale knob hint */
#define APLUG_PARAM_TOGGLE       (1u << 5)   /* boolean: 0.0 or 1.0 */
#define APLUG_PARAM_STEPPED      (1u << 6)   /* integer steps */

/* ── Parameter descriptor ────────────────────────────────────────────── */

typedef struct {
    unsigned int  id;
    const char   *name;
    const char   *unit;     /* "dB", "Hz", "ms", "%", "", … */
    float         min;
    float         max;
    float         def;      /* default value */
    unsigned int  flags;
} aplug_param_t;

/* ── Plugin descriptor (static, no heap) ─────────────────────────────── */

typedef struct {
    unsigned int         version;     /* APLUG_VERSION(maj,min,patch) */
    const char          *id;          /* unique reverse-DNS id, e.g. "os.aether.od1" */
    const char          *name;        /* display name, e.g. "Overture Overdrive" */
    const char          *vendor;      /* "AetherOS Audio" */
    int                  category;    /* APLUG_CAT_* */
    int                  num_params;
    const aplug_param_t *params;
    int                  num_inputs;  /* audio input channels (1 or 2) */
    int                  num_outputs; /* audio output channels */
} aplug_descriptor_t;

#define APLUG_VERSION(maj, min, patch) \
    (((unsigned)(maj) << 16) | ((unsigned)(min) << 8) | (unsigned)(patch))

/* ── Plugin instance vtable ──────────────────────────────────────────── */

typedef struct aplug_s aplug_t;

typedef struct {
    /* Called once after create; allocate DSP state here.
     * sr = sample rate, period = block size (frames).              */
    int  (*activate)(aplug_t *p, float sr, int period);

    /* Called once before destroy; free DSP state.                   */
    void (*deactivate)(aplug_t *p);

    /* Hot path: process n frames.  in/out may alias when in-place.
     * n == period given at activate(); guaranteed.                  */
    void (*process)(aplug_t *p,
                    const float *const *in,  /* [num_inputs][n]  */
                    float       *const *out, /* [num_outputs][n] */
                    int n);

    /* Set parameter value.  Called from the non-RT control thread;
     * implementations MUST be lock-free (use adsp_smooth_t target). */
    void (*param_set)(aplug_t *p, unsigned int id, float value);

    /* Read current parameter value (may be smoothed current).       */
    float (*param_get)(aplug_t *p, unsigned int id);
} aplug_vtable_t;

struct aplug_s {
    const aplug_descriptor_t *desc;
    const aplug_vtable_t     *vtable;
    void                     *state;   /* plugin-private DSP state */
};

/* ── Factory type ────────────────────────────────────────────────────── */

/* Factory allocates and returns a plugin instance.  Returns NULL on OOM. */
typedef aplug_t *(*aplug_factory_fn)(void);

/* ── Public API ──────────────────────────────────────────────────────── */

/* Registry: call aplug_registry_init() once at startup, then register
 * each plugin factory with aplug_registry_register().               */
void aplug_registry_init(void);
int  aplug_registry_register(aplug_factory_fn factory);

/* Lookup by unique id string.  Returns NULL if not found.            */
aplug_t *aplug_registry_create(const char *id);

/* Iterate plugins by category.  idx=0 returns first; returns NULL
 * when exhausted.  Use to populate a plugin browser.                */
aplug_t *aplug_registry_enum(int category, int *idx);

/* Convenience wrappers around the vtable.                            */
static inline int aplug_activate(aplug_t *p, float sr, int period)
{
    return p->vtable->activate(p, sr, period);
}

static inline void aplug_deactivate(aplug_t *p)
{
    p->vtable->deactivate(p);
}

static inline void aplug_process(aplug_t *p,
                                  const float *const *in,
                                  float       *const *out,
                                  int n)
{
    p->vtable->process(p, in, out, n);
}

static inline void aplug_param_set(aplug_t *p, unsigned int id, float v)
{
    p->vtable->param_set(p, id, v);
}

static inline float aplug_param_get(aplug_t *p, unsigned int id)
{
    return p->vtable->param_get(p, id);
}

/* Destroy a plugin instance (calls deactivate if not already called). */
void aplug_destroy(aplug_t *p);

/* Built-in plugin factories — called by aplug_register_builtins().   */
void aplug_register_builtins(void);

#endif /* AETHER_APLUG_H */
