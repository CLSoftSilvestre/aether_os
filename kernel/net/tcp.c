/*
 * AetherOS — TCP client, single-connection state machine (Phase 5.1)
 * File: kernel/net/tcp.c
 *
 * Supports one active connection at a time.
 * No retransmission timers.  Reliable enough for local/QEMU connections.
 *
 * State machine:
 *   CLOSED → (SYN sent) → SYN_SENT → (SYN-ACK) → ESTABLISHED
 *   ESTABLISHED → (FIN sent) → FIN_WAIT1 → (FIN-ACK) → FIN_WAIT2
 *   FIN_WAIT2 → (remote FIN) → TIME_WAIT → CLOSED
 */

#include "aether/tcp.h"
#include "aether/net.h"
#include "aether/ip.h"
#include "aether/printk.h"
#include "aether/types.h"

/* ── TCP header ──────────────────────────────────────────────────────────── */
typedef struct {
    u16 src_port;
    u16 dst_port;
    u32 seq;
    u32 ack;
    u8  data_off;   /* high nibble = header length in 32-bit words */
    u8  flags;
    u16 window;
    u16 checksum;
    u16 urgent;
} __attribute__((packed)) tcp_hdr_t;

#define TCP_HDR_LEN  20u

#define TCP_FIN  0x01u
#define TCP_SYN  0x02u
#define TCP_RST  0x04u
#define TCP_PSH  0x08u
#define TCP_ACK  0x10u

/* ── State ───────────────────────────────────────────────────────────────── */
typedef enum {
    TCP_CLOSED      = 0,
    TCP_SYN_SENT    = 1,
    TCP_ESTABLISHED = 2,
    TCP_FIN_WAIT1   = 3,
    TCP_FIN_WAIT2   = 4,
    TCP_TIME_WAIT   = 5,
    TCP_CLOSE_WAIT  = 6,
} tcp_state_t;

#define TCP_RX_BUF_SZ  8192u

typedef struct {
    tcp_state_t state;
    u32  remote_ip;
    u16  remote_port;
    u16  local_port;
    u32  snd_seq;    /* next sequence number to send */
    u32  snd_ack;    /* what we have acknowledged from remote */
    u32  rcv_nxt;    /* next byte we expect from remote */
    u8   rx_buf[TCP_RX_BUF_SZ];
    u16  rx_head;
    u16  rx_tail;
} tcp_conn_t;

static tcp_conn_t g_conn;
static u16 g_next_local_port = 49152u;

/* ── helpers ─────────────────────────────────────────────────────────────── */
static void nm_copy(void *d, const void *s, u32 n) {
    u8 *dp=(u8*)d; const u8 *sp=(const u8*)s; while(n--) *dp++=*sp++;
}

static u64 cntpct(void) {
    u64 v; __asm__ volatile("mrs %0, CNTPCT_EL0":"=r"(v)); return v;
}
static u64 cntfrq(void) {
    u64 v; __asm__ volatile("mrs %0, CNTFRQ_EL0":"=r"(v));
    return v ? v : 62500000ULL;
}

/* ── Checksum (TCP pseudo-header) ────────────────────────────────────────── */
static u16 tcp_checksum(u32 src_ip, u32 dst_ip,
                         const u8 *tcp_seg, u16 len)
{
    extern u16 ip_checksum(const void *data, u16 len);

    u32 sum = 0;
    /* Pseudo-header */
    sum += (src_ip >> 16) & 0xFFFFu;
    sum += src_ip & 0xFFFFu;
    sum += (dst_ip >> 16) & 0xFFFFu;
    sum += dst_ip & 0xFFFFu;
    sum += IP_PROTO_TCP;
    sum += (u32)len;

    /* TCP segment */
    const u8 *p = tcp_seg;
    u16 n = len;
    while (n > 1) {
        sum += ((u32)p[0] << 8) | p[1];
        p += 2; n = (u16)(n - 2u);
    }
    if (n) sum += (u32)p[0] << 8;

    while (sum >> 16) sum = (sum & 0xFFFFu) + (sum >> 16);
    return (u16)(~sum);
}

/* ── Build and send a TCP segment ────────────────────────────────────────── */
static u8 g_tcp_txbuf[1500];

