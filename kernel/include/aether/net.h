/*
 * AetherOS — Network subsystem global state & entry points
 * File: kernel/include/aether/net.h
 *
 * IPs are stored in host byte order (u32).
 * Use htonl()/ntohl() when building/parsing packets.
 */
#ifndef AETHER_NET_H
#define AETHER_NET_H

#include "aether/types.h"

/* ── Global network state (set by DHCP; read-only after init) ─────────────── */
extern u32 g_our_ip;        /* our IPv4 address          (host byte order) */
extern u32 g_gateway_ip;    /* default gateway            (host byte order) */
extern u32 g_subnet_mask;   /* subnet mask                (host byte order) */
extern u32 g_dns_ip;        /* DNS server                 (host byte order) */
extern u8  g_our_mac[6];    /* our Ethernet MAC address                     */
extern int g_net_ready;     /* 1 after DHCP/static config completes         */

/* ── Byte-order helpers ────────────────────────────────────────────────────── */
static inline u16 net_htons(u16 x) {
    return (u16)((x >> 8) | (x << 8));
}
static inline u32 net_htonl(u32 x) {
    return ((x & 0xFFu) << 24) | ((x & 0xFF00u) << 8) |
           ((x >> 8)  & 0xFF00u) | ((x >> 24) & 0xFFu);
}
#define net_ntohs  net_htons
#define net_ntohl  net_htonl

/* ── IP address utilities ──────────────────────────────────────────────────── */
/* Print a host-order IP as "a.b.c.d" into buf (needs at least 16 bytes). */
static inline void net_ip_str(u32 ip, char *buf) {
    char *p = buf;
    for (int sh = 24; sh >= 0; sh -= 8) {
        u8 v = (u8)(ip >> sh);
        if (v >= 100) { *p++ = (char)('0' + v / 100); v = (u8)(v % 100);
                        *p++ = (char)('0' + v / 10);  v = (u8)(v % 10); }
        else if (v >= 10) { *p++ = (char)('0' + v / 10); v = (u8)(v % 10); }
        *p++ = (char)('0' + v);
        if (sh > 0) *p++ = '.';
    }
    *p = '\0';
}

/* Parse "a.b.c.d" → host-order u32, returns 0 on parse error */
u32 net_ip_parse(const char *s);

/* ── Subsystem init & poll ─────────────────────────────────────────────────── */

/* Called from kernel_main() before process_spawn. */
void net_init(void);

/* Called from timer_irq_handler() at 100 Hz. */
void net_rx_poll(void);

/* Called by virtio_net_rx_poll() for each received Ethernet frame. */
void net_deliver_frame(const u8 *buf, u16 len);

/* Called by upper layers to transmit a raw Ethernet frame. */
int  net_tx_raw(const u8 *buf, u16 len);

#endif /* AETHER_NET_H */
