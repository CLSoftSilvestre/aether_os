/*
 * AetherOS — IPv4 + ICMP layer
 * File: kernel/include/aether/ip.h
 */
#ifndef AETHER_IP_H
#define AETHER_IP_H

#include "aether/types.h"

#define IP_PROTO_ICMP  1u
#define IP_PROTO_TCP   6u
#define IP_PROTO_UDP   17u

/* Send an IPv4 packet.
 * dst_ip: host byte order. proto: IP_PROTO_*.
 * payload: data after IP header.  Returns 0 or -1. */
int  ip_tx(u32 dst_ip, u8 proto, const u8 *payload, u16 payload_len);

/* Dispatch an incoming IPv4 packet (called from eth_rx). */
void ip_rx(const u8 *ip_pkt, u16 len);

/* ICMP echo-request to dst_ip (host byte order).
 * Returns RTT in timer ticks, or (u32)-1 on timeout. */
u32  icmp_ping(u32 dst_ip);

/* IPv4 checksum (over a word array, len = byte count) */
u16  ip_checksum(const void *data, u16 len);

#endif /* AETHER_IP_H */
