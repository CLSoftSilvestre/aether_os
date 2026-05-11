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
#define SYS_SPAWN        1
#define SYS_KILL         2
#define SYS_SCHED_YIELD  3
#define SYS_WAITPID      4
#define SYS_GETPID       5
#define SYS_SLEEP_TICKS  6
#define SYS_KEY_READ     7
#define SYS_KEY_POLL     8
#define SYS_MOUSE_READ   9
#define SYS_MOUSE_POLL  10
#define SYS_WAITPID_NB  11
#define SYS_PS          21
#define SYS_PIPE        22
#define SYS_DUP2        24
#define SYS_CLOSE       30
#define SYS_READ         63
#define SYS_WRITE        34
#define SYS_INITRD_LS   500
#define SYS_INITRD_READ 501
#define SYS_PMM_STATS   502
#define SYS_FB_FILL     601
#define SYS_FB_CHAR     602
#define SYS_GET_TICKS   603
#define SYS_FB_CLAIM    604
#define SYS_CURSOR_MOVE 605
#define SYS_CURSOR_SHOW 606
#define SYS_FB_BLIT      608  /* (buf, dst_x<<32|dst_y, w<<32|h, stride) → 0 */
#define SYS_FB_CHAR_NOBG 609  /* ((x<<32)|y, (ch<<32)|fg) → 0              */
#define SYS_RTC_GET      610  /* () → seconds since epoch (u32)            */
#define SYS_VSYNC_WAIT   905  /* () → 0; blocks until next 60Hz boundary   */

/* Window Manager syscalls (Phase 4.6) */
#define SYS_WM_REGISTER   12
#define SYS_WM_KEY_RECV   13
#define SYS_WM_UNREGISTER 14
#define SYS_WM_FOCUS_SET  15
#define SYS_WM_FOCUS_GET  16
#define SYS_WM_MOVE       17
#define SYS_WM_GET_POS    18
#define SYS_WM_GET_SIZE   19
#define SYS_WM_GET_PID    20

/* Window Manager syscalls (Phase 4.7) */
#define SYS_WM_CLOSE       23
#define SYS_WM_EVENT_POLL  25

/* Window Manager syscalls (Phase 5.3) */
#define SYS_WM_PUSH_EVENT  28

/* Process argv syscall (Phase 5.5) */
#define SYS_SPAWN_ARGS     29

/* WM event types (match kernel wm.h) */
#define WM_EV_REDRAW         0xFEu
#define WM_EV_WINDOW_CLOSED  0xFFu

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

static inline long _sys2(long nr, long a0, long a1)
{
    register long x0 asm("x0") = a0;
    register long x1 asm("x1") = a1;
    register long x8 asm("x8") = nr;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x8)
                     : "memory", "cc");
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

static inline long _sys4(long nr, long a0, long a1, long a2, long a3)
{
    register long x0 asm("x0") = a0;
    register long x1 asm("x1") = a1;
    register long x2 asm("x2") = a2;
    register long x3 asm("x3") = a3;
    register long x8 asm("x8") = nr;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3), "r"(x8)
                     : "memory", "cc");
    return x0;
}

