/*
 * AetherOS — Ethernet + ARP layer
 * File: kernel/include/aether/ethernet.h
 */
#ifndef AETHER_ETHERNET_H
#define AETHER_ETHERNET_H

#include "aether/types.h"

#define ETH_HLEN        14u
#define ETHERTYPE_IP    0x0800u
#define ETHERTYPE_ARP   0x0806u

extern const u8 eth_broadcast[6];   /* FF:FF:FF:FF:FF:FF */

/* Build + send an Ethernet frame.
 * dst_mac: destination MAC (6 bytes). ethertype in host byte order. */
void eth_tx(const u8 *dst_mac, u16 ethertype,
            const u8 *payload, u16 payload_len);

/* Dispatch an incoming Ethernet frame (called by net_deliver_frame). */
void eth_rx(const u8 *frame, u16 len);

/* ARP: resolve host-order IP → MAC.
 * Returns 1 if resolved (mac_out filled), 0 on timeout (~1s). */
int  arp_resolve(u32 ip, u8 *mac_out);

/* ARP: insert/update an entry (called when we see an ARP reply). */
void arp_update(u32 ip, const u8 *mac);

/* Send a gratuitous ARP to announce our IP/MAC. */
void arp_announce(void);

#endif /* AETHER_ETHERNET_H */
