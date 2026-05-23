/*
 * AetherOS — libAetherChain: Signal chain engine (Phase 8.8)
 * File: userspace/lib/libAetherChain/src/achain.c
 */

#include "achain.h"
#include <stdlib.h>
#include <string.h>

/* ── Atomics (GCC built-ins, freestanding safe) ──────────────────────── */

#define ATOMIC_LOAD(p)      __atomic_load_n((p), __ATOMIC_ACQUIRE)
#define ATOMIC_STORE(p, v)  __atomic_store_n((p), (v), __ATOMIC_RELEASE)

/* ── Internal struct ─────────────────────────────────────────────────── */

typedef struct achain_s {
    /* Nodes */
    aplug_t        *nodes[ACHAIN_MAX_NODES];
    int             bypass[ACHAIN_MAX_NODES]; /* 1 = bypassed */
    int             node_count;

    /* Audio config */
    float           sr;
    int             period;

    /* Intermediate scratch buffers (stereo, period samples each) */
    float          *scratch_l;
    float          *scratch_r;

    /* SPSC lock-free param queue */
    achain_param_event_t  pq_buf[ACHAIN_PARAM_QUEUE];
    volatile unsigned int pq_write;     /* written by UI thread  */
    volatile unsigned int pq_read;      /* written by audio thread */

    /* MIDI routing table */
    achain_midi_route_t   midi_routes[ACHAIN_MIDI_ROUTES];
    int                   midi_route_count;
} achain_t;

/* ── Create / destroy ────────────────────────────────────────────────── */

achain_t *achain_create(float sr, int period)
{
    achain_t *c = (achain_t *)malloc(sizeof(achain_t));
    if (!c) return NULL;
    memset(c, 0, sizeof(*c));

    c->sr     = sr;
    c->period = period;

    c->scratch_l = (float *)malloc(sizeof(float) * (unsigned int)period);
    c->scratch_r = (float *)malloc(sizeof(float) * (unsigned int)period);
    if (!c->scratch_l || !c->scratch_r) {
        free(c->scratch_l);
        free(c->scratch_r);
        free(c);
        return NULL;
    }
    return c;
}

void achain_destroy(achain_t *c)
{
    if (!c) return;
    for (int i = 0; i < c->node_count; i++) {
        if (c->nodes[i]) {
            aplug_deactivate(c->nodes[i]);
            aplug_destroy(c->nodes[i]);
        }
    }
    free(c->scratch_l);
    free(c->scratch_r);
    free(c);
}

/* ── Node management ─────────────────────────────────────────────────── */

int achain_append(achain_t *c, aplug_t *plug)
{
    if (c->node_count >= ACHAIN_MAX_NODES) return -1;
    aplug_activate(plug, c->sr, c->period);
    int idx = c->node_count++;
    c->nodes[idx]  = plug;
    c->bypass[idx] = 0;
    return idx;
}

void achain_remove(achain_t *c, int node_idx)
{
    if (node_idx < 0 || node_idx >= c->node_count) return;
    aplug_deactivate(c->nodes[node_idx]);
    aplug_destroy(c->nodes[node_idx]);

    /* Shift remaining nodes down */
    for (int i = node_idx; i < c->node_count - 1; i++) {
        c->nodes[i]  = c->nodes[i + 1];
        c->bypass[i] = c->bypass[i + 1];
    }
    c->node_count--;
    c->nodes[c->node_count]  = NULL;
    c->bypass[c->node_count] = 0;
}

void achain_move(achain_t *c, int from, int to)
{
    if (from < 0 || from >= c->node_count) return;
    if (to   < 0 || to   >= c->node_count) return;
    if (from == to) return;

    aplug_t *plug   = c->nodes[from];
    int      byp    = c->bypass[from];
    int       step  = (from < to) ? 1 : -1;

    for (int i = from; i != to; i += step) {
        c->nodes[i]  = c->nodes[i + step];
        c->bypass[i] = c->bypass[i + step];
    }
    c->nodes[to]  = plug;
    c->bypass[to] = byp;
}

