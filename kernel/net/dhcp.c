/*
 * AetherOS — DHCP client (Phase 5.1)
 * File: kernel/net/dhcp.c
 *
 * DISCOVER → OFFER → REQUEST → ACK cycle.
 * Busy-polls virtio_net_rx_poll() — no IRQs required.
 * QEMU user networking: server at 10.0.2.3, assigns 10.0.2.15/24.
 */

#include "aether/dhcp.h"
#include "aether/net.h"
#include "aether/udp.h"
#include "aether/printk.h"
#include "aether/types.h"

#define DHCP_CLIENT_PORT  68u
#define DHCP_SERVER_PORT  67u
#define DHCP_MAGIC        0x63825363UL

/* DHCP message types (option 53) */
#define DHCP_DISCOVER  1u
#define DHCP_OFFER     2u
#define DHCP_REQUEST   3u
#define DHCP_ACK       5u

/* ── DHCP packet ─────────────────────────────────────────────────────────── */
typedef struct {
    u8  op;            /* 1=BOOTREQUEST, 2=BOOTREPLY */
    u8  htype;         /* 1=Ethernet */
    u8  hlen;          /* 6 */
    u8  hops;
    u32 xid;           /* transaction ID */
    u16 secs;
    u16 flags;         /* 0x8000 = broadcast */
    u32 ciaddr;        /* client IP */
    u32 yiaddr;        /* your IP */
    u32 siaddr;        /* server IP */
    u32 giaddr;        /* relay agent IP */
    u8  chaddr[16];    /* client hardware address */
    u8  sname[64];
    u8  file[128];
    u32 magic;         /* 0x63825363 */
    u8  options[308];
} __attribute__((packed)) dhcp_pkt_t;   /* 548 bytes total */

/* State shared with the UDP receive handler */
static volatile int  g_dhcp_got_offer;
static volatile int  g_dhcp_got_ack;
static volatile u32  g_dhcp_offered_ip;
static volatile u32  g_dhcp_server_ip;
static volatile u32  g_dhcp_subnet_mask;
static volatile u32  g_dhcp_gateway;
static volatile u32  g_dhcp_dns;
static volatile u32  g_dhcp_xid;

/* ── helpers ─────────────────────────────────────────────────────────────── */
static void nm_copy(void *d, const void *s, u32 n) {
    u8 *dp=(u8*)d; const u8 *sp=(const u8*)s; while(n--) *dp++=*sp++;
}
static void nm_set(void *d, u8 v, u32 n) {
    u8 *dp=(u8*)d; while(n--) *dp++=v;
}
static u32 u32_from_be(const u8 *b) {
    return ((u32)b[0]<<24)|((u32)b[1]<<16)|((u32)b[2]<<8)|(u32)b[3];
}
static void u32_to_be(u32 v, u8 *b) {
    b[0]=(u8)(v>>24); b[1]=(u8)(v>>16); b[2]=(u8)(v>>8); b[3]=(u8)v;
}

/* ── Option writer ───────────────────────────────────────────────────────── */
static int opt_write(u8 *opts, int pos, u8 code, const u8 *data, u8 len) {
    opts[pos++] = code;
    opts[pos++] = len;
    nm_copy(opts + pos, data, len);
    return pos + len;
}

/* ── Build and send a DHCP DISCOVER or REQUEST ───────────────────────────── */
static u8 g_dhcp_buf[548];

static void dhcp_send(u8 msg_type, u32 offer_ip, u32 server_ip)
{
    nm_set(g_dhcp_buf, 0, sizeof(g_dhcp_buf));
    dhcp_pkt_t *pkt = (dhcp_pkt_t *)g_dhcp_buf;

    pkt->op    = 1u;          /* BOOTREQUEST */
    pkt->htype = 1u;          /* Ethernet */
    pkt->hlen  = 6u;
    pkt->xid   = net_htonl(g_dhcp_xid);
    pkt->flags = net_htons(0x8000u);   /* broadcast */
    nm_copy(pkt->chaddr, g_our_mac, 6);
    pkt->magic = net_htonl(DHCP_MAGIC);

    int pos = 0;
    u8 type_val = msg_type;
    pos = opt_write(pkt->options, pos, 53, &type_val, 1);   /* msg type */

    if (msg_type == DHCP_REQUEST) {
        u8 req_ip[4], srv_ip[4];
        u32_to_be(offer_ip, req_ip);
        u32_to_be(server_ip, srv_ip);
        pos = opt_write(pkt->options, pos, 50, req_ip, 4);  /* requested IP */
        pos = opt_write(pkt->options, pos, 54, srv_ip, 4);  /* server ID */
    }

    u8 params[] = { 1, 3, 6, 15 };  /* subnet, router, DNS, domain */
    pos = opt_write(pkt->options, pos, 55, params, 4);

    pkt->options[pos++] = 255u;  /* END */

    udp_send(0xFFFFFFFFu, DHCP_SERVER_PORT, DHCP_CLIENT_PORT,
             g_dhcp_buf, (u16)sizeof(g_dhcp_buf));
}