static inline long _sys5(long nr, long a0, long a1, long a2, long a3, long a4)
{
    register long x0 asm("x0") = a0;
    register long x1 asm("x1") = a1;
    register long x2 asm("x2") = a2;
    register long x3 asm("x3") = a3;
    register long x4 asm("x4") = a4;
    register long x8 asm("x8") = nr;
    __asm__ volatile("svc #0" : "+r"(x0)
                     : "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x8)
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

/* Sleep for N 100Hz ticks (~10ms per tick) */
static inline void sys_sleep(long ticks)
{
    _sys1(SYS_SLEEP_TICKS, ticks);
}

/* ── Process management (Phase 4.3) ─────────────────────────────────── */

/* Spawn a process by initrd path; returns child PID or -1 */
static inline long sys_spawn(const char *path)
{
    return _sys1(SYS_SPAWN, (long)(const void *)path);
}

/* Spawn with argv: kernel copies up to 16 strings onto the child's user stack */
static inline long sys_spawn_args(const char *path,
                                   const char *const *argv, long argc)
{
    return _sys3(SYS_SPAWN_ARGS, (long)(const void *)path,
                 (long)(const void *)argv, argc);
}

/* Wait for child PID; writes exit code to *status (may be NULL) */
static inline long sys_waitpid(long pid, int *status)
{
    return _sys2(SYS_WAITPID, pid, (long)(void *)status);
}

/* Non-blocking waitpid: returns child PID if done, 0 if still running, -1 if not found */
static inline long sys_waitpid_nb(long pid, int *status)
{
    return _sys2(SYS_WAITPID_NB, pid, (long)(void *)status);
}

/* Kill a process by PID; returns 0 or -1 */
static inline long sys_kill(long pid)
{
    return _sys1(SYS_KILL, pid);
}

/* Process snapshot entry (mirrors ps_entry_t in kernel scheduler.h) */
#define PROC_NAME_MAX 16
typedef struct {
    unsigned int       pid;
    unsigned int       ppid;
    int                state;
    char               name[PROC_NAME_MAX];
    unsigned int       mem_pages;  /* user_code_pages + user_stack_pages (×4 KB) */
    unsigned long long cpu_ticks;  /* cumulative scheduler invocations            */
} ps_entry_t;

/* Fill entries[] with live processes; returns count or -1 */
static inline long sys_ps(ps_entry_t *entries, int max)
{
    return _sys2(SYS_PS, (long)(void *)entries, (long)max);
}

static inline long sys_getpid(void)
{
    return _sys0(SYS_GETPID);
}

/* Create a pipe; fds[0]=read end, fds[1]=write end */
static inline long sys_pipe(int fds[2])
{
    return _sys1(SYS_PIPE, (long)(void *)fds);
}

/* Duplicate oldfd to newfd; returns newfd or -1 */
static inline long sys_dup2(int oldfd, int newfd)
{
    return _sys2(SYS_DUP2, (long)oldfd, (long)newfd);
}

/* Close a file descriptor (pipe end, socket, or vfs fd) */
static inline void sys_close(int fd)
{
    _sys1(SYS_CLOSE, (long)fd);
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

/* Returns current wall-clock time as seconds since epoch (via PL031 RTC). */
static inline unsigned long sys_rtc_get(void)
{
    return (unsigned long)_sys0(SYS_RTC_GET);
}

static inline void sys_fb_claim(void)
{
    _sys0(SYS_FB_CLAIM);
}

/* ── Input syscalls (Phase 4.5) ──────────────────────────────────────────── */

/* Block until a key event; returns packed key_event_t (use key_event_unpack) */
static inline unsigned long long sys_key_read(void)
{
    return (unsigned long long)_sys0(SYS_KEY_READ);
}

/* Non-blocking; returns 0 if no event pending */
static inline unsigned long long sys_key_poll(void)
{
    return (unsigned long long)_sys0(SYS_KEY_POLL);
}

/* Block until mouse event; returns packed mouse_event_t */
static inline unsigned long long sys_mouse_read(void)
{
    return (unsigned long long)_sys0(SYS_MOUSE_READ);
}

/* Non-blocking; returns 0 if no event pending */
static inline unsigned long long sys_mouse_poll(void)
{
    return (unsigned long long)_sys0(SYS_MOUSE_POLL);
}

/* Move and redraw the software cursor; kernel saves/restores background */
static inline void sys_cursor_move(unsigned int x, unsigned int y)
{
    long xy = ((long)x << 32) | (long)y;
    _sys1(SYS_CURSOR_MOVE, xy);
}

static inline void sys_cursor_show(int visible)
{
    _sys1(SYS_CURSOR_SHOW, (long)visible);
}

/* Blit a w×h region from a user XRGB8888 pixel buffer to the framebuffer.
 * src_stride_bytes = bytes per row in the source buffer (typically bmp_w*4). */
static inline void sys_fb_blit(const unsigned *pixels,
                                unsigned dst_x, unsigned dst_y,
                                unsigned w, unsigned h,
                                unsigned src_stride_bytes)
{
    long xy = ((long)dst_x << 32) | (long)dst_y;
    long wh = ((long)w     << 32) | (long)h;
    _sys4(SYS_FB_BLIT, (long)(const void *)pixels, xy, wh,
          (long)src_stride_bytes);
}

/* Draw a character without filling background pixels — foreground only.
 * Use over glass/translucent surfaces to avoid overwriting the glass bg. */
static inline void sys_fb_char_nobg(unsigned x, unsigned y,
                                    unsigned char ch, unsigned fg)
{
    long xy    = ((long)x  << 32) | (long)y;
    long ch_fg = ((long)ch << 32) | (long)fg;
    _sys2(SYS_FB_CHAR_NOBG, xy, ch_fg);
}

/* Block until the next 60Hz vsync boundary (~16.67ms cadence). */
static inline void sys_vsync_wait(void)
{
    _sys0(SYS_VSYNC_WAIT);
}

/* ── Window Manager syscall wrappers (Phase 4.6) ───────────────────────── */

/*
 * Register a window for this process; returns win_id (0-15) or -1.
 * Geometry: (x, y) = top-left corner, (w, h) = size.
 */
static inline long sys_wm_register(int x, int y, int w, int h,
                                    const char *title)
{
    long xy = ((long)x << 32) | (long)(unsigned int)y;
    long wh = ((long)w << 32) | (long)(unsigned int)h;
    return _sys3(SYS_WM_REGISTER, xy, wh, (long)(const void *)title);
}

/* Block until a key or WM event is ready for this process (focus-routed) */
static inline unsigned long long sys_wm_key_recv(void)
{
    return (unsigned long long)_sys0(SYS_WM_KEY_RECV);
}

/* Unregister a window (call on process exit) */
static inline void sys_wm_unregister(long win_id)
{
    _sys1(SYS_WM_UNREGISTER, win_id);
}

/* Set the focused PID (called by init on click) */
static inline void sys_wm_focus_set(long pid)
{
    _sys1(SYS_WM_FOCUS_SET, pid);
}

/* Query the currently focused PID */
static inline long sys_wm_focus_get(void)
{
    return _sys0(SYS_WM_FOCUS_GET);
}

/* Move a window and notify its owner (init calls this at drag end) */
static inline void sys_wm_move(long win_id, int x, int y)
{
    long xy = ((long)x << 32) | (long)(unsigned int)y;
    _sys2(SYS_WM_MOVE, win_id, xy);
}

/* Query window position: returns x<<32|y, or -1 if win_id invalid */
static inline long sys_wm_get_pos(long win_id)
{
    return _sys1(SYS_WM_GET_POS, win_id);
}

/* Query window size: returns w<<32|h, or 0 if win_id invalid */
static inline long sys_wm_get_size(long win_id)
{
    return _sys1(SYS_WM_GET_SIZE, win_id);
}

/* Query window owner PID, or 0 if invalid */
static inline long sys_wm_get_pid(long win_id)
{
    return _sys1(SYS_WM_GET_PID, win_id);
}

/* Hard-kill a window's owner process (PID-1-only); returns 0 or -1 */
static inline long sys_wm_close(long win_id)
{
    return _sys1(SYS_WM_CLOSE, win_id);
}

/* Non-blocking dequeue from this process's WM event ring; returns 0 if empty */
static inline unsigned long long sys_wm_event_poll(void)
{
    return (unsigned long long)_sys0(SYS_WM_EVENT_POLL);
}

/* Inject a packed WM event into any process's event ring (Phase 5.3).
 * Called by init to forward mouse events to the window under the cursor. */
static inline void sys_wm_push_event(long pid, unsigned long long event)
{
    _sys2(SYS_WM_PUSH_EVENT, pid, (long)event);
}

/* ── WM event helpers (Phase 4.7) ──────────────────────────────────────── */

/* Check whether a raw WM event is a WM_EV_WINDOW_CLOSED notification */
static inline int wm_is_window_closed(unsigned long long ev)
{
    return (int)((ev >> 56) == WM_EV_WINDOW_CLOSED);
}

/* Decode WM_EV_WINDOW_CLOSED geometry — call only when wm_is_window_closed() */
static inline void wm_decode_closed(unsigned long long ev,
                                     int *x, int *y, int *w, int *h)
{
    *x = (int)((ev >> 44) & 0xFFFu);
    *y = (int)((ev >> 32) & 0xFFFu);
    *w = (int)((ev >> 16) & 0xFFFFu);
    *h = (int)(ev & 0xFFFFu);
}

/* ── Networking syscalls (Phase 5.1) ─────────────────────────────────── */

#define SYS_NET_STATUS  700
#define SYS_NET_PING    701
#define SYS_NET_DNS     702
#define SYS_SOCKET      703
#define SYS_CONNECT     704
#define SYS_NET_SEND    705
#define SYS_NET_RECV    706
#define SYS_NET_CLOSE   707

#define SOCK_TCP  0
#define SOCK_UDP  1

typedef struct {
    unsigned int ip, mask, gateway, dns;
    unsigned char mac[6];
    unsigned char _pad[2];
} net_status_t;

/* Fill *st with current IP/MAC/gateway/DNS; returns 0 or -1 */
static inline long sys_net_status(net_status_t *st)
{
    return _sys1(SYS_NET_STATUS, (long)(void *)st);
}

/* ICMP echo to ip (host byte order); returns RTT ms or -1 on timeout */
static inline long sys_net_ping(unsigned int ip)
{
    return _sys1(SYS_NET_PING, (long)(unsigned long)ip);
}

/* DNS A-record lookup; returns host-order IP or 0 on failure */
static inline unsigned int sys_net_dns(const char *hostname)
{
    return (unsigned int)_sys1(SYS_NET_DNS, (long)(const void *)hostname);
}

/* Create a socket; type = SOCK_TCP or SOCK_UDP; returns fd >= 100 or -1 */
static inline long sys_socket(int type)
{
    return _sys1(SYS_SOCKET, (long)type);
}

/* Connect TCP socket to ip:port (both host byte order); returns 0 or -1 */
static inline long sys_connect(long fd, unsigned int ip, unsigned short port)
{
    return _sys3(SYS_CONNECT, fd, (long)(unsigned long)ip, (long)port);
}

/* Send data on socket; returns bytes sent or -1 */
static inline long sys_net_send(long fd, const void *buf, long len)
{
    return _sys3(SYS_NET_SEND, fd, (long)buf, len);
}

/* Receive data from socket (5 s timeout); returns bytes or -1 */
static inline long sys_net_recv(long fd, void *buf, long len)
{
    return _sys3(SYS_NET_RECV, fd, (long)buf, len);
}

/* Close socket */
static inline long sys_net_close(long fd)
{
    return _sys1(SYS_NET_CLOSE, fd);
}

/* ── Clipboard syscalls (Phase 5.3) ──────────────────────────────── */

#define SYS_CLIPBOARD_WRITE 26
#define SYS_CLIPBOARD_READ  27

/* Write up to 4KB of text into the kernel clipboard; returns 0 or -1 */
static inline long sys_clipboard_write(const char *buf, long len)
{
    return _sys2(SYS_CLIPBOARD_WRITE, (long)(const void *)buf, len);
}

/* Read clipboard into buf (max_len includes NUL); returns bytes copied or -1 */
static inline long sys_clipboard_read(char *buf, long max_len)
{
    return _sys2(SYS_CLIPBOARD_READ, (long)(void *)buf, max_len);
}

/* ── Filesystem syscalls (Phase 5.2) ─────────────────────────────── */

#define SYS_FS_OPEN      800
#define SYS_FS_READ      801
#define SYS_FS_CLOSE     802
#define SYS_FS_READDIR   803

#define VFS_FD_BASE      200   /* VFS fds are in range 200-215 */

/* Open a file by path; returns vfd (>=200) or -1 */
static inline long sys_fs_open(const char *path)
{
    return _sys1(SYS_FS_OPEN, (long)(const void *)path);
}

/* Read up to len bytes from vfd into buf; returns bytes read, 0=EOF, -1=error */
static inline long sys_fs_read(long vfd, void *buf, long len)
{
    return _sys3(SYS_FS_READ, vfd, (long)buf, len);
}

/* Close a VFS file descriptor */
static inline void sys_fs_close(long vfd)
{
    _sys1(SYS_FS_CLOSE, vfd);
}

/* List a directory path into buf; returns bytes written or -1 */
static inline long sys_fs_readdir(const char *path, char *buf, long len)
{
    return _sys3(SYS_FS_READDIR, (long)(const void *)path, (long)buf, len);
}

/* ── GPU / V3D syscalls (Phase 6.1) ──────────────────────────────── */

#define SYS_GPU_ALLOC   900
#define SYS_GPU_FREE    901
#define SYS_GPU_MAP     902
#define SYS_GPU_INFO    903
#define SYS_GPU_BLUR    904
/* Animation compositing (Phase 6.1.7 / 6.2) */
#define SYS_FB_CAPTURE      910  /* (bo, src_x<<32|src_y, w<<32|h) → 0/-1        */
#define SYS_GPU_BLIT        911  /* (bo, sw<<32|sh, dx<<32|dy, dw<<32|dh, alpha) */
#define SYS_COMPOSITE_ANIM  912  /* (bo, nat_x<<32|nat_y, nat_w<<32|nat_h,       *
                                  *    anim_x<<32|anim_y,                         *
                                  *    anim_w<<32|(anim_h<<16)|alpha) → 0/-1      */

/* Wallpaper sharing (Phase 6.1.x) */
#define SYS_WP_REGISTER   906  /* (buf_ptr, (w<<32)|h) → 0 */
#define SYS_WP_GET        907  /* () → raw pointer or 0    */
#define SYS_WP_SIZE       908  /* () → (bmp_w<<32)|bmp_h   */
#define SYS_WP_BLEND_FILL 909  /* (x<<32|y, w<<32|h, color) → 0 */

static inline long sys_wp_register(const unsigned *buf, unsigned w, unsigned h)
{
    long wh = ((long)w << 32) | (long)h;
    return _sys2(SYS_WP_REGISTER, (long)(const void *)buf, wh);
}

static inline long sys_wp_get(void)
{
    return _sys0(SYS_WP_GET);
}

static inline long sys_wp_size(void)
{
    return _sys0(SYS_WP_SIZE);
}

static inline long sys_wp_blend_fill(unsigned x, unsigned y,
                                      unsigned w, unsigned h,
                                      unsigned color)
{
    long xy = ((long)x << 32) | (long)y;
    long wh = ((long)w << 32) | (long)h;
    return _sys3(SYS_WP_BLEND_FILL, xy, wh, (long)color);
}

/* Copy a live framebuffer region into a GPU BO (animation snapshot).
 * Returns 0 on success, -1 on error. */
static inline long sys_fb_capture(int bo, unsigned src_x, unsigned src_y,
                                   unsigned w, unsigned h)
{
    long xy = ((long)src_x << 32) | (long)src_y;
    long wh = ((long)w     << 32) | (long)h;
    return _sys3(SYS_FB_CAPTURE, (long)bo, xy, wh);
}

/* Bilinear-scale a GPU BO and alpha-blend it onto the framebuffer.
 * src_w/h: natural dimensions of the BO content.
 * dst_x/y: framebuffer destination top-left.
 * dst_w/h: destination size (may differ from src for scaling effect).
 * alpha:   0 = transparent (no-op), 255 = opaque.
 * Returns 0 on success, -1 on error. */
static inline long sys_gpu_blit(int bo,
                                 unsigned src_w, unsigned src_h,
                                 unsigned dst_x, unsigned dst_y,
                                 unsigned dst_w, unsigned dst_h,
                                 unsigned char alpha)
{
    long src_wh = ((long)src_w << 32) | (long)src_h;
    long dst_xy = ((long)dst_x << 32) | (long)dst_y;
    long dst_wh = ((long)dst_w << 32) | (long)dst_h;
    return _sys5(SYS_GPU_BLIT, (long)bo, src_wh, dst_xy, dst_wh, (long)alpha);
}

/* Single-pass wallpaper-restore + BO composite for window animations.
 * Writes each FB pixel exactly once: no intermediate blank-window frame.
 *
 * nat_*:  the natural (unscaled) window rect — defines the area to clear.
 * anim_*: the current scaled rect centred inside the natural rect.
 * alpha:  animation opacity (0=transparent, 255=opaque).
 * The BO must have been captured at nat_w × nat_h resolution.
 */
static inline long sys_composite_anim(int bo,
                                       unsigned nat_x, unsigned nat_y,
                                       unsigned nat_w, unsigned nat_h,
                                       unsigned anim_x, unsigned anim_y,
                                       unsigned anim_w, unsigned anim_h,
                                       unsigned char alpha)
{
    long nat_pos   = ((long)nat_x  << 32) | (long)nat_y;
    long nat_sz    = ((long)nat_w  << 32) | (long)nat_h;
    long anim_pos  = ((long)anim_x << 32) | (long)anim_y;
    long anim_wha  = ((long)anim_w << 32) | ((long)anim_h << 16) | (long)alpha;
    return _sys5(SYS_COMPOSITE_ANIM, (long)bo, nat_pos, nat_sz, anim_pos, anim_wha);
}

/* ── AetherFS write syscalls (Phase 5.5) ────────────────────────── */

#define SYS_FS_WRITE  804
#define SYS_FS_CREATE 805
#define SYS_FS_MKDIR  806
#define SYS_FS_RM     807

/* Create (or truncate) a file by path; returns vfd (>=200) or -1 */
static inline long sys_fs_create(const char *path)
{
    return _sys1(SYS_FS_CREATE, (long)(const void *)path);
}

/* Write len bytes from buf to vfd; returns bytes written or -1 */
static inline long sys_fs_write(long vfd, const void *buf, long len)
{
    return _sys3(SYS_FS_WRITE, vfd, (long)buf, len);
}

/* Create a directory by path (FAT32 only); returns 0 or -1 */
static inline long sys_fs_mkdir(const char *path)
{
    return _sys1(SYS_FS_MKDIR, (long)(const void *)path);
}

/* Remove file or directory by path (FAT32 only); returns 0 or -1 */
static inline long sys_fs_rm(const char *path)
{
    return _sys1(SYS_FS_RM, (long)(const void *)path);
}

/* ── String helpers (no libc) ────────────────────────────────────── */

static inline long sys_puts(const char *s)
{
    long len = 0;
    while (s[len]) len++;
    return sys_write(STDOUT_FILENO, s, len);
}

#endif /* AETHER_USERSPACE_SYS_H */
