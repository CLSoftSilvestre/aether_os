/*
 * AetherOS — UDP layer
 * File: kernel/include/aether/udp.h
 */
#ifndef AETHER_UDP_H
#define AETHER_UDP_H

#include "aether/types.h"

/* Send a UDP datagram.
 * All ports and IPs in host byte order. */
int  udp_send(u32 dst_ip, u16 dst_port, u16 src_port,
              const u8 *data, u16 len);

/* Dispatch incoming UDP (called from ip_rx). */
void udp_rx(u32 src_ip, const u8 *udp_pkt, u16 len);

/* Register a handler for incoming packets on dst_port.
 * handler(src_ip, src_port, data, data_len).
 * src_port and dst_port are in host byte order. */
typedef void (*udp_handler_t)(u32 src_ip, u16 src_port,
                               const u8 *data, u16 data_len);
void udp_bind(u16 dst_port, udp_handler_t handler);
void udp_unbind(u16 dst_port);

/*
 * Blocking receive on a port (busy-polls net_rx_poll).
 * Fills buf, sets *from_ip and *from_port (host byte order).
 * Returns bytes received, or -1 on timeout (timeout_ms = 0 → 1000 ms).
 */
int udp_recv_blocking(u16 port, u8 *buf, u16 maxlen,
                      u32 *from_ip, u16 *from_port,
                      u32 timeout_ms);

#endif /* AETHER_UDP_H */
