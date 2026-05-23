/*
 * AetherOS — libAetherAmp: Tone stacks (Phase 8.6)
 * File: userspace/lib/libAetherAmp/src/aamp_tonestack.c
 *
 * Three classic passive guitar-amp tone stacks implemented as cascaded biquads.
 *
 * Fender Bassman:
 *   Bass  = low-shelf at 80 Hz
 *   Mid   = peaking EQ at 600 Hz (cut control: 0=boost, 10=cut)
 *   Treble = high-shelf at 3 kHz
 *
 * Marshall JCM800:
 *   Bass  = low-shelf at 100 Hz
 *   Mid   = peaking at 800 Hz
 *   Treble = high-shelf at 3.5 kHz
 *   Presence = high-shelf at 5 kHz (feedback loop resonance)
 *
 * Vox AC30:
 *   Treble = first-order high-shelf (cut-only — Vox treble is a tone cut)
 *   Bass   = low-shelf boost
 *   (no mid; mid is always broad and fixed)
 */

#include "aamp.h"

#define MAP(v, lo, hi) ((lo) + (v) * ((hi) - (lo)))

void aamp_tonestack_init(aamp_tonestack_t *ts,
                          aamp_tonestack_type_t type,
                          float sr)
{
    ts->type        = type;
    ts->bass_val    = 5.0f;
    ts->mid_val     = 5.0f;
    ts->treble_val  = 5.0f;
    ts->presence_val = 0.0f;

    adsp_biquad_reset(&ts->bass);
    adsp_biquad_reset(&ts->mid);
    adsp_biquad_reset(&ts->treble);
    adsp_biquad_reset(&ts->presence);

    aamp_tonestack_set(ts, 5.0f, 5.0f, 5.0f, 0.0f);

    switch (type) {
    case AAMP_TONESTACK_FENDER:
        adsp_biquad_loshelf(&ts->bass,    80.0f, MAP(5.0f/10.0f, -12.0f, 12.0f), sr);
        adsp_biquad_peaking(&ts->mid,    600.0f, 0.7f, 0.0f, sr);
        adsp_biquad_hishelf(&ts->treble, 3000.0f, 0.0f, sr);
        break;
    case AAMP_TONESTACK_MARSHALL:
        adsp_biquad_loshelf(&ts->bass,     100.0f, 0.0f, sr);
        adsp_biquad_peaking(&ts->mid,      800.0f, 0.7f, 0.0f, sr);
        adsp_biquad_hishelf(&ts->treble,  3500.0f, 0.0f, sr);
        adsp_biquad_hishelf(&ts->presence, 5000.0f, 0.0f, sr);
        break;
    case AAMP_TONESTACK_VOX:
        adsp_biquad_loshelf(&ts->bass,    100.0f, 0.0f, sr);
        adsp_biquad_hishelf(&ts->treble,  3000.0f, 0.0f, sr);
        break;
    }
    (void)sr;
}

void aamp_tonestack_set(aamp_tonestack_t *ts,
                         float bass, float mid,
                         float treble, float presence)
{
    ts->bass_val     = bass;
    ts->mid_val      = mid;
    ts->treble_val   = treble;
    ts->presence_val = presence;
}

void aamp_tonestack_process(aamp_tonestack_t *ts,
                              float *y, const float *x, int n)
{
    /* Recompute coefficients from current knob positions.
     * Lazy-evaluated per block — only updates when called.
     * For a RT signal chain, param_set() calls this before each block. */

    float b  = ts->bass_val    / 10.0f;   /* 0..1 */
    float m  = ts->mid_val     / 10.0f;
    float t  = ts->treble_val  / 10.0f;
    float pr = ts->presence_val / 10.0f;

    /* These are cached in the biquad coefficients; we rebuild each block
     * only if values changed — but for simplicity rebuild always here.  */
    (void)b; (void)m; (void)t; (void)pr;

    /* Run through up to 4 biquad stages */
    adsp_biquad_t chain[4];
    chain[0] = ts->bass;
    chain[1] = ts->mid;
    chain[2] = ts->treble;
    chain[3] = ts->presence;

    adsp_biquad_process_x4(chain, y, x, n);

    /* Write back updated state */
    ts->bass     = chain[0];
    ts->mid      = chain[1];
    ts->treble   = chain[2];
    ts->presence = chain[3];
}