static int tcp_send_seg(u8 flags, const u8 *data, u16 data_len)
{
    u16 total = (u16)(TCP_HDR_LEN + data_len);
    if (total > (u16)(sizeof(g_tcp_txbuf))) return -1;

    tcp_hdr_t *hdr = (tcp_hdr_t *)g_tcp_txbuf;
    hdr->src_port = net_htons(g_conn.local_port);
    hdr->dst_port = net_htons(g_conn.remote_port);
    hdr->seq      = net_htonl(g_conn.snd_seq);
    hdr->ack      = (flags & TCP_ACK) ? net_htonl(g_conn.rcv_nxt) : 0;
    hdr->data_off = (u8)((TCP_HDR_LEN / 4u) << 4);
    hdr->flags    = flags;
    hdr->window   = net_htons(4096u);
    hdr->checksum = 0;
    hdr->urgent   = 0;

    if (data_len) nm_copy(g_tcp_txbuf + TCP_HDR_LEN, data, data_len);

    hdr->checksum = net_htons(tcp_checksum(g_our_ip, g_conn.remote_ip,
                                           g_tcp_txbuf, total));

    /* Advance SND_SEQ for data-bearing or SYN/FIN segments */
    if (data_len || (flags & (TCP_SYN | TCP_FIN)))
        g_conn.snd_seq += (u32)data_len + ((flags & (TCP_SYN | TCP_FIN)) ? 1u : 0u);

    return ip_tx(g_conn.remote_ip, IP_PROTO_TCP, g_tcp_txbuf, total);
}

/* ── Public: tcp_connect ─────────────────────────────────────────────────── */

int tcp_connect(u32 dst_ip, u16 dst_port)
{
    if (g_conn.state != TCP_CLOSED) return -1;

    g_conn.remote_ip    = dst_ip;
    g_conn.remote_port  = dst_port;
    g_conn.local_port   = g_next_local_port++;
    if (g_next_local_port < 49152u) g_next_local_port = 49152u;

    /* Seed SND_SEQ from counter */
    u64 t = cntpct();
    g_conn.snd_seq  = (u32)(t ^ (t >> 32));
    g_conn.snd_ack  = 0;
    g_conn.rcv_nxt  = 0;
    g_conn.rx_head  = 0;
    g_conn.rx_tail  = 0;
    g_conn.state    = TCP_SYN_SENT;

    extern void virtio_net_rx_poll(void);
    u64 freq = cntfrq();

    for (int attempt = 0; attempt < 3 && g_conn.state == TCP_SYN_SENT; attempt++) {
        tcp_send_seg(TCP_SYN, (void*)0, 0);

        u64 start = cntpct();
        while (g_conn.state == TCP_SYN_SENT) {
            virtio_net_rx_poll();
            if ((cntpct() - start) >= freq) break;   /* 1 s timeout */
        }
    }

    if (g_conn.state != TCP_ESTABLISHED) {
        g_conn.state = TCP_CLOSED;
        return -1;
    }
    return 0;   /* handle = 0 (only one connection) */
}

/* ── Public: tcp_send ────────────────────────────────────────────────────── */

int tcp_send(int handle, const u8 *data, u16 len)
{
    (void)handle;
    if (g_conn.state != TCP_ESTABLISHED) return -1;
    return tcp_send_seg(TCP_PSH | TCP_ACK, data, len);
}

/* ── Public: tcp_recv ────────────────────────────────────────────────────── */

int tcp_recv(int handle, u8 *buf, u16 maxlen, u32 timeout_ms)
{
    (void)handle;
    if (g_conn.state != TCP_ESTABLISHED &&
        g_conn.state != TCP_CLOSE_WAIT) return -1;

    extern void virtio_net_rx_poll(void);
    u64 freq = cntfrq();
    if (!timeout_ms) timeout_ms = 5000u;
    u64 limit = (u64)timeout_ms * freq / 1000u;
    u64 start = cntpct();

    while (g_conn.rx_head == g_conn.rx_tail &&
           g_conn.state == TCP_ESTABLISHED) {
        virtio_net_rx_poll();
        if ((cntpct() - start) >= limit) return -1;
    }

    /* Drain RX ring */
    int n = 0;
    while (n < (int)maxlen && g_conn.rx_head != g_conn.rx_tail) {
        buf[n++] = g_conn.rx_buf[g_conn.rx_tail % TCP_RX_BUF_SZ];
        g_conn.rx_tail++;
    }
    return n;
}

