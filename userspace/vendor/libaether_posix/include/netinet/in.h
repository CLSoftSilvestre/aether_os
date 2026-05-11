#ifndef _POSIX_NETINET_IN_H
#define _POSIX_NETINET_IN_H

#include <sys/socket.h>
#include <stdint.h>

struct in_addr {
    in_addr_t s_addr;
};

struct in6_addr {
    uint8_t s6_addr[16];
};

struct sockaddr_in {
    sa_family_t sin_family;   /* AF_INET */
    in_port_t   sin_port;     /* network byte order */
    struct in_addr sin_addr;
    unsigned char  sin_zero[8]; /* padding */
};

struct sockaddr_in6 {
    sa_family_t sin6_family;
    in_port_t   sin6_port;
    uint32_t    sin6_flowinfo;
    struct in6_addr sin6_addr;
    uint32_t    sin6_scope_id;
};

#define INADDR_ANY       ((in_addr_t)0x00000000)
#define INADDR_BROADCAST ((in_addr_t)0xffffffff)
#define INADDR_LOOPBACK  ((in_addr_t)0x7f000001)
#define INADDR_NONE      ((in_addr_t)0xffffffff)

#define IN6ADDR_ANY_INIT  { { { 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0 } } }

#define IPPROTO_IP   0
#define IPPROTO_TCP  6
#define IPPROTO_UDP  17

#define IP_TOS       1
#define IP_TTL       2

#endif /* _POSIX_NETINET_IN_H */
