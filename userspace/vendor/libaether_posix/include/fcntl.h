#ifndef _POSIX_FCNTL_H
#define _POSIX_FCNTL_H

#include <sys/types.h>

/* File access modes */
#define O_RDONLY    0
#define O_WRONLY    1
#define O_RDWR      2
#define O_ACCMODE   3

/* File creation flags */
#define O_CREAT     0x40
#define O_EXCL      0x80
#define O_NOCTTY    0x100
#define O_TRUNC     0x200
#define O_APPEND    0x400
#define O_NONBLOCK  0x800

/* open(): create or open a file; returns fd or -1 */
int open(const char *path, int flags, ...);

#endif /* _POSIX_FCNTL_H */
