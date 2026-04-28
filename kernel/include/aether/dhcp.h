/*
 * AetherOS — DHCP client
 * File: kernel/include/aether/dhcp.h
 */
#ifndef AETHER_DHCP_H
#define AETHER_DHCP_H

/* Run a DHCP DISCOVER→OFFER→REQUEST→ACK cycle.
 * Populates g_our_ip, g_gateway_ip, g_subnet_mask, g_dns_ip.
 * Busy-polls the VirtIO RX queue (no IRQs needed).
 * Returns 1 on success, 0 on timeout (3 s). */
int dhcp_init(void);

#endif /* AETHER_DHCP_H */
