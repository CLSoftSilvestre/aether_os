/*
 * AetherOS — IPv4 + ICMP (Phase 5.1)
 * File: kernel/net/ip.c
 */

#include "aether/ip.h"
#include "aether/net.h"
#include "aether/ethernet.h"
#include "aether/udp.h"
#include "aether/tcp.h"
#include "aether/printk.h"
#include "aether/types.h"

/* ── IP header ───────────────────────────────────────────────────────────── */
typedef struct {
    u8  ver_ihl;     /* version (4) | IHL in 32-bit words (5) */
    u8  dscp_ecn;
    u16 total_len;
    u16 id;
    u16 flags_frag;
    u8  ttl;
    u8  proto;
    u16 checksum;
    u8  src[4];
    u8  dst[4];
} __attribute__((packed)) ip_hdr_t;   /* 20 bytes */

#define IP_HDR_LEN  20u

/* ICMP header */
typedef struct {
    u8  type;
    u8  code;
    u16 checksum;
    u16 id;
    u16 seq;
} __attribute__((packed)) icmp_hdr_t;

#define ICMP_ECHO_REQUEST  8u
#define ICMP_ECHO_REPLY    0u

/* ── State ───────────────────────────────────────────────────────────────── */
static volatile u32 g_icmp_reply_id;
static volatile u32 g_icmp_reply_seq;
static volatile int g_icmp_reply_received;
static u16 g_ip_id;

/* ── helpers ─────────────────────────────────────────────────────────────── */
static void nm_copy(void *d, const void *s, u32 n) {
    u8 *dp=(u8*)d; const u8 *sp=(const u8*)s; while(n--) *dp++=*sp++;
}

static u32 ip_from_bytes(const u8 *b) {
    return ((u32)b[0]<<24)|((u32)b[1]<<16)|((u32)b[2]<<8)|(u32)b[3];
}
static void ip_to_bytes(u32 ip, u8 *b) {
    b[0]=(u8)(ip>>24); b[1]=(u8)(ip>>16); b[2]=(u8)(ip>>8); b[3]=(u8)ip;
}

/* ── Checksum ────────────────────────────────────────────────────────────── */

u16 ip_checksum(const void *data, u16 len)
{
    const u8 *p = (const u8 *)data;
    u32 sum = 0;
    while (len > 1) {
        sum += ((u32)p[0] << 8) | p[1];
        p += 2;
        len = (u16)(len - 2u);
    }
    if (len) sum += (u32)p[0] << 8;
    while (sum >> 16) sum = (sum & 0xFFFFu) + (sum >> 16);
    return (u16)(~sum);
}

/* Pseudo-header checksum for UDP/TCP (src/dst in host order) */
static u32 pseudo_hdr_sum(u32 src_ip, u32 dst_ip, u8 proto, u16 data_len)
{
    u32 s = 0;
    s += (src_ip >> 16) & 0xFFFFu;
    s += src_ip & 0xFFFFu;
    s += (dst_ip >> 16) & 0xFFFFu;
    s += dst_ip & 0xFFFFu;
    s += (u32)proto;
    s += (u32)data_len;
    return s;
}

/* ── TX buffer ───────────────────────────────────────────────────────────── */
static u8 g_ip_txbuf[1500];

/* ── Public: ip_tx ───────────────────────────────────────────────────────── */

int ip_tx(u32 dst_ip, u8 proto, const u8 *payload, u16 payload_len)
{
    u16 total = (u16)(IP_HDR_LEN + payload_len);
    if (total > (u16)sizeof(g_ip_txbuf)) return -1;

    /* Resolve destination MAC */
    u8 dst_mac[6];
    if (!arp_resolve(dst_ip, dst_mac)) {
        char s[16]; net_ip_str(dst_ip, s);
        kwarn("ip_tx: ARP failed for %s\n", s);
        return -1;
    }

    ip_hdr_t *hdr = (ip_hdr_t *)g_ip_txbuf;
    hdr->ver_ihl    = 0x45u;   /* IPv4, 5-word header */
    hdr->dscp_ecn   = 0;
    hdr->total_len  = net_htons(total);
    hdr->id         = net_htons(++g_ip_id);
    hdr->flags_frag = net_htons(0x4000u);   /* DF bit */
    hdr->ttl        = 64;
    hdr->proto      = proto;
    hdr->checksum   = 0;
    ip_to_bytes(g_our_ip, hdr->src);
    ip_to_bytes(dst_ip,   hdr->dst);
    hdr->checksum = ip_checksum(hdr, (u16)IP_HDR_LEN);

    nm_copy(g_ip_txbuf + IP_HDR_LEN, payload, payload_len);
    eth_tx(dst_mac, ETHERTYPE_IP, g_ip_txbuf, total);
    return 0;
}

