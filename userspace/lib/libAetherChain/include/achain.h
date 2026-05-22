/*
 * AetherOS — libAetherChain: Signal chain engine (Phase 8.8)
 * File: userspace/lib/libAetherChain/include/achain.h
 *
 * Provides:
 *  1. achain_t      — DAG-ordered signal chain (mono → stereo out)
 *  2. achain_param_t — lock-free parameter event queue (SPSC)
 *  3. achain_preset_t — preset system (.aepre binary format)
 *  4. achain_midi_t  — MIDI CC/PC routing table
 *
 * Processing model:
 *   - Chain holds up to ACHAIN_MAX_NODES aplug_t* nodes in series
 *   - Mono input → node 0 → ... → node N-1 → stereo output
 *   - Nodes with num_outputs==1 duplicate to stereo at the final stage
 *   - Nodes with num_outputs==2 produce stereo natively (chorus, reverb…)
 *   - achain_process() drains the param queue before running DSP
 *
 * Lock-free param queue:
 *   - Single-producer (UI thread) / single-consumer (audio thread) SPSC
 *   - Capacity: ACHAIN_PARAM_QUEUE = 256 events
 *   - Events are { node_index, param_id, value }
 *   - Audio thread reads, applies, discards
 *
 * .aepre preset format:
 *   [4]  magic "AEPR"
 *   [4]  version u32 LE
 *   [64] preset name
 *   [4]  node_count u32
 *   Per node:
 *     [64] plugin_id string
 *     [4]  param_count u32
 *     Per param: [4] id u32, [4] value f32
 *
 * MIDI routing:
 *   Table of up to ACHAIN_MIDI_ROUTES entries.
 *   Each entry: { channel, cc_or_pc, node_index, param_id, lo, hi }
 *   PC (program change) → achain_preset_load() call
 */

#ifndef AETHER_ACHAIN_H
#define AETHER_ACHAIN_H

#include "aplug.h"

/* ── Limits ──────────────────────────────────────────────────────────── */

#define ACHAIN_MAX_NODES    16
#define ACHAIN_PARAM_QUEUE  256
#define ACHAIN_MIDI_ROUTES  64
#define ACHAIN_PRESET_NAME  64
#define ACHAIN_MAX_PRESETS  128

/* ── Parameter event ─────────────────────────────────────────────────── */

typedef struct {
    unsigned short node_idx;
    unsigned short param_id;
    float          value;
} achain_param_event_t;

/* ── Signal chain ────────────────────────────────────────────────────── */

typedef struct achain_s achain_t;

/* Create an empty chain.  sr = sample rate, period = block size. */
achain_t *achain_create(float sr, int period);
void      achain_destroy(achain_t *c);

/* Add/remove plugins.  append adds to the end of the series chain.
 * Returns node index on success, -1 on failure (chain full).          */
int  achain_append(achain_t *c, aplug_t *plug);
void achain_remove(achain_t *c, int node_idx);

/* Move node to a different position in the chain (for reordering).    */
void achain_move(achain_t *c, int from, int to);

/* Bypass / un-bypass a single node.                                   */
void achain_set_bypass(achain_t *c, int node_idx, int bypass);
int  achain_get_bypass(achain_t *c, int node_idx);

/* Hot-path: process one block.
 *   in  — mono input, n samples
 *   out_l, out_r — stereo output, n samples each
 * Drains param queue and applies all pending param changes first.      */
void achain_process(achain_t *c,
                    const float *in,
                    float *out_l, float *out_r,
                    int n);

/* Info */
int          achain_node_count(achain_t *c);
aplug_t     *achain_node(achain_t *c, int idx);
float        achain_sample_rate(achain_t *c);

/* ── Lock-free param queue (UI → audio thread) ───────────────────────── */

/* Post a parameter change from the UI thread.
 * Returns 0 on success, -1 if queue is full (caller may retry later).  */
int achain_param_post(achain_t *c, int node_idx,
                       unsigned int param_id, float value);

/* ── Preset system ───────────────────────────────────────────────────── */

typedef struct {
    char name[ACHAIN_PRESET_NAME];
} achain_preset_info_t;

/* Load a preset from raw memory blob.  Rebuilds chain nodes.
 * Must be called from the control thread (not the audio thread).       */
int  achain_preset_load(achain_t *c,
                         const unsigned char *data, int data_len);

/* Save current chain state to a heap-allocated blob.
 * Caller owns the returned pointer; free with achain_preset_free_blob. */
unsigned char *achain_preset_save(achain_t *c, int *out_len);
void           achain_preset_free_blob(unsigned char *blob);

/* ── MIDI routing ────────────────────────────────────────────────────── */

#define ACHAIN_MIDI_CC 0
#define ACHAIN_MIDI_PC 1   /* program change → preset load */

typedef struct {
    unsigned char midi_ch;     /* 0..15 */
    unsigned char midi_type;   /* ACHAIN_MIDI_CC / ACHAIN_MIDI_PC */
    unsigned char midi_num;    /* CC number or ignored for PC */
    unsigned char node_idx;
    unsigned int  param_id;
    float         value_lo;    /* maps MIDI 0 → value_lo */
    float         value_hi;    /* maps MIDI 127 → value_hi */
} achain_midi_route_t;

/* Register a MIDI→param mapping.  Returns route slot index or -1.     */
int  achain_midi_add_route(achain_t *c, const achain_midi_route_t *r);
void achain_midi_clear_routes(achain_t *c);

/* Call from the control thread when MIDI data arrives.
 * status = MIDI status byte (e.g. 0xB0=CC, 0xC0=PC).                  */
void achain_midi_event(achain_t *c,
                        unsigned char status,
                        unsigned char data1,
                        unsigned char data2);

#endif /* AETHER_ACHAIN_H */
