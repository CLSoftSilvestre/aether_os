/*
 * AetherOS — libAetherAmp: WAV loader (Phase 8.6)
 * File: userspace/lib/libAetherAmp/src/aamp_wav.c
 *
 * Parses PCM RIFF/WAVE files from in-memory buffers.
 * Handles: 16-bit, 24-bit, 32-bit integer PCM (format tag 1).
 * Stereo input → mono downmix.  No compression support needed for IRs.
 */

#include "aamp.h"
#include <stdlib.h>
#include <string.h>

/* Little-endian read helpers */
static unsigned int le32(const unsigned char *p)
{
    return (unsigned)p[0] | ((unsigned)p[1] << 8) |
           ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24);
}

static unsigned int le16(const unsigned char *p)
{
    return (unsigned)p[0] | ((unsigned)p[1] << 8);
}

int aamp_wav_load(aamp_wav_t *w, const unsigned char *data, int data_len)
{
    if (!w || !data || data_len < 44) return -1;

    /* RIFF header */
    if (memcmp(data, "RIFF", 4) != 0) return -1;
    if (memcmp(data + 8, "WAVE", 4) != 0) return -1;

    /* Walk chunks looking for fmt + data */
    int          fmt_found  = 0;
    int          data_found = 0;
    unsigned int channels   = 0;
    unsigned int sample_rate = 0;
    unsigned int bits        = 0;
    const unsigned char *pcm_ptr = NULL;
    unsigned int pcm_bytes = 0;

    int pos = 12;
    while (pos + 8 <= data_len) {
        unsigned int chunk_size = le32(data + pos + 4);

        if (memcmp(data + pos, "fmt ", 4) == 0) {
            if (chunk_size < 16) return -1;
            unsigned int fmt_tag = le16(data + pos + 8);
            if (fmt_tag != 1) return -1;   /* PCM only */
            channels    = le16(data + pos + 10);
            sample_rate = le32(data + pos + 12);
            bits        = le16(data + pos + 22);
            fmt_found   = 1;
        } else if (memcmp(data + pos, "data", 4) == 0) {
            pcm_ptr    = data + pos + 8;
            pcm_bytes  = chunk_size;
            data_found = 1;
        }

        pos += 8 + (int)chunk_size;
        if (chunk_size & 1) pos++;   /* word-align */
    }

    if (!fmt_found || !data_found) return -1;
    if (channels == 0 || (bits != 16 && bits != 24 && bits != 32)) return -1;

    int bytes_per_sample = (int)bits / 8;
    int total_samples    = (int)pcm_bytes / (bytes_per_sample * (int)channels);
    if (total_samples <= 0) return -1;

    w->samples     = (float *)malloc(sizeof(float) * total_samples);
    w->num_frames  = total_samples;
    w->sample_rate = (int)sample_rate;
    w->channels    = (int)channels;

    if (!w->samples) return -1;

    const unsigned char *src = pcm_ptr;
    for (int i = 0; i < total_samples; i++) {
        float s = 0.0f;

        if (channels == 1) {
            if (bits == 16) {
                short v = (short)le16(src);
                s = v / 32768.0f;
                src += 2;
            } else if (bits == 24) {
                int v = (int)(src[0] | ((unsigned)src[1] << 8) |
                              ((unsigned)src[2] << 16));
                if (v & 0x800000) v |= (int)0xFF000000;
                s = v / 8388608.0f;
                src += 3;
            } else {   /* 32-bit */
                int v = (int)le32(src);
                s = v / 2147483648.0f;
                src += 4;
            }
        } else {
            /* Stereo: downmix L+R to mono */
            float L = 0.0f, R = 0.0f;
            if (bits == 16) {
                short vL = (short)le16(src); src += 2;
                short vR = (short)le16(src); src += 2;
                L = vL / 32768.0f;
                R = vR / 32768.0f;
            } else if (bits == 24) {
                int vL = (int)(src[0] | ((unsigned)src[1] << 8) |
                                ((unsigned)src[2] << 16));
                if (vL & 0x800000) vL |= (int)0xFF000000;
                src += 3;
                int vR = (int)(src[0] | ((unsigned)src[1] << 8) |
                                ((unsigned)src[2] << 16));
                if (vR & 0x800000) vR |= (int)0xFF000000;
                src += 3;
                L = vL / 8388608.0f;
                R = vR / 8388608.0f;
            } else {
                int vL = (int)le32(src); src += 4;
                int vR = (int)le32(src); src += 4;
                L = vL / 2147483648.0f;
                R = vR / 2147483648.0f;
            }
            s = (L + R) * 0.5f;
        }

        w->samples[i] = s;
    }

    return 0;
}

void aamp_wav_free(aamp_wav_t *w)
{
    if (!w) return;
    free(w->samples);
    w->samples    = NULL;
    w->num_frames = 0;
}
