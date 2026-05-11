#ifndef _POSIX_SYS_TYPES_H
#define _POSIX_SYS_TYPES_H

#include <stddef.h>
#include <stdint.h>

typedef long           ssize_t;
typedef long           off_t;
typedef int            pid_t;
typedef unsigned int   uid_t;
typedef unsigned int   gid_t;
typedef unsigned int   mode_t;
typedef unsigned int   nlink_t;
typedef unsigned int   dev_t;
typedef unsigned long  ino_t;
typedef unsigned int   socklen_t;
typedef uint16_t       sa_family_t;
typedef uint16_t       in_port_t;
typedef uint32_t       in_addr_t;

#endif /* _POSIX_SYS_TYPES_H */
