#ifndef _POSIX_NETDB_H
#define _POSIX_NETDB_H

#include <sys/socket.h>
#include <netinet/in.h>

/* Host entry returned by gethostbyname */
struct hostent {
    char  *h_name;           /* official name of host */
    char **h_aliases;        /* alias list (NULL terminated) */
    int    h_addrtype;       /* host address type = AF_INET */
    int    h_length;         /* length of address = 4 */
    char **h_addr_list;      /* list of addresses (network byte order) */
};
#define h_addr h_addr_list[0]

/* Address info (for getaddrinfo) */
struct addrinfo {
    int             ai_flags;
    int             ai_family;
    int             ai_socktype;
    int             ai_protocol;
    socklen_t       ai_addrlen;
    struct sockaddr *ai_addr;
    char            *ai_canonname;
    struct addrinfo *ai_next;
};

#define AI_PASSIVE     1
#define AI_CANONNAME   2
#define AI_NUMERICHOST 4

#define EAI_AGAIN  -3
#define EAI_FAIL   -4
#define EAI_NONAME -8

struct hostent *gethostbyname(const char *name);
int             getaddrinfo(const char *node, const char *service,
                            const struct addrinfo *hints,
                            struct addrinfo **res);
void            freeaddrinfo(struct addrinfo *res);
const char     *gai_strerror(int errcode);

#endif /* _POSIX_NETDB_H */
