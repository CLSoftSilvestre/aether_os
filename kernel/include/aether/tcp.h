/*
 * AetherOS — TCP client (single connection)
 * File: kernel/include/aether/tcp.h
 *
 * Single-connection TCP client state machine.
 * Phases: CLOSED → SYN_SENT → ESTABLISHED → FIN_WAIT → CLOSED.
 * No retransmission timers — suitable for local/LAN connections.
 */
#ifndef AETHER_TCP_H
#define AETHER_TCP_H

#include "aether/types.h"

/* Open a TCP connection to dst_ip:dst_port (host byte order).
 * Blocks until ESTABLISHED or timeout (~3 s).
 * Returns a non-negative handle on success, -1 on failure. */
int  tcp_connect(u32 dst_ip, u16 dst_port);

/* Send data on an open connection. Returns bytes sent or -1. */
int  tcp_send(int handle, const u8 *data, u16 len);

/* Receive data (blocks up to timeout_ms ms). Returns bytes or -1. */
int  tcp_recv(int handle, u8 *buf, u16 maxlen, u32 timeout_ms);

/* Close connection gracefully (FIN-ACK). */
void tcp_close(int handle);

/* Called from ip_rx() for incoming TCP segments. */
void tcp_rx(u32 src_ip, const u8 *tcp_seg, u16 len);

#endif /* AETHER_TCP_H */