/* ── Bypass ──────────────────────────────────────────────────────────── */

void achain_set_bypass(achain_t *c, int node_idx, int bypass)
{
    if (node_idx < 0 || node_idx >= c->node_count) return;
    c->bypass[node_idx] = bypass ? 1 : 0;
}

int achain_get_bypass(achain_t *c, int node_idx)
{
    if (node_idx < 0 || node_idx >= c->node_count) return 0;
    return c->bypass[node_idx];
}

/* ── Info ─────────────────────────────────────────────────────────────── */

int     achain_node_count(achain_t *c) { return c->node_count; }
aplug_t *achain_node(achain_t *c, int idx) {
    if (idx < 0 || idx >= c->node_count) return NULL;
    return c->nodes[idx];
}
float achain_sample_rate(achain_t *c) { return c->sr; }

/* ── SPSC param queue ─────────────────────────────────────────────────── */

int achain_param_post(achain_t *c, int node_idx,
                      unsigned int param_id, float value)
{
    unsigned int w = ATOMIC_LOAD(&c->pq_write);
    unsigned int r = ATOMIC_LOAD(&c->pq_read);

    /* Full when (w - r) == ACHAIN_PARAM_QUEUE */
    if ((w - r) >= ACHAIN_PARAM_QUEUE) return -1;

    unsigned int slot = w & (ACHAIN_PARAM_QUEUE - 1);
    c->pq_buf[slot].node_idx = (unsigned short)node_idx;
    c->pq_buf[slot].param_id = (unsigned short)param_id;
    c->pq_buf[slot].value    = value;

    ATOMIC_STORE(&c->pq_write, w + 1);
    return 0;
}

/* Drain all pending param events (called from audio thread). */
static void drain_param_queue(achain_t *c)
{
    unsigned int r = c->pq_read;
    unsigned int w = ATOMIC_LOAD(&c->pq_write);

    while (r != w) {
        unsigned int slot = r & (ACHAIN_PARAM_QUEUE - 1);
        const achain_param_event_t *ev = &c->pq_buf[slot];

        int idx = ev->node_idx;
        if (idx >= 0 && idx < c->node_count && c->nodes[idx])
            aplug_param_set(c->nodes[idx], ev->param_id, ev->value);

        r++;
    }
    /* Release-store so UI thread sees updated read position */
    ATOMIC_STORE(&c->pq_read, r);
}

/* ── Process ─────────────────────────────────────────────────────────── */

void achain_process(achain_t *c,
                    const float *in,
                    float *out_l, float *out_r,
                    int n)
{
    drain_param_queue(c);

    if (c->node_count == 0) {
        /* Pass-through: duplicate mono to both channels */
        memcpy(out_l, in, sizeof(float) * (unsigned int)n);
        memcpy(out_r, in, sizeof(float) * (unsigned int)n);
        return;
    }

    /* Use scratch_l as the running mono bus; scratch_r for stereo mid-chain */
    float *cur_l = c->scratch_l;
    float *cur_r = c->scratch_r;
    memcpy(cur_l, in, sizeof(float) * (unsigned int)n);

    for (int i = 0; i < c->node_count; i++) {
        aplug_t *plug = c->nodes[i];
        if (!plug) continue;

        int is_last   = (i == c->node_count - 1);
        int bypassed  = c->bypass[i];

        if (bypassed) {
            /* Bypass: pass mono through unchanged.
             * If last node is stereo-native but bypassed, duplicate. */
            if (is_last) {
                memcpy(out_l, cur_l, sizeof(float) * (unsigned int)n);
                memcpy(out_r, cur_l, sizeof(float) * (unsigned int)n);
            }
            continue;
        }

        int num_out = plug->desc->num_outputs;

        if (is_last) {
            /* Final node writes directly into out_l / out_r */
            const float *ins[1]  = { cur_l };
            if (num_out >= 2) {
                float *outs[2] = { out_l, out_r };
                aplug_process(plug, ins, outs, n);
            } else {
                float *outs[1] = { out_l };
                aplug_process(plug, ins, outs, n);
                /* Duplicate mono → stereo */
                memcpy(out_r, out_l, sizeof(float) * (unsigned int)n);
            }
        } else {
            /* Mid-chain: process mono → mono (or stereo, but collapse) */
            const float *ins[1] = { cur_l };
            if (num_out >= 2) {
                float *outs[2] = { cur_l, cur_r };
                aplug_process(plug, ins, outs, n);
                /* Mix stereo down to mono for next stage */
                for (int s = 0; s < n; s++)
                    cur_l[s] = (cur_l[s] + cur_r[s]) * 0.5f;
            } else {
                float *outs[1] = { cur_l };
                aplug_process(plug, ins, outs, n);
            }
        }
    }
}

