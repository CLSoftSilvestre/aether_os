#ifndef _POSIX_SYS_SELECT_H
#define _POSIX_SYS_SELECT_H

#include <sys/types.h>
#include <time.h>

#define FD_SETSIZE 64

typedef struct {
    unsigned long fds_bits[FD_SETSIZE / (8 * sizeof(unsigned long))];
} fd_set;

static inline void FD_ZERO(fd_set *s)
{
    for (int i = 0; i < (int)(FD_SETSIZE / (8 * sizeof(unsigned long))); i++)
        s->fds_bits[i] = 0;
}

static inline void FD_SET(int fd, fd_set *s)
{
    s->fds_bits[fd / (int)(8*sizeof(unsigned long))] |=
        (1UL << (fd % (int)(8*sizeof(unsigned long))));
}

static inline void FD_CLR(int fd, fd_set *s)
{
    s->fds_bits[fd / (int)(8*sizeof(unsigned long))] &=
        ~(1UL << (fd % (int)(8*sizeof(unsigned long))));
}

static inline int FD_ISSET(int fd, const fd_set *s)
{
    return !!(s->fds_bits[fd / (int)(8*sizeof(unsigned long))] &
              (1UL << (fd % (int)(8*sizeof(unsigned long)))));
}

/* select(): single-fd polling — always returns 1 if fd is a valid socket */
int select(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds,
           struct timeval *timeout);

#endif /* _POSIX_SYS_SELECT_H */