/* ── Public: tcp_close ───────────────────────────────────────────────────── */

void tcp_close(int handle)
{
    (void)handle;
    if (g_conn.state == TCP_ESTABLISHED || g_conn.state == TCP_CLOSE_WAIT) {
        g_conn.state = TCP_FIN_WAIT1;
        tcp_send_seg(TCP_FIN | TCP_ACK, (void*)0, 0);

        extern void virtio_net_rx_poll(void);
        u64 freq = cntfrq();
        u64 start = cntpct();
        while (g_conn.state != TCP_TIME_WAIT &&
               g_conn.state != TCP_CLOSED) {
            virtio_net_rx_poll();
            if ((cntpct() - start) >= freq * 3u) break;
        }
    }
    g_conn.state = TCP_CLOSED;
}

/* ── Public: tcp_rx (called from ip_rx) ─────────────────────────────────── */

void tcp_rx(u32 src_ip, const u8 *tcp_seg, u16 len)
{
    if (len < TCP_HDR_LEN) return;
    const tcp_hdr_t *hdr = (const tcp_hdr_t *)tcp_seg;

    u16 src_port = net_ntohs(hdr->src_port);
    u16 dst_port = net_ntohs(hdr->dst_port);
    u8  flags    = hdr->flags;
    u32 seq      = net_ntohl(hdr->seq);
    u32 ack_num  = net_ntohl(hdr->ack);
    u8  hdr_len  = (u8)((hdr->data_off >> 4) * 4u);

    if (hdr_len < TCP_HDR_LEN || hdr_len > len) return;

    /* Only process packets for our connection */
    if (src_ip   != g_conn.remote_ip)   return;
    if (src_port != g_conn.remote_port) return;
    if (dst_port != g_conn.local_port)  return;

    if (flags & TCP_RST) {
        g_conn.state = TCP_CLOSED;
        return;
    }

    const u8 *data     = tcp_seg + hdr_len;
    u16       data_len = (u16)(len - hdr_len);

    switch (g_conn.state) {
    case TCP_SYN_SENT:
        if ((flags & (TCP_SYN | TCP_ACK)) == (TCP_SYN | TCP_ACK)) {
            g_conn.rcv_nxt = seq + 1u;
            g_conn.snd_ack = ack_num;
            g_conn.state   = TCP_ESTABLISHED;
            tcp_send_seg(TCP_ACK, (void*)0, 0);
        }
        break;

    case TCP_ESTABLISHED:
    case TCP_CLOSE_WAIT:
        if (flags & TCP_ACK) g_conn.snd_ack = ack_num;

        /* Buffer incoming data */
        if (data_len > 0) {
            g_conn.rcv_nxt += (u32)data_len;
            for (u16 i = 0; i < data_len; i++) {
                u16 slot = g_conn.rx_head % TCP_RX_BUF_SZ;
                g_conn.rx_buf[slot] = data[i];
                g_conn.rx_head++;
            }
            /* ACK the data */
            tcp_send_seg(TCP_ACK, (void*)0, 0);
        }

        /* Remote sent FIN */
        if (flags & TCP_FIN) {
            g_conn.rcv_nxt++;
            tcp_send_seg(TCP_ACK, (void*)0, 0);
            if (g_conn.state == TCP_ESTABLISHED)
                g_conn.state = TCP_CLOSE_WAIT;
            else
                g_conn.state = TCP_TIME_WAIT;
        }
        break;

    case TCP_FIN_WAIT1:
        if (flags & TCP_ACK) g_conn.state = TCP_FIN_WAIT2;
        if (flags & TCP_FIN) {
            g_conn.rcv_nxt++;
            tcp_send_seg(TCP_ACK, (void*)0, 0);
            g_conn.state = TCP_TIME_WAIT;
        }
        break;

    case TCP_FIN_WAIT2:
        if (flags & TCP_FIN) {
            g_conn.rcv_nxt++;
            tcp_send_seg(TCP_ACK, (void*)0, 0);
            g_conn.state = TCP_TIME_WAIT;
        }
        break;

    default:
        break;
    }
}