/* ── Preset system ───────────────────────────────────────────────────── */

/* .aepre layout helpers */
static unsigned int read_u32_le(const unsigned char *p)
{
    return (unsigned int)p[0]
         | ((unsigned int)p[1] << 8)
         | ((unsigned int)p[2] << 16)
         | ((unsigned int)p[3] << 24);
}

static void write_u32_le(unsigned char *p, unsigned int v)
{
    p[0] = (unsigned char)(v & 0xff);
    p[1] = (unsigned char)((v >> 8)  & 0xff);
    p[2] = (unsigned char)((v >> 16) & 0xff);
    p[3] = (unsigned char)((v >> 24) & 0xff);
}

static float read_f32_le(const unsigned char *p)
{
    unsigned int u = read_u32_le(p);
    float f;
    memcpy(&f, &u, 4);
    return f;
}

static void write_f32_le(unsigned char *p, float v)
{
    unsigned int u;
    memcpy(&u, &v, 4);
    write_u32_le(p, u);
}

/* Minimum bytes needed for a preset blob with nc nodes. */
static int preset_min_size(int nc, int total_params)
{
    /* 4 magic + 4 ver + 64 name + 4 node_count */
    /* per node: 64 plugin_id + 4 param_count + total_params*(4+4) */
    return 4 + 4 + 64 + 4 + nc * (64 + 4) + total_params * 8;
}

int achain_preset_load(achain_t *c, const unsigned char *data, int data_len)
{
    if (!data || data_len < 76) return -1; /* 4+4+64+4 minimum */

    /* magic */
    if (data[0]!='A'||data[1]!='E'||data[2]!='P'||data[3]!='R') return -1;

    /* version (ignored for now, accept any) */

    unsigned int nc = read_u32_le(data + 72);
    if (nc > ACHAIN_MAX_NODES) return -1;

    /* Destroy existing chain */
    for (int i = 0; i < c->node_count; i++) {
        if (c->nodes[i]) {
            aplug_deactivate(c->nodes[i]);
            aplug_destroy(c->nodes[i]);
            c->nodes[i] = NULL;
        }
        c->bypass[i] = 0;
    }
    c->node_count = 0;

    const unsigned char *p   = data + 76;
    const unsigned char *end = data + data_len;

    for (unsigned int ni = 0; ni < nc; ni++) {
        if (p + 68 > end) return -1; /* plugin_id + param_count */

        char plugin_id[65];
        memcpy(plugin_id, p, 64);
        plugin_id[64] = '\0';
        p += 64;

        unsigned int pc = read_u32_le(p);
        p += 4;

        if (p + pc * 8 > end) return -1;

        aplug_t *plug = aplug_registry_create(plugin_id);
        if (!plug) {
            /* Skip this node's params and continue */
            p += pc * 8;
            continue;
        }

        int idx = achain_append(c, plug);
        if (idx < 0) {
            aplug_destroy(plug);
            p += pc * 8;
            continue;
        }

        for (unsigned int pi = 0; pi < pc; pi++) {
            unsigned int param_id = read_u32_le(p);     p += 4;
            float        value    = read_f32_le(p);     p += 4;
            aplug_param_set(plug, param_id, value);
        }
    }

    return 0;
}

