/*
 * libaether_posix/socket_posix.c — POSIX socket API over AetherOS Phase 5.1
 *
 * Maps:
 *   socket(AF_INET, SOCK_STREAM/DGRAM, 0) → sys_socket(SOCK_TCP/SOCK_UDP)
 *   connect(fd, sockaddr_in, ...)         → sys_connect(fd, ip, port)
 *   send/recv                             → sys_net_send/sys_net_recv
 *   close(fd ≥ 100)                       → sys_net_close
 *   gethostbyname                         → sys_net_dns
 *
 * Unsupported operations (bind, listen, accept) return ENOSYS — AetherOS
 * Phase 5.1 is client-side only.  Server-side support comes in a later phase.
 *
 * All socket fds returned by sys_socket are ≥ 100 (kernel convention).
 */

#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys.h>   /* AetherOS syscalls */

/* ── socket() ────────────────────────────────────────────────────────── */

int socket(int domain, int type, int protocol)
{
    (void)protocol;
    if (domain != AF_INET && domain != AF_UNSPEC) { errno = EAFNOSUPPORT; return -1; }
    int atype;
    if (type == SOCK_STREAM)      atype = SOCK_TCP; /* matches AetherOS 0 */
    else if (type == SOCK_DGRAM)  atype = SOCK_UDP; /* matches AetherOS 1 */
    else { errno = ESOCKTNOSUPPORT; return -1; }
    long fd = sys_socket(atype);
    if (fd < 0) { errno = ENOMEM; return -1; }
    return (int)fd;
}

/* ── connect() ───────────────────────────────────────────────────────── */

int connect(int fd, const struct sockaddr *addr, socklen_t addrlen)
{
    (void)addrlen;
    if (!addr) { errno = EFAULT; return -1; }
    if (addr->sa_family != AF_INET) { errno = EAFNOSUPPORT; return -1; }

    const struct sockaddr_in *sin = (const struct sockaddr_in *)addr;
    /* sin->sin_addr.s_addr is in network byte order; sys_connect expects host order */
    unsigned int ip   = ntohl(sin->sin_addr.s_addr);
    unsigned short port = ntohs(sin->sin_port);

    long r = sys_connect((long)fd, ip, port);
    if (r < 0) { errno = ECONNREFUSED; return -1; }
    return 0;
}

/* ── send / recv ─────────────────────────────────────────────────────── */

ssize_t send(int fd, const void *buf, size_t len, int flags)
{
    (void)flags;
    long n = sys_net_send((long)fd, buf, (long)len);
    if (n < 0) { errno = EPIPE; return -1; }
    return (ssize_t)n;
}

ssize_t recv(int fd, void *buf, size_t len, int flags)
{
    (void)flags;
    long n = sys_net_recv((long)fd, buf, (long)len);
    if (n < 0) { errno = ETIMEDOUT; return -1; }
    return (ssize_t)n;
}

ssize_t sendto(int fd, const void *buf, size_t len, int flags,
               const struct sockaddr *dest, socklen_t addrlen)
{
    (void)dest; (void)addrlen;
    return send(fd, buf, len, flags);
}

ssize_t recvfrom(int fd, void *buf, size_t len, int flags,
                 struct sockaddr *src, socklen_t *addrlen)
{
    (void)src; (void)addrlen;
    return recv(fd, buf, len, flags);
}

/* ── bind / listen / accept (unsupported) ────────────────────────────── */

int bind(int fd, const struct sockaddr *addr, socklen_t addrlen)
{
    (void)fd; (void)addr; (void)addrlen;
    errno = ENOSYS; return -1;
}

int listen(int fd, int backlog)
{
    (void)fd; (void)backlog;
    errno = ENOSYS; return -1;
}

int accept(int fd, struct sockaddr *addr, socklen_t *addrlen)
{
    (void)fd; (void)addr; (void)addrlen;
    errno = ENOSYS; return -1;
}

/* ── shutdown ────────────────────────────────────────────────────────── */

int shutdown(int fd, int how)
{
    (void)how;
    sys_net_close((long)fd);
    return 0;
}

/* ── setsockopt / getsockopt (silently accept) ───────────────────────── */

int setsockopt(int fd, int level, int optname,
               const void *optval, socklen_t optlen)
{
    (void)fd; (void)level; (void)optname; (void)optval; (void)optlen;
    return 0;
}

int getsockopt(int fd, int level, int optname,
               void *optval, socklen_t *optlen)
{
    (void)fd; (void)level;
    if (optname == SO_ERROR && optval && optlen && *optlen >= sizeof(int)) {
        *(int *)optval = 0;
    }
    return 0;
}

int getsockname(int fd, struct sockaddr *addr, socklen_t *addrlen)
{
    (void)fd; (void)addr; (void)addrlen;
    errno = ENOSYS; return -1;
}

int getpeername(int fd, struct sockaddr *addr, socklen_t *addrlen)
{
    (void)fd; (void)addr; (void)addrlen;
    errno = ENOSYS; return -1;
}

/* ── select() ────────────────────────────────────────────────────────── */

