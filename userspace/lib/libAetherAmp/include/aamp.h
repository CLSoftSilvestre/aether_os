/*
 * AetherOS — libAetherAmp: Amp engine API (Phase 8.6)
 * File: userspace/lib/libAetherAmp/include/aamp.h
 *
 * Three sub-systems:
 *
 *  1. WAV loader — parse PCM WAV files from raw memory (for IR loading)
 *  2. Cabinet simulator — overlap-add convolution with a loaded IR
 *  3. NAM engine — Neural Amp Modeler: LSTM + WaveNet-style inference
 *     in fixed-point friendly float32; weights loaded from .nam files
 *
 *  4. Classic tone stacks (Fender/Marshall/Vox topology biquads)
 *
 * All processing is mono (guitar chain).  The cabinet sim widens to
 * stereo by duplicating with slight comb filtering for width.
 */

#ifndef AETHER_AAMP_H
#define AETHER_AAMP_H

#include "adsp.h"

/* ── WAV loader ──────────────────────────────────────────────────────── */

typedef struct {
    float        *samples;   /* caller-owned, malloc'd by aamp_wav_load */
    int           num_frames;
    int           sample_rate;
    int           channels;
} aamp_wav_t;

/* Parse a PCM WAV from raw memory.  Returns 0 on success.
 * Converts 16/24/32-bit PCM to float32.  Stereo → mono downmix if ch>1. */
int  aamp_wav_load(aamp_wav_t *w, const unsigned char *data, int data_len);
void aamp_wav_free(aamp_wav_t *w);

/* ── Cabinet simulator ───────────────────────────────────────────────── */

typedef struct aamp_cab_s aamp_cab_t;

/* Create cabinet sim from a mono IR (float32, ir_len frames). */
aamp_cab_t *aamp_cab_create(const float *ir, int ir_len,
                             int block_size, float sr);
void        aamp_cab_destroy(aamp_cab_t *cab);

/* Process one block.  in/out are mono; out_r gets the stereo widening copy. */
void aamp_cab_process(aamp_cab_t *cab,
                      const float *in,
                      float *out_l, float *out_r,
                      int n);

/* ── Tone stacks ─────────────────────────────────────────────────────── */

typedef enum {
    AAMP_TONESTACK_FENDER,   /* Bassman-style: bass / mid / treble */
    AAMP_TONESTACK_MARSHALL, /* JCM-style: bass / mid / treble / presence */
    AAMP_TONESTACK_VOX,      /* AC30-style: treble / bass cut filter */
} aamp_tonestack_type_t;

typedef struct {
    adsp_biquad_t bass;
    adsp_biquad_t mid;
    adsp_biquad_t treble;
    adsp_biquad_t presence;
    float         bass_val;
    float         mid_val;
    float         treble_val;
    float         presence_val;
    aamp_tonestack_type_t type;
} aamp_tonestack_t;

void aamp_tonestack_init(aamp_tonestack_t *ts,
                          aamp_tonestack_type_t type,
                          float sr);

void aamp_tonestack_set(aamp_tonestack_t *ts,
                         float bass, float mid,
                         float treble, float presence);

void aamp_tonestack_process(aamp_tonestack_t *ts,
                              float *y, const float *x, int n);

/* ── NAM engine ──────────────────────────────────────────────────────── */

/* .nam file format: simple binary header + weight arrays.
 * See aamp_nam.c for the on-disk layout.                              */

typedef enum {
    AAMP_NAM_LSTM,     /* LSTM-based model (accurate, ~0.5ms/block) */
    AAMP_NAM_WAVENET,  /* WaveNet-lite model (warmer, ~1ms/block)   */
} aamp_nam_type_t;

typedef struct aamp_nam_s aamp_nam_t;

/* Load a NAM model from raw memory (.nam binary blob).
 * Returns NULL on parse error or OOM.                                 */
aamp_nam_t *aamp_nam_load(const unsigned char *data, int data_len);
void        aamp_nam_destroy(aamp_nam_t *nam);

/* Process one block (mono in-place).  Applies input gain before inference. */
void aamp_nam_process(aamp_nam_t *nam,
                       float *y, const float *x,
                       float input_gain, int n);

/* Reset LSTM hidden state (when switching presets mid-stream). */
void aamp_nam_reset(aamp_nam_t *nam);

/* Model info */
aamp_nam_type_t aamp_nam_type(aamp_nam_t *nam);
const char     *aamp_nam_name(aamp_nam_t *nam);

#endif /* AETHER_AAMP_H */
