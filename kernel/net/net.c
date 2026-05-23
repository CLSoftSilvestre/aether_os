/*
 * AetherOS — Network subsystem glue (Phase 5.1)
 * File: kernel/net/net.c
 *
 * Holds the global network state and bridges the VirtIO driver
 * to the Ethernet receive path.
 *
 * boot sequence (after SP storage reorder):
 *   fat32_mount → vfs_init → net_init → reads /config/network.conf
 *   → static (applies immediately) or DHCP fallback
 */

#include "aether/net.h"
#include "aether/ethernet.h"
#include "aether/dhcp.h"
#include "aether/config.h"
#include "aether/fat32.h"
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

/* ── Bridge functions ────────────────────────────────────────────────────── */

void net_deliver_frame(const u8 *buf, u16 len)
{
    eth_rx(buf, len);
}

int net_tx_raw(const u8 *buf, u16 len)
{
    return virtio_net_tx(buf, len);
}

void net_rx_poll(void)
{
    virtio_net_rx_poll();
}

/* ── net_ip_parse ────────────────────────────────────────────────────────── */

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

/* ── net_conf_get ────────────────────────────────────────────────────────── */

void net_conf_get(net_conf_t *out)
{
    out->ip      = g_our_ip;
    out->mask    = g_subnet_mask;
    out->gateway = g_gateway_ip;
    out->dns     = g_dns_ip;
    out->ready   = (u8)g_net_ready;
    for (int i = 0; i < 6; i++) out->mac[i] = g_our_mac[i];

    /* Determine mode by checking the saved config */
    char mode[8];
    out->mode = 0;   /* default: DHCP */
    if (kconfig_read("/config/network.conf", "mode", mode, 8) == 0) {
        if (mode[0] == 's') out->mode = 1;   /* "static" */
    }
}

/* ── net_conf_set ────────────────────────────────────────────────────────── */

static void net_ip_to_str(u32 ip, char *buf)
{
    /* Reuse net_ip_str from net.h inline */
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

int net_conf_set(const net_conf_t *cfg)
{
    char ipbuf[16];

    if (cfg->mode == 1) {
        /* Static: apply immediately and persist */
        g_our_ip      = cfg->ip;
        g_subnet_mask = cfg->mask;
        g_gateway_ip  = cfg->gateway;
        g_dns_ip      = cfg->dns;
        g_net_ready   = 1;
        kinfo("NET: static IP set via System Preferences\n");

        kconfig_write("/config/network.conf", "mode", "static");
        net_ip_to_str(cfg->ip,      ipbuf); kconfig_write("/config/network.conf", "ip",      ipbuf);
        net_ip_to_str(cfg->mask,    ipbuf); kconfig_write("/config/network.conf", "mask",    ipbuf);
        net_ip_to_str(cfg->gateway, ipbuf); kconfig_write("/config/network.conf", "gateway", ipbuf);
        net_ip_to_str(cfg->dns,     ipbuf); kconfig_write("/config/network.conf", "dns",     ipbuf);
    } else {
        /* DHCP: persist only; requires reboot to re-run DHCP discovery */
        kconfig_write("/config/network.conf", "mode", "dhcp");
        kinfo("NET: DHCP mode saved — reboot to apply\n");
    }
    return 0;
}

/* ── net_init ────────────────────────────────────────────────────────────── */

void net_init(void)
{
    kinfo("NET: initializing VirtIO net...\n");

    if (!virtio_net_init()) {
        kwarn("NET: no network device — skipping\n");
        return;
    }

    /* Read /config/network.conf (FAT32 is mounted before net_init now) */
    char mode[8];
    if (kconfig_read("/config/network.conf", "mode", mode, 8) == 0 &&
        mode[0] == 's') {
        /* Static configuration */
        char ibuf[16];
        u32 ip = 0, mask = 0, gw = 0, dns = 0;
        if (kconfig_read("/config/network.conf", "ip",      ibuf, 16) == 0) ip   = net_ip_parse(ibuf);
        if (kconfig_read("/config/network.conf", "mask",    ibuf, 16) == 0) mask = net_ip_parse(ibuf);
        if (kconfig_read("/config/network.conf", "gateway", ibuf, 16) == 0) gw   = net_ip_parse(ibuf);
        if (kconfig_read("/config/network.conf", "dns",     ibuf, 16) == 0) dns  = net_ip_parse(ibuf);

        if (ip != 0) {
            g_our_ip      = ip;
            g_subnet_mask = mask ? mask : 0xFFFFFF00u;
            g_gateway_ip  = gw;
            g_dns_ip      = dns ? dns : 0x08080808u;
            g_net_ready   = 1;
            kinfo("NET: static config from /config/network.conf\n");
            return;
        }
        kwarn("NET: static mode but IP is 0.0.0.0 — falling back to DHCP\n");
    }

    /* DHCP */
    kinfo("NET: running DHCP...\n");
    if (!dhcp_init()) {
        kwarn("NET: DHCP failed — no IP address\n");
        return;
    }

    arp_announce();
    kinfo("NET: ready\n");
}