/* ── ICMP ────────────────────────────────────────────────────────────────── */

static void icmp_rx(u32 src_ip, const u8 *icmp_pkt, u16 len)
{
    if (len < (u16)sizeof(icmp_hdr_t)) return;
    const icmp_hdr_t *hdr = (const icmp_hdr_t *)icmp_pkt;

    if (hdr->type == ICMP_ECHO_REQUEST) {
        /* Reply: swap src/dst, change type to 0, recalculate checksum */
        static u8 reply_buf[1500];
        if (len > (u16)sizeof(reply_buf)) return;
        nm_copy(reply_buf, icmp_pkt, len);
        icmp_hdr_t *r = (icmp_hdr_t *)reply_buf;
        r->type     = ICMP_ECHO_REPLY;
        r->code     = 0;
        r->checksum = 0;
        r->checksum = ip_checksum(reply_buf, len);
        ip_tx(src_ip, IP_PROTO_ICMP, reply_buf, len);
    } else if (hdr->type == ICMP_ECHO_REPLY) {
        if (net_ntohs(hdr->id) == (u16)g_icmp_reply_id) {
            g_icmp_reply_seq      = net_ntohs(hdr->seq);
            g_icmp_reply_received = 1;
        }
    }
}

/* ── Public: icmp_ping ───────────────────────────────────────────────────── */

u32 icmp_ping(u32 dst_ip)
{
    static u16 s_seq;
    u16 seq = ++s_seq;
    u16 id  = 0xAE00u;

    u8 pkt[8 + 32];
    icmp_hdr_t *hdr = (icmp_hdr_t *)pkt;
    hdr->type     = ICMP_ECHO_REQUEST;
    hdr->code     = 0;
    hdr->checksum = 0;
    hdr->id       = net_htons(id);
    hdr->seq      = net_htons(seq);
    /* 32-byte payload filled with 'A' */
    for (int i = 8; i < 40; i++) pkt[i] = 'A';
    hdr->checksum = ip_checksum(pkt, 40u);

    g_icmp_reply_id       = id;
    g_icmp_reply_received = 0;

    extern u64 timer_get_freq(void);
    extern void virtio_net_rx_poll(void);
    u64 freq = timer_get_freq();
    if (!freq) freq = 62500000ULL;

    u64 t_start;
    __asm__ volatile("mrs %0, CNTPCT_EL0" : "=r"(t_start));

    ip_tx(dst_ip, IP_PROTO_ICMP, pkt, 40u);

    u64 timeout = freq;   /* 1 second */
    while (!g_icmp_reply_received) {
        virtio_net_rx_poll();
        u64 now;
        __asm__ volatile("mrs %0, CNTPCT_EL0" : "=r"(now));
        if ((now - t_start) >= timeout) return (u32)-1;
    }

    u64 t_end;
    __asm__ volatile("mrs %0, CNTPCT_EL0" : "=r"(t_end));
    return (u32)((t_end - t_start) * 1000u / freq);   /* RTT in ms */
}

/* ── Public: ip_rx ───────────────────────────────────────────────────────── */

void ip_rx(const u8 *pkt, u16 len)
{
    if (len < IP_HDR_LEN) return;
    const ip_hdr_t *hdr = (const ip_hdr_t *)pkt;

    u8 ihl = (hdr->ver_ihl & 0x0Fu) * 4u;
    if (ihl < IP_HDR_LEN || ihl > len) return;
    if ((hdr->ver_ihl >> 4) != 4u) return;

    u32 dst_ip = ip_from_bytes(hdr->dst);
    /* Accept unicast for us or broadcast */
    if (dst_ip != g_our_ip && dst_ip != 0xFFFFFFFFu &&
        (dst_ip & ~g_subnet_mask) != ~g_subnet_mask)
        return;

    u32 src_ip      = ip_from_bytes(hdr->src);
    const u8 *upper = pkt + ihl;
    u16 upper_len   = (u16)(net_ntohs(hdr->total_len) - ihl);
    if (upper_len > len - ihl) upper_len = (u16)(len - ihl);

    switch (hdr->proto) {
    case IP_PROTO_ICMP: icmp_rx(src_ip, upper, upper_len); break;
    case IP_PROTO_UDP:  udp_rx(src_ip, upper, upper_len);  break;
    case IP_PROTO_TCP:  tcp_rx(src_ip, upper, upper_len);  break;
    }
}
