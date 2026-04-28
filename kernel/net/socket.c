/*
 * AetherOS — Socket API (kernel side, Phase 5.1)
 * File: kernel/net/socket.c
 *
 * 8 socket slots, fd range 100..107.
 * TCP: wraps tcp_connect/tcp_send/tcp_recv/tcp_close.
 * UDP: wraps udp_send/udp_recv_blocking.
 */

#include "aether/socket.h"
#include "aether/tcp.h"
#include "aether/udp.h"
#include "aether/types.h"

#define SOCK_FD_BASE   100
#define SOCK_TABLE_SZ  8

typedef struct {
    int  in_use;
    int  type;         /* SOCK_TYPE_TCP or SOCK_TYPE_UDP */
    u16  local_port;   /* UDP: bound port */
    u32  remote_ip;    /* TCP only */
    u16  remote_port;  /* TCP only */
} sock_t;

static sock_t g_socks[SOCK_TABLE_SZ];

static inline int fd_to_idx(int fd) {
    int idx = fd - SOCK_FD_BASE;
    return (idx >= 0 && idx < SOCK_TABLE_SZ) ? idx : -1;
}
static inline int idx_to_fd(int idx) { return idx + SOCK_FD_BASE; }

static inline sock_t *get_sock(int fd) {
    int idx = fd_to_idx(fd);
    if (idx < 0 || !g_socks[idx].in_use) return (sock_t*)0;
    return &g_socks[idx];
}

/* ── Public ─────────────────────────────────────────────────────────────── */

int sock_create(int type)
{
    for (int i = 0; i < SOCK_TABLE_SZ; i++) {
        if (!g_socks[i].in_use) {
            g_socks[i].in_use      = 1;
            g_socks[i].type        = type;
            g_socks[i].local_port  = 0;
            g_socks[i].remote_ip   = 0;
            g_socks[i].remote_port = 0;
            return idx_to_fd(i);
        }
    }
    return -1;
}

int sock_connect(int fd, u32 dst_ip, u16 dst_port)
{
    sock_t *s = get_sock(fd);
    if (!s || s->type != SOCK_TYPE_TCP) return -1;
    s->remote_ip   = dst_ip;
    s->remote_port = dst_port;
    return tcp_connect(dst_ip, dst_port);
}

int sock_send(int fd, const u8 *buf, u16 len)
{
    sock_t *s = get_sock(fd);
    if (!s) return -1;
    if (s->type == SOCK_TYPE_TCP)
        return tcp_send(0, buf, len);
    return -1;
}

int sock_recv(int fd, u8 *buf, u16 maxlen, u32 timeout_ms)
{
    sock_t *s = get_sock(fd);
    if (!s) return -1;
    if (s->type == SOCK_TYPE_TCP)
        return tcp_recv(0, buf, maxlen, timeout_ms);
    return -1;
}

int sock_close(int fd)
{
    sock_t *s = get_sock(fd);
    if (!s) return -1;
    if (s->type == SOCK_TYPE_TCP) tcp_close(0);
    s->in_use = 0;
    return 0;
}

int sock_bind(int fd, u16 port)
{
    sock_t *s = get_sock(fd);
    if (!s || s->type != SOCK_TYPE_UDP) return -1;
    s->local_port = port;
    return 0;
}

int sock_sendto(int fd, u32 dst_ip, u16 dst_port, const u8 *buf, u16 len)
{
    sock_t *s = get_sock(fd);
    if (!s || s->type != SOCK_TYPE_UDP) return -1;
    u16 src = s->local_port ? s->local_port : 49200u;
    return udp_send(dst_ip, dst_port, src, buf, len);
}

int sock_recvfrom(int fd, u8 *buf, u16 maxlen,
                  u32 *from_ip, u16 *from_port, u32 timeout_ms)
{
    sock_t *s = get_sock(fd);
    if (!s || s->type != SOCK_TYPE_UDP || !s->local_port) return -1;
    return udp_recv_blocking(s->local_port, buf, maxlen,
                              from_ip, from_port, timeout_ms);
}
