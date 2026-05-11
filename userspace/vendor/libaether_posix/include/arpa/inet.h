#ifndef _POSIX_ARPA_INET_H
#define _POSIX_ARPA_INET_H

#include <netinet/in.h>

/* AArch64 (Pi 4/5) is little-endian; network byte order is big-endian */
static inline uint16_t htons(uint16_t v)
{
    return (uint16_t)((v >> 8) | (v << 8));
}

static inline uint16_t ntohs(uint16_t v)
{
    return htons(v);
}

static inline uint32_t htonl(uint32_t v)
{
    return ((v & 0xff000000u) >> 24) |
           ((v & 0x00ff0000u) >>  8) |
           ((v & 0x0000ff00u) <<  8) |
           ((v & 0x000000ffu) << 24);
}

static inline uint32_t ntohl(uint32_t v)
{
    return htonl(v);
}

in_addr_t   inet_addr(const char *s);
char       *inet_ntoa(struct in_addr in);
int         inet_aton(const char *s, struct in_addr *out);
const char *inet_ntop(int af, const void *src, char *dst, socklen_t size);
int         inet_pton(int af, const char *src, void *dst);

#endif /* _POSIX_ARPA_INET_H */
