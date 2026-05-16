/* stub sys/utsname.h for AetherOS */
#ifndef _SYS_UTSNAME_H
#define _SYS_UTSNAME_H

struct utsname {
    char sysname[65];
    char nodename[65];
    char release[65];
    char version[65];
    char machine[65];
};

static inline int uname(struct utsname *buf)
{
    if (!buf) return -1;
    __builtin_memcpy(buf->sysname,  "AetherOS", 9);
    __builtin_memcpy(buf->nodename, "aether",   7);
    __builtin_memcpy(buf->release,  "0.1",      4);
    __builtin_memcpy(buf->version,  "Phase7",   7);
    __builtin_memcpy(buf->machine,  "aarch64",  8);
    return 0;
}

#endif /* _SYS_UTSNAME_H */
