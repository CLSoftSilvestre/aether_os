/*
 * AetherOS — libAetherAmp: Cabinet simulator (Phase 8.6)
 * File: userspace/lib/libAetherAmp/src/aamp_cab.c
 *
 * Mono IR convolution via libAetherDSP OLA engine.
 * Stereo width: right channel uses a 1-tap comb (3 sample delay) for a
 * subtle spread without a second full convolution pass.
 */

#include "aamp.h"
#include <stdlib.h>
#include <string.h>

#define CAB_COMB_DELAY 3   /* samples; subtle L/R difference */

struct aamp_cab_s {
    adsp_conv_t *conv;
    float        comb_buf[CAB_COMB_DELAY];
    int          comb_pos;
};

aamp_cab_t *aamp_cab_create(const float *ir, int ir_len,
                              int block_size, float sr)
{
    (void)sr;
    if (!ir || ir_len <= 0 || block_size <= 0) return NULL;

    aamp_cab_t *cab = (aamp_cab_t *)malloc(sizeof(aamp_cab_t));
    if (!cab) return NULL;

    cab->conv     = adsp_conv_create(ir, ir_len, block_size);
    cab->comb_pos = 0;
    memset(cab->comb_buf, 0, sizeof(cab->comb_buf));

    if (!cab->conv) {
        free(cab);
        return NULL;
    }

    return cab;
}

void aamp_cab_destroy(aamp_cab_t *cab)
{
    if (!cab) return;
    adsp_conv_destroy(cab->conv);
    free(cab);
}

void aamp_cab_process(aamp_cab_t *cab,
                       const float *in,
                       float *out_l, float *out_r,
                       int n)
{
    /* Convolve into left channel */
    adsp_conv_process(cab->conv, out_l, in, n);

    /* Right channel: comb-delayed copy of convolved output */
    for (int i = 0; i < n; i++) {
        int rpos = (cab->comb_pos + CAB_COMB_DELAY - 1) % CAB_COMB_DELAY;
        float delayed = cab->comb_buf[rpos];
        cab->comb_buf[cab->comb_pos] = out_l[i];
        cab->comb_pos = (cab->comb_pos + 1) % CAB_COMB_DELAY;
        out_r[i] = out_l[i] * 0.9f + delayed * 0.1f;
    }
}
