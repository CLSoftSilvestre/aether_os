/*
 * AetherOS — Network subsystem glue (Phase 5.1)
 * File: kernel/net/net.c
 *
 * Holds the global network state and bridges the VirtIO driver
 * to the Ethernet receive path.
 */

#include "aether/net.h"
#include "aether/ethernet.h"
#include "aether/dhcp.h"
#include "drivers/net/virtio_net.h"
#include "aether/printk.h"
#include "aether/types.h"

/* ── Global network state ────────────────────────────────────────────────── */
u32 g_our_ip      = 0;
u32 g_gateway_ip  = 0;
u32 g_subnet_mask = 0;
u32 g_dns_ip      = 0;
u8  g_our_mac[6]  = {0};
int g_net_ready   = 0;

/* ── net_deliver_frame: called by virtio_net_rx_poll ────────────────────── */
void net_deliver_frame(const u8 *buf, u16 len)
{
    eth_rx(buf, len);
}

/* ── net_tx_raw: called by ethernet layer ────────────────────────────────── */
int net_tx_raw(const u8 *buf, u16 len)
{
    return virtio_net_tx(buf, len);
}

/* ── net_rx_poll: called from timer IRQ at 100 Hz ───────────────────────── */
void net_rx_poll(void)
{
    virtio_net_rx_poll();
}

/* ── net_ip_parse: "a.b.c.d" → host-order u32 ───────────────────────────── */
u32 net_ip_parse(const char *s)
{
    u32 a = 0, b = 0, c = 0, d = 0;
    int dots = 0;
    u32 cur  = 0;
    for (; *s; s++) {
        if (*s >= '0' && *s <= '9') {
            cur = cur * 10u + (u32)(*s - '0');
            if (cur > 255u) return 0;
        } else if (*s == '.') {
            if (dots == 0) a = cur;
            else if (dots == 1) b = cur;
            else if (dots == 2) c = cur;
            else return 0;
            cur = 0;
            dots++;
        } else {
            return 0;
        }
    }
    if (dots != 3) return 0;
    d = cur;
    return (a << 24) | (b << 16) | (c << 8) | d;
}

/* ── net_init: called from kernel_main ───────────────────────────────────── */
void net_init(void)
{
    kinfo("NET: initializing VirtIO net...\n");

    if (!virtio_net_init()) {
        kwarn("NET: no network device — skipping DHCP\n");
        return;
    }

    kinfo("NET: running DHCP...\n");
    if (!dhcp_init()) {
        kwarn("NET: DHCP failed — no IP address\n");
        return;
    }

    arp_announce();
    kinfo("NET: ready\n");
}