int select(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds,
           struct timeval *timeout)
{
    /* Stub: if any socket fd is set in readfds, claim it's readable.
     * Real implementation would require a kernel poll syscall.
     * For NetSurf's single-connection model this is sufficient. */
    (void)timeout;
    int ready = 0;
    for (int fd = 0; fd < nfds; fd++) {
        if (readfds  && FD_ISSET(fd, readfds))  ready++;
        if (writefds && FD_ISSET(fd, writefds)) ready++;
        (void)exceptfds;
    }
    return ready > 0 ? ready : 0;
}

/* ── inet_addr / inet_ntoa / inet_aton ───────────────────────────────── */

in_addr_t inet_addr(const char *s)
{
    if (!s) return (in_addr_t)-1;
    unsigned int a[4]; int i = 0;
    while (i < 4) {
        a[i] = 0;
        while (*s >= '0' && *s <= '9') { a[i] = a[i]*10 + (*s++ - '0'); }
        i++;
        if (*s == '.') s++;
        else break;
    }
    if (i != 4) return (in_addr_t)-1;
    return htonl((a[0]<<24)|(a[1]<<16)|(a[2]<<8)|a[3]);
}

int inet_aton(const char *s, struct in_addr *out)
{
    in_addr_t r = inet_addr(s);
    if (r == (in_addr_t)-1) return 0;
    if (out) out->s_addr = r;
    return 1;
}

static char _ntoa_buf[20];
char *inet_ntoa(struct in_addr in)
{
    unsigned int v = ntohl(in.s_addr);
    snprintf(_ntoa_buf, sizeof(_ntoa_buf), "%u.%u.%u.%u",
             (v>>24)&0xff, (v>>16)&0xff, (v>>8)&0xff, v&0xff);
    return _ntoa_buf;
}

const char *inet_ntop(int af, const void *src, char *dst, socklen_t size)
{
    if (af != AF_INET) { errno = EAFNOSUPPORT; return NULL; }
    const struct in_addr *a = src;
    unsigned int v = ntohl(a->s_addr);
    snprintf(dst, (size_t)size, "%u.%u.%u.%u",
             (v>>24)&0xff, (v>>16)&0xff, (v>>8)&0xff, v&0xff);
    return dst;
}

int inet_pton(int af, const char *src, void *dst)
{
    if (af != AF_INET) { errno = EAFNOSUPPORT; return -1; }
    struct in_addr *a = dst;
    return inet_aton(src, a);
}

/* ── gethostbyname() ─────────────────────────────────────────────────── */

static struct hostent _he;
static char  *_he_aliases[1];
static char  *_he_addr_list[2];
static struct in_addr _he_addr;
static char   _he_name[256];

struct hostent *gethostbyname(const char *name)
{
    if (!name) return NULL;

    /* First try: parse as dotted-decimal */
    in_addr_t ip = inet_addr(name);
    if (ip != (in_addr_t)-1) {
        _he_addr.s_addr = ip;
    } else {
        /* DNS lookup via AetherOS Phase 5.1 */
        unsigned int host_ip = sys_net_dns(name);
        if (!host_ip) return NULL;
        /* sys_net_dns returns host-byte-order; convert to network order */
        _he_addr.s_addr = htonl(host_ip);
    }

    size_t nlen = strlen(name);
    if (nlen >= sizeof(_he_name)) nlen = sizeof(_he_name) - 1;
    memcpy(_he_name, name, nlen);
    _he_name[nlen] = '\0';

    _he_aliases[0]   = NULL;
    _he_addr_list[0] = (char *)&_he_addr;
    _he_addr_list[1] = NULL;

    _he.h_name      = _he_name;
    _he.h_aliases   = _he_aliases;
    _he.h_addrtype  = AF_INET;
    _he.h_length    = 4;
    _he.h_addr_list = _he_addr_list;

    return &_he;
}

/* ── getaddrinfo / freeaddrinfo ──────────────────────────────────────── */

static struct addrinfo _ai;
static struct sockaddr_in _ai_addr;

int getaddrinfo(const char *node, const char *service,
                const struct addrinfo *hints, struct addrinfo **res)
{
    (void)hints;
    if (!res) return EAI_FAIL;

    struct hostent *he = gethostbyname(node ? node : "localhost");
    if (!he) return EAI_NONAME;

    unsigned short port = 80;
    if (service) {
        char *end;
        unsigned long p = strtoul(service, &end, 10);
        if (end != service) port = (unsigned short)p;
    }

    memset(&_ai_addr, 0, sizeof(_ai_addr));
    _ai_addr.sin_family = AF_INET;
    _ai_addr.sin_port   = htons(port);
    memcpy(&_ai_addr.sin_addr, he->h_addr_list[0], 4);

    memset(&_ai, 0, sizeof(_ai));
    _ai.ai_family    = AF_INET;
    _ai.ai_socktype  = SOCK_STREAM;
    _ai.ai_protocol  = IPPROTO_TCP;
    _ai.ai_addrlen   = sizeof(_ai_addr);
    _ai.ai_addr      = (struct sockaddr *)&_ai_addr;
    _ai.ai_next      = NULL;

    *res = &_ai;
    return 0;
}

void freeaddrinfo(struct addrinfo *res) { (void)res; }

const char *gai_strerror(int errcode)
{
    switch (errcode) {
    case EAI_AGAIN:  return "Temporary failure in name resolution";
    case EAI_FAIL:   return "Non-recoverable failure in name resolution";
    case EAI_NONAME: return "Name or service not known";
    default:         return "Unknown getaddrinfo error";
    }
}
