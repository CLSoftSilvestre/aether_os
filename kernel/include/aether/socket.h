/*
 * AetherOS — Socket API (kernel side)
 * File: kernel/include/aether/socket.h
 *
 * Kernel socket table (8 slots, fd range 100..107).
 * Wraps TCP (type 0) and UDP (type 1) connections.
 */
#ifndef AETHER_SOCKET_H
#define AETHER_SOCKET_H

#include "aether/types.h"

#define SOCK_TYPE_TCP  0
#define SOCK_TYPE_UDP  1

/* Create socket. Returns fd (>=100) or -1. */
int  sock_create(int type);

/* Connect TCP socket to dst_ip:dst_port (host byte order).
 * Returns 0 on success, -1 on failure. */
int  sock_connect(int fd, u32 dst_ip, u16 dst_port);

/* Send data on socket. Returns bytes sent or -1. */
int  sock_send(int fd, const u8 *buf, u16 len);

/* Receive data from socket (blocks up to timeout_ms). Returns bytes or -1. */
int  sock_recv(int fd, u8 *buf, u16 maxlen, u32 timeout_ms);

/* Close socket. Returns 0 or -1. */
int  sock_close(int fd);

/* Bind UDP socket to a local port (host byte order). Returns 0 or -1. */
int  sock_bind(int fd, u16 port);

/* Send UDP datagram to dst_ip:dst_port. Returns bytes or -1. */
int  sock_sendto(int fd, u32 dst_ip, u16 dst_port,
                 const u8 *buf, u16 len);

/* Receive UDP datagram. Fills from_ip and from_port. Returns bytes or -1. */
int  sock_recvfrom(int fd, u8 *buf, u16 maxlen,
                   u32 *from_ip, u16 *from_port, u32 timeout_ms);

#endif /* AETHER_SOCKET_H */
