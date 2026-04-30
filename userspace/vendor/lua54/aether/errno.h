#ifndef _AETHER_LUA_ERRNO_H
#define _AETHER_LUA_ERRNO_H
/* errno, ENOMEM, EINVAL are defined in luaconf.h (always included first).
   Add remaining standard constants used by lauxlib.c and liolib.c. */
#ifndef EPERM
#define EPERM    1
#endif
#ifndef ENOENT
#define ENOENT   2
#endif
#ifndef EBADF
#define EBADF    9
#endif
#ifndef EACCES
#define EACCES  13
#endif
#ifndef EEXIST
#define EEXIST  17
#endif
#ifndef ENOSPC
#define ENOSPC  28
#endif
#ifndef ENOSYS
#define ENOSYS  38
#endif
#endif /* _AETHER_LUA_ERRNO_H */
