/*
 * AetherOS — Ethernet II + ARP (Phase 5.1)
 * File: kernel/net/ethernet.c
 *
 * ARP table: 8 entries, FIFO replacement.
 * ARP requests are answered for our IP; replies populate the table.
 */

#include "aether/ethernet.h"
#include "aether/net.h"
#include "aether/ip.h"
#include "aether/printk.h"
#include "aether/types.h"

const u8 eth_broadcast[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

/* ── Frame buffer for outgoing frames ───────────────────────────────────── */
static u8 g_eth_txbuf[1514];

/* ── ARP table ───────────────────────────────────────────────────────────── */
#define ARP_TABLE_SZ  8
typedef struct {
    u32  ip;
    u8   mac[6];
    int  valid;
} arp_entry_t;

static arp_entry_t g_arp[ARP_TABLE_SZ];
static int         g_arp_next;

/* Pending ARP request state */
static volatile u32  g_arp_wait_ip;
static volatile int  g_arp_wait_done;
static u8            g_arp_wait_mac[6];

/* ── Internal memcpy/memset ──────────────────────────────────────────────── */
static void nm_copy(void *d, const void *s, u32 n) {
    u8 *dp=(u8*)d; const u8 *sp=(const u8*)s; while(n--) *dp++=*sp++;
}
static void nm_set(void *d, u8 v, u32 n) {
    u8 *dp=(u8*)d; while(n--) *dp++=v;
}
static int nm_eq(const void *a, const void *b, u32 n) {
    const u8 *ap=(const u8*)a, *bp=(const u8*)b;
    while(n--) if(*ap++!=*bp++) return 0; return 1;
}

/* ── ARP packet structure ────────────────────────────────────────────────── */
typedef struct {
    u16 hw_type;    /* 0x0001 = Ethernet */
    u16 proto;      /* 0x0800 = IPv4 */
    u8  hw_len;     /* 6 */
    u8  proto_len;  /* 4 */
    u16 op;         /* 1=request, 2=reply */
    u8  sender_mac[6];
    u8  sender_ip[4];
    u8  target_mac[6];
    u8  target_ip[4];
} __attribute__((packed)) arp_pkt_t;

/* ── Helpers ─────────────────────────────────────────────────────────────── */
static u32 ip_from_bytes(const u8 *b) {
    return ((u32)b[0]<<24)|((u32)b[1]<<16)|((u32)b[2]<<8)|(u32)b[3];
}
static void ip_to_bytes(u32 ip, u8 *b) {
    b[0]=(u8)(ip>>24); b[1]=(u8)(ip>>16); b[2]=(u8)(ip>>8); b[3]=(u8)ip;
}

/* ── ARP table operations ────────────────────────────────────────────────── */

void arp_update(u32 ip, const u8 *mac)
{
    /* Check existing entry */
    for (int i = 0; i < ARP_TABLE_SZ; i++) {
        if (g_arp[i].valid && g_arp[i].ip == ip) {
            nm_copy(g_arp[i].mac, mac, 6);
            return;
        }
    }
    /* Insert at next FIFO slot */
    g_arp[g_arp_next].ip = ip;
    nm_copy(g_arp[g_arp_next].mac, mac, 6);
    g_arp[g_arp_next].valid = 1;
    g_arp_next = (g_arp_next + 1) % ARP_TABLE_SZ;

    /* Wake pending arp_resolve if this is the IP we're waiting for */
    if (g_arp_wait_ip == ip) {
        nm_copy(g_arp_wait_mac, mac, 6);
        g_arp_wait_done = 1;
    }
}

static int arp_lookup(u32 ip, u8 *mac_out) {
    for (int i = 0; i < ARP_TABLE_SZ; i++) {
        if (g_arp[i].valid && g_arp[i].ip == ip) {
            nm_copy(mac_out, g_arp[i].mac, 6);
            return 1;
        }
    }
    return 0;
}

/* Send an ARP request for target_ip */
static void arp_request(u32 target_ip)
{
    arp_pkt_t pkt;
    pkt.hw_type   = net_htons(0x0001u);
    pkt.proto     = net_htons(0x0800u);
    pkt.hw_len    = 6;
    pkt.proto_len = 4;
    pkt.op        = net_htons(1u);   /* request */
    nm_copy(pkt.sender_mac, g_our_mac, 6);
    ip_to_bytes(g_our_ip, pkt.sender_ip);
    nm_set(pkt.target_mac, 0, 6);
    ip_to_bytes(target_ip, pkt.target_ip);

    eth_tx(eth_broadcast, ETHERTYPE_ARP, (u8*)&pkt, (u16)sizeof(pkt));
}

/* Send an ARP reply */
static void arp_reply(const u8 *dst_mac, u32 dst_ip)
{
    arp_pkt_t pkt;
    pkt.hw_type   = net_htons(0x0001u);
    pkt.proto     = net_htons(0x0800u);
    pkt.hw_len    = 6;
    pkt.proto_len = 4;
    pkt.op        = net_htons(2u);   /* reply */
    nm_copy(pkt.sender_mac, g_our_mac, 6);
    ip_to_bytes(g_our_ip, pkt.sender_ip);
    nm_copy(pkt.target_mac, dst_mac, 6);
    ip_to_bytes(dst_ip, pkt.target_ip);

    eth_tx(dst_mac, ETHERTYPE_ARP, (u8*)&pkt, (u16)sizeof(pkt));
}

void arp_announce(void)
{
    /* Gratuitous ARP: sender == target, broadcast */
    arp_pkt_t pkt;
    pkt.hw_type   = net_htons(0x0001u);
    pkt.proto     = net_htons(0x0800u);
    pkt.hw_len    = 6;
    pkt.proto_len = 4;
    pkt.op        = net_htons(1u);
    nm_copy(pkt.sender_mac, g_our_mac, 6);
    ip_to_bytes(g_our_ip, pkt.sender_ip);
    nm_set(pkt.target_mac, 0, 6);
    ip_to_bytes(g_our_ip, pkt.target_ip);

    eth_tx(eth_broadcast, ETHERTYPE_ARP, (u8*)&pkt, (u16)sizeof(pkt));
}

/* ── ARP receive handler ─────────────────────────────────────────────────── */

static void arp_handle(const u8 *payload, u16 len)
{
    if (len < (u16)sizeof(arp_pkt_t)) return;
    const arp_pkt_t *pkt = (const arp_pkt_t *)payload;

    u32 sender_ip = ip_from_bytes(pkt->sender_ip);
    u32 target_ip = ip_from_bytes(pkt->target_ip);
    u16 op        = net_ntohs(pkt->op);

    /* Learn sender MAC → IP regardless of op */
    if (sender_ip) arp_update(sender_ip, pkt->sender_mac);

    if (op == 1u && target_ip == g_our_ip && g_our_ip) {
        /* ARP request for us → reply */
        arp_reply(pkt->sender_mac, sender_ip);
    }
    /* ARP replies also handled via arp_update above */
}

/* ── Public: resolve ─────────────────────────────────────────────────────── */

int arp_resolve(u32 ip, u8 *mac_out)
{
    /* Check gateway: if on a different subnet, resolve gateway instead */
    u32 target = ip;
    if (g_subnet_mask && ((ip & g_subnet_mask) != (g_our_ip & g_subnet_mask)))
        target = g_gateway_ip;

    if (arp_lookup(target, mac_out)) return 1;

    /* Send ARP request and busy-poll for reply */
    g_arp_wait_ip   = target;
    g_arp_wait_done = 0;

    for (int attempt = 0; attempt < 3 && !g_arp_wait_done; attempt++) {
        arp_request(target);
        /* Poll ~100 ms worth of counter ticks */
        extern u64 timer_get_freq(void);
        extern void virtio_net_rx_poll(void);
        u64 freq = timer_get_freq();
        if (!freq) freq = 62500000ULL;
        u64 limit = freq / 10u;   /* 100 ms */
        extern u64 arm_cntpct_read(void);
        u64 start;
        __asm__ volatile("mrs %0, CNTPCT_EL0" : "=r"(start));
        while (!g_arp_wait_done) {
            virtio_net_rx_poll();
            u64 now;
            __asm__ volatile("mrs %0, CNTPCT_EL0" : "=r"(now));
            if ((now - start) >= limit) break;
        }
    }

    g_arp_wait_ip = 0;
    if (g_arp_wait_done) {
        nm_copy(mac_out, g_arp_wait_mac, 6);
        return 1;
    }
    return 0;
}

/* ── Public: transmit Ethernet frame ────────────────────────────────────── */

void eth_tx(const u8 *dst_mac, u16 ethertype,
            const u8 *payload, u16 payload_len)
{
    if (payload_len + ETH_HLEN > (u16)sizeof(g_eth_txbuf)) return;

    /* Ethernet header */
    nm_copy(g_eth_txbuf + 0, dst_mac,   6);    /* destination */
    nm_copy(g_eth_txbuf + 6, g_our_mac, 6);    /* source */
    g_eth_txbuf[12] = (u8)(ethertype >> 8);
    g_eth_txbuf[13] = (u8)(ethertype);
    nm_copy(g_eth_txbuf + 14, payload, payload_len);

    net_tx_raw(g_eth_txbuf, (u16)(ETH_HLEN + payload_len));
}

/* ── Public: receive dispatch ────────────────────────────────────────────── */

void eth_rx(const u8 *frame, u16 len)
{
    if (len < ETH_HLEN) return;

    u16 ethertype = ((u16)frame[12] << 8) | frame[13];
    const u8 *payload = frame + ETH_HLEN;
    u16 payload_len   = (u16)(len - ETH_HLEN);

    if (ethertype == ETHERTYPE_ARP)
        arp_handle(payload, payload_len);
    else if (ethertype == ETHERTYPE_IP)
        ip_rx(payload, payload_len);
}