unsigned char *achain_preset_save(achain_t *c, int *out_len)
{
    /* First pass: count total params */
    int total_params = 0;
    for (int i = 0; i < c->node_count; i++) {
        if (c->nodes[i])
            total_params += c->nodes[i]->desc->num_params;
    }

    int len = preset_min_size(c->node_count, total_params);
    unsigned char *blob = (unsigned char *)malloc((unsigned int)len);
    if (!blob) return NULL;
    memset(blob, 0, (unsigned int)len);

    /* magic + version + name (empty) + node_count */
    blob[0]='A'; blob[1]='E'; blob[2]='P'; blob[3]='R';
    write_u32_le(blob + 4,  1);                       /* version 1 */
    /* name left as zeros (ACHAIN_PRESET_NAME bytes at offset 8) */
    write_u32_le(blob + 72, (unsigned int)c->node_count);

    unsigned char *p = blob + 76;

    for (int i = 0; i < c->node_count; i++) {
        aplug_t *plug = c->nodes[i];
        if (!plug) continue;

        /* plugin_id (64 bytes, null-padded) */
        int id_len = (int)strlen(plug->desc->id);
        if (id_len > 63) id_len = 63;
        memcpy(p, plug->desc->id, (unsigned int)id_len);
        p += 64;

        /* param_count */
        unsigned int np = (unsigned int)plug->desc->num_params;
        write_u32_le(p, np);
        p += 4;

        for (unsigned int pi = 0; pi < np; pi++) {
            unsigned int pid = plug->desc->params[pi].id;
            float        val = aplug_param_get(plug, pid);
            write_u32_le(p, pid);  p += 4;
            write_f32_le(p, val);  p += 4;
        }
    }

    if (out_len) *out_len = len;
    return blob;
}

void achain_preset_free_blob(unsigned char *blob)
{
    free(blob);
}

/* ── MIDI routing ────────────────────────────────────────────────────── */

int achain_midi_add_route(achain_t *c, const achain_midi_route_t *r)
{
    if (c->midi_route_count >= ACHAIN_MIDI_ROUTES) return -1;
    int slot = c->midi_route_count++;
    c->midi_routes[slot] = *r;
    return slot;
}

void achain_midi_clear_routes(achain_t *c)
{
    c->midi_route_count = 0;
}

void achain_midi_event(achain_t *c,
                       unsigned char status,
                       unsigned char data1,
                       unsigned char data2)
{
    unsigned char ch   = status & 0x0f;
    unsigned char type_nibble = status >> 4;

    /* CC = 0xBn, PC = 0xCn */
    int is_cc = (type_nibble == 0x0B);
    int is_pc = (type_nibble == 0x0C);
    if (!is_cc && !is_pc) return;

    for (int i = 0; i < c->midi_route_count; i++) {
        const achain_midi_route_t *r = &c->midi_routes[i];

        if (r->midi_ch != ch) continue;

        if (is_pc && r->midi_type == ACHAIN_MIDI_PC) {
            /* Program change: data1 = program number (0..127).
             * Route has no preset storage here — caller manages presets.
             * We surface this by posting param_id=0xFFFFFFFF, value=prog. */
            achain_param_post(c, (int)r->node_idx, 0xFFFFFFFFu,
                              (float)data1);
            continue;
        }

        if (is_cc && r->midi_type == ACHAIN_MIDI_CC
            && r->midi_num == data1)
        {
            /* Map data2 [0..127] → [value_lo .. value_hi] */
            float t   = (float)data2 / 127.0f;
            float val = r->value_lo + t * (r->value_hi - r->value_lo);
            achain_param_post(c, (int)r->node_idx,
                              r->param_id, val);
        }
    }
}