/* ── Parse incoming DHCP response ────────────────────────────────────────── */
static void dhcp_rx_handler(u32 src_ip, u16 src_port,
                             const u8 *data, u16 data_len)
{
    (void)src_port;
    if (data_len < (u16)sizeof(dhcp_pkt_t)) return;

    const dhcp_pkt_t *pkt = (const dhcp_pkt_t *)data;
    if (pkt->op != 2u) return;                          /* not BOOTREPLY */
    if (net_ntohl(pkt->xid) != g_dhcp_xid) return;     /* wrong XID */
    if (net_ntohl(pkt->magic) != DHCP_MAGIC) return;

    /* Scan options */
    const u8 *opts = pkt->options;
    int  i = 0;
    u8   msg_type = 0;
    u32  sub_mask = 0, gateway = 0, dns_server = 0, server_id = 0;

    while (i < 308) {
        u8 code = opts[i++];
        if (code == 255u) break;
        if (code == 0)    continue;
        if (i >= 308) break;
        u8 len = opts[i++];
        if (i + len > 308) break;

        switch (code) {
        case 53: msg_type  = opts[i]; break;
        case  1: sub_mask  = u32_from_be(opts + i); break;
        case  3: gateway   = u32_from_be(opts + i); break;
        case  6: dns_server= u32_from_be(opts + i); break;
        case 54: server_id = u32_from_be(opts + i); break;
        }
        i += len;
    }

    u32 offered_ip = net_ntohl(pkt->yiaddr);

    if (msg_type == DHCP_OFFER && !g_dhcp_got_offer && offered_ip) {
        g_dhcp_offered_ip  = offered_ip;
        g_dhcp_server_ip   = server_id ? server_id : src_ip;
        g_dhcp_subnet_mask = sub_mask;
        g_dhcp_gateway     = gateway;
        g_dhcp_dns         = dns_server;
        g_dhcp_got_offer   = 1;
    } else if (msg_type == DHCP_ACK && offered_ip) {
        g_dhcp_offered_ip  = offered_ip;
        if (sub_mask)    g_dhcp_subnet_mask = sub_mask;
        if (gateway)     g_dhcp_gateway     = gateway;
        if (dns_server)  g_dhcp_dns         = dns_server;
        g_dhcp_got_ack     = 1;
    }
}

/* ── Public: dhcp_init ───────────────────────────────────────────────────── */

int dhcp_init(void)
{
    extern u64 timer_get_freq(void);
    extern void virtio_net_rx_poll(void);

    u64 freq = timer_get_freq();
    if (!freq) freq = 62500000ULL;

    /* Derive XID from counter */
    u64 t; __asm__ volatile("mrs %0, CNTPCT_EL0" : "=r"(t));
    g_dhcp_xid = (u32)t ^ (u32)(t >> 32) ^ 0xAE5A1700UL;

    g_dhcp_got_offer = 0;
    g_dhcp_got_ack   = 0;

    /* Register UDP handler on port 68 */
    udp_bind(DHCP_CLIENT_PORT, dhcp_rx_handler);

    /* DISCOVER phase: up to 3 retries */
    for (int attempt = 0; attempt < 3 && !g_dhcp_got_offer; attempt++) {
        kinfo("DHCP: DISCOVER (attempt %d)\n", attempt + 1);
        dhcp_send(DHCP_DISCOVER, 0, 0);

        u64 start; __asm__ volatile("mrs %0, CNTPCT_EL0" : "=r"(start));
        while (!g_dhcp_got_offer) {
            virtio_net_rx_poll();
            u64 now; __asm__ volatile("mrs %0, CNTPCT_EL0" : "=r"(now));
            if ((now - start) >= freq) break;   /* 1 s timeout per attempt */
        }
    }

    if (!g_dhcp_got_offer) {
        kwarn("DHCP: no OFFER received\n");
        udp_unbind(DHCP_CLIENT_PORT);
        return 0;
    }

    /* REQUEST phase */
    kinfo("DHCP: REQUEST for %u.%u.%u.%u from server %u.%u.%u.%u\n",
          (g_dhcp_offered_ip >> 24) & 0xFF, (g_dhcp_offered_ip >> 16) & 0xFF,
          (g_dhcp_offered_ip >>  8) & 0xFF,  g_dhcp_offered_ip         & 0xFF,
          (g_dhcp_server_ip  >> 24) & 0xFF, (g_dhcp_server_ip  >> 16) & 0xFF,
          (g_dhcp_server_ip  >>  8) & 0xFF,  g_dhcp_server_ip          & 0xFF);

    for (int attempt = 0; attempt < 3 && !g_dhcp_got_ack; attempt++) {
        dhcp_send(DHCP_REQUEST, g_dhcp_offered_ip, g_dhcp_server_ip);

        u64 start; __asm__ volatile("mrs %0, CNTPCT_EL0" : "=r"(start));
        while (!g_dhcp_got_ack) {
            virtio_net_rx_poll();
            u64 now; __asm__ volatile("mrs %0, CNTPCT_EL0" : "=r"(now));
            if ((now - start) >= freq) break;
        }
    }

    udp_unbind(DHCP_CLIENT_PORT);

    if (!g_dhcp_got_ack) {
        kwarn("DHCP: no ACK received\n");
        return 0;
    }

    /* Populate global state */
    g_our_ip      = g_dhcp_offered_ip;
    g_subnet_mask = g_dhcp_subnet_mask;
    g_gateway_ip  = g_dhcp_gateway;
    g_dns_ip      = g_dhcp_dns;
    g_net_ready   = 1;

    kinfo("DHCP: configured  IP=%u.%u.%u.%u  GW=%u.%u.%u.%u  DNS=%u.%u.%u.%u\n",
          (g_our_ip     >> 24) & 0xFF, (g_our_ip     >> 16) & 0xFF,
          (g_our_ip     >>  8) & 0xFF,  g_our_ip             & 0xFF,
          (g_gateway_ip >> 24) & 0xFF, (g_gateway_ip >> 16) & 0xFF,
          (g_gateway_ip >>  8) & 0xFF,  g_gateway_ip         & 0xFF,
          (g_dns_ip     >> 24) & 0xFF, (g_dns_ip     >> 16) & 0xFF,
          (g_dns_ip     >>  8) & 0xFF,  g_dns_ip             & 0xFF);

    return 1;
}
