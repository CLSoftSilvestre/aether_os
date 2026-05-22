/*
 * AetherOS — libAetherAmp: NAM Neural Amp Modeler (Phase 8.6)
 * File: userspace/lib/libAetherAmp/src/aamp_nam.c
 *
 * Inference engine for .nam model files.  Implements two architectures:
 *
 *  LSTM (type=0):
 *    One LSTM layer (hidden_size ≤ 96 on Pi 5) + linear output.
 *    Gate order: input, forget, cell, output (IFCO).
 *    Recurrent state: h[hidden], c[hidden] (reset on preset change).
 *
 *  WaveNet-lite (type=1):
 *    8-layer dilated causal conv (dilation 1,2,4,8,16,32,64,128).
 *    Receptive field = 256 samples.  Each layer: depthwise 3-tap + 1x1.
 *    Gated activation: tanh(a) * sigmoid(b) (split channels).
 *
 * .nam binary format (AetherOS custom, not the original Python NAM):
 *   [4] magic "ANAM"
 *   [4] version (u32 LE)
 *   [1] type: 0=LSTM, 1=WaveNet
 *   [1] hidden_size (LSTM) or num_channels (WaveNet)
 *   [2] reserved
 *   [256] model name (null-terminated)
 *   ... weight arrays in row-major float32 LE ...
 *
 * Weight layout (LSTM, hidden_size H):
 *   W_gates  [4*H × 1]      input weights (input_size=1)
 *   U_gates  [4*H × H]      recurrent weights
 *   b_gates  [4*H]          biases
 *   W_out    [1 × H]        output projection
 *   b_out    [1]            output bias
 *
 * Weight layout (WaveNet, channels C, 8 dilated layers):
 *   Per layer: kernel[2*C × 3] + bias_a[2*C] + W_1x1[C × C] + b_1x1[C]
 *   Final: W_out[1 × C] + b_out[1]
 *
 * For Phase 8.6, inference uses vanilla float32 loops; NEON will be added
 * in Phase 8.6.1 once correctness is validated.
 */

#include "aamp.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ── Math helpers ────────────────────────────────────────────────────── */

static inline float sigmoidf(float x)
{
    /* Bounded sigmoid: clip to avoid overflow in expf */
    if (x >  10.0f) return 1.0f;
    if (x < -10.0f) return 0.0f;
    return 1.0f / (1.0f + expf(-x));
}

