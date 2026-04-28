/*
 * AetherOS — UDP layer (Phase 5.1)
 * File: kernel/net/udp.c
 */

#include "aether/udp.h"
#include "aether/net.h"
#include "aether/ip.h"
#include "aether/types.h"

/* ── UDP header ──────────────────────────────────────────────────────────── */
typedef struct {
    u16 src_port;
    u16 dst_port;
    u16 length;
    u16 checksum;
} __attribute__((packed)) udp_hdr_t;

#define UDP_HDR_LEN  8u

/* ── Port handler table ──────────────────────────────────────────────────── */
#define UDP_BINDS_MAX  8
typedef struct {
    u16          port;
    udp_handler_t fn;
} udp_bind_t;

static udp_bind_t g_binds[UDP_BINDS_MAX];

void udp_bind(u16 port, udp_handler_t fn) {
    for (int i = 0; i < UDP_BINDS_MAX; i++) {
        if (!g_binds[i].fn) { g_binds[i].port = port; g_binds[i].fn = fn; return; }
    }
}
void udp_unbind(u16 port) {
    for (int i = 0; i < UDP_BINDS_MAX; i++)
        if (g_binds[i].port == port) { g_binds[i].fn = (udp_handler_t)0; g_binds[i].port = 0; }
}

/* ── Blocking receive state ──────────────────────────────────────────────── */
typedef struct {
    int  waiting;
    u16  port;
    u8  *buf;
    u16  maxlen;
    u32 *from_ip;
    u16 *from_port;
    int  received;
    int  rx_len;
} udp_wait_t;

static udp_wait_t g_udp_wait;

/* ── TX ──────────────────────────────────────────────────────────────────── */
static u8 g_udp_txbuf[1472];   /* max UDP payload for 1500 MTU */

int udp_send(u32 dst_ip, u16 dst_port, u16 src_port,
             const u8 *data, u16 data_len)
{
    u16 total = (u16)(UDP_HDR_LEN + data_len);
    if (total > (u16)sizeof(g_udp_txbuf)) return -1;

    udp_hdr_t *hdr = (udp_hdr_t *)g_udp_txbuf;
    hdr->src_port = net_htons(src_port);
    hdr->dst_port = net_htons(dst_port);
    hdr->length   = net_htons(total);
    hdr->checksum = 0;   /* UDP checksum optional in IPv4 */

    const u8 *src = data;
    u8 *dst = g_udp_txbuf + UDP_HDR_LEN;
    for (u16 i = 0; i < data_len; i++) dst[i] = src[i];

    return ip_tx(dst_ip, IP_PROTO_UDP, g_udp_txbuf, total);
}

/* ── RX ──────────────────────────────────────────────────────────────────── */

void udp_rx(u32 src_ip, const u8 *udp_pkt, u16 len)
{
    if (len < UDP_HDR_LEN) return;
    const udp_hdr_t *hdr = (const udp_hdr_t *)udp_pkt;

    u16 dst_port = net_ntohs(hdr->dst_port);
    u16 src_port = net_ntohs(hdr->src_port);
    const u8 *data = udp_pkt + UDP_HDR_LEN;
    u16 data_len = (u16)(len - UDP_HDR_LEN);

    /* Serve blocking wait first */
    if (g_udp_wait.waiting && g_udp_wait.port == dst_port &&
        !g_udp_wait.received) {
        u16 n = (data_len < g_udp_wait.maxlen) ? data_len : g_udp_wait.maxlen;
        for (u16 i = 0; i < n; i++) g_udp_wait.buf[i] = data[i];
        if (g_udp_wait.from_ip)   *g_udp_wait.from_ip   = src_ip;
        if (g_udp_wait.from_port) *g_udp_wait.from_port = src_port;
        g_udp_wait.rx_len  = (int)n;
        g_udp_wait.received = 1;
    }

    /* Dispatch to registered handler */
    for (int i = 0; i < UDP_BINDS_MAX; i++) {
        if (g_binds[i].fn && g_binds[i].port == dst_port) {
            g_binds[i].fn(src_ip, src_port, data, data_len);
            break;
        }
    }
}

/* ── Blocking receive ────────────────────────────────────────────────────── */

int udp_recv_blocking(u16 port, u8 *buf, u16 maxlen,
                      u32 *from_ip, u16 *from_port, u32 timeout_ms)
{
    if (!timeout_ms) timeout_ms = 1000u;

    g_udp_wait.waiting   = 1;
    g_udp_wait.port      = port;
    g_udp_wait.buf       = buf;
    g_udp_wait.maxlen    = maxlen;
    g_udp_wait.from_ip   = from_ip;
    g_udp_wait.from_port = from_port;
    g_udp_wait.received  = 0;
    g_udp_wait.rx_len    = -1;

    extern u64 timer_get_freq(void);
    extern void virtio_net_rx_poll(void);
    u64 freq = timer_get_freq();
    if (!freq) freq = 62500000ULL;
    u64 limit = (u64)timeout_ms * freq / 1000u;
    u64 start;
    __asm__ volatile("mrs %0, CNTPCT_EL0" : "=r"(start));

    while (!g_udp_wait.received) {
        virtio_net_rx_poll();
        u64 now;
        __asm__ volatile("mrs %0, CNTPCT_EL0" : "=r"(now));
        if ((now - start) >= limit) break;
    }

    int n = g_udp_wait.rx_len;
    g_udp_wait.waiting = 0;
    return n;
}
