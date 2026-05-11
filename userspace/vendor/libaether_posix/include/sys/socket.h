#ifndef _POSIX_SYS_SOCKET_H
#define _POSIX_SYS_SOCKET_H

#include <sys/types.h>

/* Address families */
#define AF_UNSPEC  0
#define AF_INET    2
#define AF_INET6   10

/* Socket types */
#define SOCK_STREAM 1
#define SOCK_DGRAM  2
#define SOCK_RAW    3

/* Protocol */
#define IPPROTO_TCP 6
#define IPPROTO_UDP 17
#define IPPROTO_IP  0

/* Socket options */
#define SOL_SOCKET      1
#define SO_REUSEADDR    2
#define SO_KEEPALIVE    9
#define SO_RCVTIMEO     20
#define SO_SNDTIMEO     21
#define SO_ERROR        4
#define TCP_NODELAY     1
#define IPPROTO_TCP     6

#define MSG_PEEK        2
#define MSG_WAITALL     256

struct sockaddr {
    sa_family_t sa_family;
    char        sa_data[14];
};

/* socket() — creates a POSIX socket, mapped to AetherOS sys_socket */
int socket(int domain, int type, int protocol);

/* connect() — maps to sys_connect */
int connect(int fd, const struct sockaddr *addr, socklen_t addrlen);

/* send / recv */
ssize_t send(int fd, const void *buf, size_t len, int flags);
ssize_t recv(int fd, void *buf, size_t len, int flags);
ssize_t sendto(int fd, const void *buf, size_t len, int flags,
               const struct sockaddr *dest, socklen_t addrlen);
ssize_t recvfrom(int fd, void *buf, size_t len, int flags,
                 struct sockaddr *src, socklen_t *addrlen);

/* bind / listen / accept */
int bind(int fd, const struct sockaddr *addr, socklen_t addrlen);
int listen(int fd, int backlog);
int accept(int fd, struct sockaddr *addr, socklen_t *addrlen);

/* Shutdown */
int shutdown(int fd, int how);
#define SHUT_RD   0
#define SHUT_WR   1
#define SHUT_RDWR 2

/* Options */
int setsockopt(int fd, int level, int optname,
               const void *optval, socklen_t optlen);
int getsockopt(int fd, int level, int optname,
               void *optval, socklen_t *optlen);
int getsockname(int fd, struct sockaddr *addr, socklen_t *addrlen);
int getpeername(int fd, struct sockaddr *addr, socklen_t *addrlen);

#endif /* _POSIX_SYS_SOCKET_H */