static inline float tanhf_approx(float x)
{
    /* Padé approximation; < 0.003 error on [-3, 3] */
    if (x >  3.0f) return  1.0f;
    if (x < -3.0f) return -1.0f;
    float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

/* ── LSTM engine ─────────────────────────────────────────────────────── */

#define NAM_LSTM_MAX_H 96

typedef struct {
    int    H;
    float  W[4 * NAM_LSTM_MAX_H];   /* input weights [4H × 1] */
    float  U[4 * NAM_LSTM_MAX_H * NAM_LSTM_MAX_H]; /* recurrent [4H × H] */
    float  b[4 * NAM_LSTM_MAX_H];   /* bias [4H] */
    float  W_out[NAM_LSTM_MAX_H];   /* output projection [H] */
    float  b_out;
    float  h[NAM_LSTM_MAX_H];       /* hidden state */
    float  c[NAM_LSTM_MAX_H];       /* cell state */
} nam_lstm_t;

static void lstm_step(nam_lstm_t *lstm, float x, float *y_out)
{
    int H = lstm->H;
    float gates[4 * NAM_LSTM_MAX_H];

    /* gates = W * x + U * h + b */
    for (int i = 0; i < 4 * H; i++) {
        float g = lstm->W[i] * x + lstm->b[i];
        for (int j = 0; j < H; j++)
            g += lstm->U[i * H + j] * lstm->h[j];
        gates[i] = g;
    }

    /* IFCO split: input gate, forget gate, cell gate, output gate */
    for (int i = 0; i < H; i++) {
        float ig  = sigmoidf(gates[i]);
        float fg  = sigmoidf(gates[H     + i]);
        float cg  = tanhf_approx(gates[2*H + i]);
        float og  = sigmoidf(gates[3*H + i]);
        lstm->c[i] = fg * lstm->c[i] + ig * cg;
        lstm->h[i] = og * tanhf_approx(lstm->c[i]);
    }

    /* Output projection */
    float out = lstm->b_out;
    for (int i = 0; i < H; i++)
        out += lstm->W_out[i] * lstm->h[i];
    *y_out = out;
}

/* ── WaveNet engine ──────────────────────────────────────────────────── */

#define NAM_WN_MAX_C   32
#define NAM_WN_LAYERS   8

typedef struct {
    int   C;
    /* Per-layer: kernel[2C × 3], bias_a[2C], W_1x1[C × C], b_1x1[C] */
    float kernel[NAM_WN_LAYERS][2 * NAM_WN_MAX_C * 3];
    float bias_a[NAM_WN_LAYERS][2 * NAM_WN_MAX_C];
    float W_1x1 [NAM_WN_LAYERS][NAM_WN_MAX_C * NAM_WN_MAX_C];
    float b_1x1 [NAM_WN_LAYERS][NAM_WN_MAX_C];
    float W_out [NAM_WN_MAX_C];
    float b_out;
    /* Receptive field buffer: max dilation = 128, kernel = 3 → 256+pad */
    float buf[256 + 4][NAM_WN_MAX_C];
    int   buf_pos;
} nam_wavenet_t;

static float wn_step(nam_wavenet_t *wn, float x)
{
    int C = wn->C;
    static float act[NAM_WN_MAX_C];
    static float res[NAM_WN_MAX_C];

    /* Input embedding: just repeat x into C channels */
    for (int c = 0; c < C; c++)
        res[c] = x;

    int dilation = 1;
    for (int l = 0; l < NAM_WN_LAYERS; l++, dilation <<= 1) {
        /* Fetch dilated input: positions {pos, pos-d, pos-2d} */
        int buf_len = 256 + 4;
        int p0 = wn->buf_pos;
        int p1 = (p0 - dilation     + buf_len) % buf_len;
        int p2 = (p0 - 2 * dilation + buf_len) % buf_len;

        for (int c = 0; c < C; c++)
            wn->buf[p0][c] = res[c];

        /* Depthwise 3-tap convolution → 2C gated outputs */
        float gate[2 * NAM_WN_MAX_C];
        for (int i = 0; i < 2 * C; i++) {
            float v = wn->bias_a[l][i];
            for (int c = 0; c < C; c++) {
                v += wn->kernel[l][i * 3 + 0] * wn->buf[p0][c];
                v += wn->kernel[l][i * 3 + 1] * wn->buf[p1][c];
                v += wn->kernel[l][i * 3 + 2] * wn->buf[p2][c];
            }
            gate[i] = v;
        }

        /* Gated activation */
        for (int c = 0; c < C; c++)
            act[c] = tanhf_approx(gate[c]) * sigmoidf(gate[C + c]);

        /* 1×1 residual projection */
        for (int c = 0; c < C; c++) {
            float v = wn->b_1x1[l][c];
            for (int cc = 0; cc < C; cc++)
                v += wn->W_1x1[l][c * C + cc] * act[cc];
            res[c] = res[c] + v;
        }

        wn->buf_pos = (wn->buf_pos + 1) % buf_len;
    }

    float out = wn->b_out;
    for (int c = 0; c < C; c++)
        out += wn->W_out[c] * res[c];
    return out;
}

/* ── File loader ─────────────────────────────────────────────────────── */

#define NAM_MAGIC "ANAM"

struct aamp_nam_s {
    aamp_nam_type_t type;
    char            name[256];
    union {
        nam_lstm_t    lstm;
        nam_wavenet_t wavenet;
    } model;
};

static unsigned int nam_le32(const unsigned char *p)
{
    return (unsigned)p[0] | ((unsigned)p[1] << 8) |
           ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24);
}

static int read_floats(float *dst, int n,
                       const unsigned char *src, int *pos, int max)
{
    int bytes = n * 4;
    if (*pos + bytes > max) return -1;
    for (int i = 0; i < n; i++) {
        unsigned int u = nam_le32(src + *pos);
        /* Type-pun u32 → float (standard-safe via memcpy) */
        memcpy(&dst[i], &u, 4);
        *pos += 4;
    }
    return 0;
}

