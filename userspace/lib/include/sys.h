#ifndef AETHER_USERSPACE_SYS_H
#define AETHER_USERSPACE_SYS_H

/*
 * AetherOS — Userspace Syscall Interface
 *
 * Linux AArch64 ABI:
 *   x8  = syscall number
 *   x0–x5 = arguments
 *   x0  = return value (after svc #0)
 *
 * Inline wrappers keep libc out of the picture entirely.
 */

/* Syscall numbers */
#define SYS_EXIT         0
#define SYS_SCHED_YIELD  3
#define SYS_READ         63
#define SYS_WRITE        34
#define SYS_INITRD_LS   500
#define SYS_INITRD_READ 501
#define SYS_PMM_STATS   502
#define SYS_FB_FILL     601
#define SYS_FB_CHAR     602
#define SYS_GET_TICKS   603
#define SYS_FB_CLAIM    604

/* Standard file descriptors */
#define STDIN_FILENO   0
#define STDOUT_FILENO  1
#define STDERR_FILENO  2

/* ── Low-level syscall stubs ─────────────────────────────────────── */

static inline long _sys1(long nr, long a0)
{
    register long x0 asm("x0") = a0;
    register long x8 asm("x8") = nr;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8) : "memory", "cc");
    return x0;
}

static inline long _sys0(long nr)
{
    register long x0 asm("x0") = 0;
    register long x8 asm("x8") = nr;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8) : "memory", "cc");
    return x0;
}

static inline long _sys3(long nr, long a0, long a1, long a2)
{
    register long x0 asm("x0") = a0;
    register long x1 asm("x1") = a1;
    register long x2 asm("x2") = a2;
    register long x8 asm("x8") = nr;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8)
                     : "memory", "cc");
    return x0;
}

/* ── Typed wrappers ──────────────────────────────────────────────── */

__attribute__((noreturn))
static inline void sys_exit(int code)
{
    _sys1(SYS_EXIT, (long)code);
    __builtin_unreachable();
}

static inline long sys_read(int fd, char *buf, long len)
{
    return _sys3(SYS_READ, (long)fd, (long)(void *)buf, len);
}

static inline long sys_write(int fd, const char *buf, long len)
{
    return _sys3(SYS_WRITE, (long)fd, (long)(const void *)buf, len);
}

static inline long sys_sched_yield(void)
{
    return _sys1(SYS_SCHED_YIELD, 0);
}

static inline long sys_initrd_ls(char *buf, long len)
{
    return _sys3(SYS_INITRD_LS, (long)(void *)buf, len, 0);
}

static inline long sys_initrd_read(const char *name, char *buf, long len)
{
    return _sys3(SYS_INITRD_READ, (long)(const void *)name, (long)(void *)buf, len);
}

static inline long sys_pmm_stats(void)
{
    return _sys0(SYS_PMM_STATS);
}

/* ── Graphics syscalls ───────────────────────────────────────────────── */

static inline void sys_fb_fill(unsigned x, unsigned y,
                                unsigned w, unsigned h, unsigned color)
{
    long xy = ((long)x << 32) | (long)y;
    long wh = ((long)w << 32) | (long)h;
    _sys3(SYS_FB_FILL, xy, wh, (long)color);
}

static inline void sys_fb_char(unsigned x, unsigned y,
                                unsigned char ch, unsigned fg, unsigned bg)
{
    long xy    = ((long)x  << 32) | (long)y;
    long ch_fg = ((long)ch << 32) | (long)fg;
    _sys3(SYS_FB_CHAR, xy, ch_fg, (long)bg);
}

static inline long sys_get_ticks(void)
{
    return _sys0(SYS_GET_TICKS);
}

static inline void sys_fb_claim(void)
{
    _sys0(SYS_FB_CLAIM);
}

/* ── String helpers (no libc) ────────────────────────────────────── */

static inline long sys_puts(const char *s)
{
    long len = 0;
    while (s[len]) len++;
    return sys_write(STDOUT_FILENO, s, len);
}

#endif /* AETHER_USERSPACE_SYS_H */