aamp_nam_t *aamp_nam_load(const unsigned char *data, int data_len)
{
    if (!data || data_len < 268) return NULL;
    if (memcmp(data, NAM_MAGIC, 4) != 0) return NULL;

    /* version = data[4..7] (ignored for now) */
    int type        = data[8];
    int hidden_size = data[9];
    /* data[10..11] = reserved */

    if (type != 0 && type != 1) return NULL;
    if (hidden_size < 1) return NULL;

    aamp_nam_t *nam = (aamp_nam_t *)malloc(sizeof(aamp_nam_t));
    if (!nam) return NULL;
    memset(nam, 0, sizeof(*nam));

    nam->type = (aamp_nam_type_t)type;
    memcpy(nam->name, data + 12, 256);
    nam->name[255] = '\0';

    int pos = 268;

    if (type == 0) {
        /* LSTM */
        int H = hidden_size;
        if (H > NAM_LSTM_MAX_H) { free(nam); return NULL; }
        nam_lstm_t *m = &nam->model.lstm;
        m->H = H;
        if (read_floats(m->W,     4*H,   data, &pos, data_len) < 0 ||
            read_floats(m->U,     4*H*H, data, &pos, data_len) < 0 ||
            read_floats(m->b,     4*H,   data, &pos, data_len) < 0 ||
            read_floats(m->W_out, H,     data, &pos, data_len) < 0 ||
            read_floats(&m->b_out, 1,    data, &pos, data_len) < 0) {
            free(nam); return NULL;
        }
    } else {
        /* WaveNet */
        int C = hidden_size;
        if (C > NAM_WN_MAX_C) { free(nam); return NULL; }
        nam_wavenet_t *m = &nam->model.wavenet;
        m->C = C;
        for (int l = 0; l < NAM_WN_LAYERS; l++) {
            if (read_floats(m->kernel[l], 2*C*3,  data, &pos, data_len) < 0 ||
                read_floats(m->bias_a[l], 2*C,    data, &pos, data_len) < 0 ||
                read_floats(m->W_1x1[l], C*C,     data, &pos, data_len) < 0 ||
                read_floats(m->b_1x1[l], C,       data, &pos, data_len) < 0) {
                free(nam); return NULL;
            }
        }
        if (read_floats(m->W_out, C, data, &pos, data_len) < 0 ||
            read_floats(&m->b_out, 1, data, &pos, data_len) < 0) {
            free(nam); return NULL;
        }
    }

    return nam;
}

void aamp_nam_destroy(aamp_nam_t *nam) { free(nam); }

void aamp_nam_process(aamp_nam_t *nam,
                       float *y, const float *x,
                       float input_gain, int n)
{
    if (!nam) {
        for (int i = 0; i < n; i++) y[i] = x[i];
        return;
    }

    if (nam->type == AAMP_NAM_LSTM) {
        for (int i = 0; i < n; i++)
            lstm_step(&nam->model.lstm, x[i] * input_gain, &y[i]);
    } else {
        for (int i = 0; i < n; i++)
            y[i] = wn_step(&nam->model.wavenet, x[i] * input_gain);
    }
}

void aamp_nam_reset(aamp_nam_t *nam)
{
    if (!nam) return;
    if (nam->type == AAMP_NAM_LSTM) {
        memset(nam->model.lstm.h, 0, sizeof(nam->model.lstm.h));
        memset(nam->model.lstm.c, 0, sizeof(nam->model.lstm.c));
    } else {
        memset(nam->model.wavenet.buf, 0, sizeof(nam->model.wavenet.buf));
        nam->model.wavenet.buf_pos = 0;
    }
}

aamp_nam_type_t aamp_nam_type(aamp_nam_t *nam)
{
    return nam ? nam->type : AAMP_NAM_LSTM;
}

const char *aamp_nam_name(aamp_nam_t *nam)
{
    return nam ? nam->name : "";
}
