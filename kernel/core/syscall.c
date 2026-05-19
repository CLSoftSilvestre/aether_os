/*
 * AetherOS — System Call Dispatcher
 * File: kernel/core/syscall.c
 *
 * All user-space calls to the kernel arrive here via SVC #0.
 * The exception handler (el0_sync_handler in exceptions.c) checks that
 * the fault was an SVC, then calls syscall_dispatch().
 *
 * Calling convention (matches Linux AArch64 ABI):
 *   x8  = syscall number
 *   x0  = arg0   (also return value on exit)
 *   x1  = arg1
 *   x2  = arg2
 *   x3  = arg3
 *   x4  = arg4
 *   x5  = arg5
 *
 * The trap_frame_t holds all user registers as saved by EXCEPTION_ENTRY.
 * We read arguments from frame->x[n] and write the return value to frame->x[0].
 */

#include "aether/syscall.h"
#include "aether/exceptions.h"
#include "aether/initrd.h"
#include "aether/mm.h"
#include "aether/pipe.h"
#include "aether/printk.h"
#include "aether/process.h"
#include "aether/scheduler.h"
#include "aether/types.h"
#include "aether/wm.h"
#include "drivers/char/uart_pl011.h"
#include "drivers/timer/arm_timer.h"
#include "drivers/video/fb.h"
#include "drivers/video/font.h"
#include "drivers/video/fb_console.h"
#include "drivers/video/cursor.h"
#include "drivers/input/pl050_kbd.h"
#include "drivers/input/pl050_mouse.h"
#include "drivers/input/keycodes.h"
#include "aether/net.h"
#include "aether/ip.h"
#include "aether/dns.h"
#include "aether/socket.h"
#include "aether/vfs.h"
#include "aether/kmalloc.h"
#include "drivers/gpu/v3d.h"
#include "aether/vmm.h"
#include "drivers/power/cpufreq.h"
#include "drivers/power/thermal.h"
#include "drivers/power/dpms.h"
#include "drivers/rtc/pl031.h"

/* ── Wallpaper sharing globals (Phase 6.1.x) ────────────────────────────── */
static uintptr_t g_wp_ptr   = 0;   /* PMM physical address (kernel range) */
static u32       g_wp_bmpw  = 0;
static u32       g_wp_bmph  = 0;
static u32       g_wp_pages = 0;   /* number of PMM pages owned */

/* ── Individual syscall implementations ─────────────────────────────────── */

/* Network status struct (written into user buffer at arg0) */
typedef struct {
    u32 ip, mask, gateway, dns;
    u8  mac[6];
    u8  _pad[2];
} net_status_t;

static long do_sys_net_status(long buf_ptr) {
    net_status_t *st = (net_status_t *)buf_ptr;
    if (!st) return -1;
    st->ip      = g_our_ip;
    st->mask    = g_subnet_mask;
    st->gateway = g_gateway_ip;
    st->dns     = g_dns_ip;
    for (int i = 0; i < 6; i++) st->mac[i] = g_our_mac[i];
    st->_pad[0] = st->_pad[1] = 0;
    return 0;
}

static long do_sys_net_ping(long ip_u32) {
    u32 rtt = icmp_ping((u32)ip_u32);
    return (long)(s32)rtt;   /* (u32)-1 cast to s32 = -1 for error */
}

static long do_sys_net_dns(long hostname_ptr) {
    const char *h = (const char *)hostname_ptr;
    if (!h) return 0;
    return (long)dns_resolve(h);
}

static long do_sys_socket(long type) {
    return (long)sock_create((int)type);
}

static long do_sys_connect(long fd, long ip_u32, long port) {
    return (long)sock_connect((int)fd, (u32)ip_u32, (u16)port);
}

static long do_sys_net_send(long fd, long buf_ptr, long len) {
    if (!buf_ptr || len <= 0) return -1;
    if (len > 1460) len = 1460;
    return (long)sock_send((int)fd, (const u8 *)buf_ptr, (u16)len);
}

static long do_sys_net_recv(long fd, long buf_ptr, long len) {
    if (!buf_ptr || len <= 0) return -1;
    return (long)sock_recv((int)fd, (u8 *)buf_ptr, (u16)len, 5000u);
}

static long do_sys_net_close(long fd) {
    return (long)sock_close((int)fd);
}

/*
 * sys_exit — terminate the calling process.
 *
 * For Phase 3 MVP: marks the current task as dead and yields.
 * The scheduler will never schedule it again.
 *
 * In a full implementation this would also:
 *   - Close all file descriptors
 *   - Unmap virtual memory
 *   - Notify parent (if waiting in sys_wait)
 *   - Free the task control block
 */
static long do_sys_exit(long exit_code)
{
    kinfo("[SYS] sys_exit(%ld) — process %lu terminating\n",
          exit_code, (unsigned long)task_current_pid());
    task_exit();   /* noreturn — switches to another task */
}

/* Resolve an fd number to an fd_entry_t* for the current task.
 * Returns NULL if fd is out of range or closed. */
static fd_entry_t *resolve_fd(long fd)
{
    if (fd < 0 || fd >= PROC_MAX_FD) return NULL;
    fd_entry_t *e = task_get_fd((u32)fd);
    if (!e || e->type == FD_TYPE_CLOSED) return NULL;
    return e;
}

static long do_sys_write(long fd, const char *buf, long len)
{
    if (!buf || len <= 0) return 0;
    if (len > 4096) len = 4096;

    fd_entry_t *e = resolve_fd(fd);
    if (!e) { kwarn("[SYS] sys_write: bad fd=%ld\n", fd); return -1; }

    if (e->type == FD_TYPE_UART) {
        for (long i = 0; i < len; i++) uart_putc(buf[i]);
        return len;
    }
    if (e->type == FD_TYPE_PIPE_W)
        return pipe_write((int)e->pipe_idx, buf, len);

    kwarn("[SYS] sys_write: fd=%ld has unwritable type %u\n", fd, e->type);
    return -1;
}

static long do_sys_read(long fd, char *buf, long len)
{
    if (!buf || len <= 0) return 0;

    fd_entry_t *e = resolve_fd(fd);
    if (!e) { kwarn("[SYS] sys_read: bad fd=%ld\n", fd); return -1; }

    if (e->type == FD_TYPE_UART) {
        while (uart_rx_empty())
            __asm__ volatile("wfi" ::: "memory");
        long n = 0;
        while (n < len && !uart_rx_empty())
            buf[n++] = uart_getc_nowait();
        return n;
    }
    if (e->type == FD_TYPE_PIPE_R)
        return pipe_read((int)e->pipe_idx, buf, len);

    kwarn("[SYS] sys_read: fd=%ld has unreadable type %u\n", fd, e->type);
    return -1;
}

/*
 * sys_initrd_ls — list initrd files into a user-supplied buffer.
 *
 * Returns bytes written (not counting NUL), or -1 on error.
 */
static long do_sys_initrd_ls(char *buf, long len)
{
    if (!buf || len <= 0) return -1;
    return (long)initrd_list(buf, (u32)len);
}

/*
 * sys_sched_yield — voluntarily surrender the CPU.
 * Delegates to the cooperative scheduler's task_yield().
 */
static long do_sys_sched_yield(void)
{
    task_yield();
    return 0;
}

static long do_sys_initrd_read(const char *name, char *buf, long len)
{
    if (!name || !buf || len <= 0) return -1;
    u32 n = initrd_read(name, buf, (u32)len);
    return (n == (u32)-1) ? -1L : (long)n;
}

static long do_sys_pmm_stats(void)
{
    u32 free  = pmm_free_pages();
    u32 total = pmm_total_pages();
    return (long)(((u64)free << 32) | (u64)total);
}

/* ── Process management syscalls (Phase 4.3) ─────────────────────────────── */

static long do_sys_spawn(const char *path)
{
    if (!path) return -1;
    u32 child_pid = 0;
    u32 ppid = task_current_pid();
    int rc = process_spawn_child(path, ppid, &child_pid);
    return rc == 0 ? (long)child_pid : -1L;
}

/* SYS_SPAWN_ARGS (29): path + user argv[] + argc — x1 points to char*[] */
static long do_sys_spawn_args(const char *path,
                               const char *const *argv, u32 argc)
{
    if (!path || !argv || argc == 0) return -1;
    u32 child_pid = 0;
    u32 ppid = task_current_pid();
    int rc = process_spawn_child_args(path, ppid, &child_pid, argv, argc);
    return rc == 0 ? (long)child_pid : -1L;
}

static long do_sys_waitpid(long pid, int *status)
{
    return (long)task_waitpid((u32)pid, status);
}

static long do_sys_waitpid_nb(long pid, int *status)
{
    return (long)task_waitpid_nb((u32)pid, status);
}

static long do_sys_kill(long pid)
{
    return (long)task_kill((u32)pid, -1);
}

static long do_sys_getpid(void)
{
    return (long)task_current_pid();
}

static long do_sys_ps(ps_entry_t *entries, long max)
{
    if (!entries || max <= 0) return -1;
    return (long)task_ps(entries, (int)max);
}

/* ── IPC syscalls (Phase 4.3) ────────────────────────────────────────────── */

static long do_sys_pipe(int *fds)
{
    if (!fds) return -1;

    int pipe_idx = pipe_alloc();
    if (pipe_idx < 0) return -1;

    /* Find two free fd slots in the current task */
    int rfd = task_alloc_fd(FD_TYPE_PIPE_R, (u16)pipe_idx);
    int wfd = task_alloc_fd(FD_TYPE_PIPE_W, (u16)pipe_idx);

    if (rfd < 0 || wfd < 0) {
        pipe_close_read(pipe_idx);
        pipe_close_write(pipe_idx);
        if (rfd >= 0) task_close_fd((u32)rfd);
        if (wfd >= 0) task_close_fd((u32)wfd);
        return -1;
    }

    fds[0] = rfd;
    fds[1] = wfd;
    return 0;
}

static long do_sys_dup2(long oldfd, long newfd)
{
    if (oldfd < 0 || newfd < 0 || oldfd >= PROC_MAX_FD || newfd >= PROC_MAX_FD)
        return -1;
    return task_dup2_fd((u32)oldfd, (u32)newfd);
}

/* ── Graphics syscalls ───────────────────────────────────────────────────── */

/*
 * Arg packing (all use _sys3 ABI):
 *
 *  sys_fb_fill:  arg0 = (x << 32 | y)   arg1 = (w << 32 | h)   arg2 = color
 *  sys_fb_char:  arg0 = (x << 32 | y)   arg1 = (ch << 32 | fg) arg2 = bg
 *
 * This avoids needing a struct pointer and works with the existing 3-arg ABI.
 */

static long do_sys_fb_fill(u64 xy, u64 wh, u64 color)
{
    u32 x = (u32)(xy >> 32);
    u32 y = (u32)(xy & 0xFFFFFFFFu);
    u32 w = (u32)(wh >> 32);
    u32 h = (u32)(wh & 0xFFFFFFFFu);
    cursor_hide();
    fb_fill_rect(x, y, w, h, (u32)color);
    cursor_show();
    return 0;
}

static long do_sys_fb_char(u64 xy, u64 ch_fg, u64 bg)
{
    u32 x  = (u32)(xy >> 32);
    u32 y  = (u32)(xy & 0xFFFFFFFFu);
    u8  ch = (u8)(ch_fg >> 32);
    u32 fg = (u32)(ch_fg & 0xFFFFFFFFu);
    cursor_hide();
    font_draw_char(x, y, ch, fg, (u32)bg);
    cursor_show();
    return 0;
}

static long do_sys_get_ticks(void)
{
    return (long)timer_get_ticks();
}

static long do_sys_fb_claim(void)
{
    fb_console_claim();
    v3d_dbl_init();    /* allocate compositor back buffer at FB resolution */
    v3d_blur_init();   /* allocate WM4 Kawase blur scratch (quarter-res × 2) */
    return 0;
}

/* SYS_FB_BLIT — copy a user XRGB8888 pixel buffer to the framebuffer.
 * arg0 = src pointer, arg1 = dst_x<<32|dst_y, arg2 = w<<32|h,
 * arg3 = src_stride_bytes (bytes per row in the source buffer). */
static long do_sys_fb_blit(u64 buf_ptr, u64 xy, u64 wh, u64 stride_bytes)
{
    const u32 *src = (const u32 *)buf_ptr;
    u32 dx = (u32)(xy >> 32), dy = (u32)(xy & 0xFFFFFFFFu);
    u32 w  = (u32)(wh >> 32), h  = (u32)(wh & 0xFFFFFFFFu);
    if (!src || !fb_base) return -1;
    /* Drop blits from minimized windows so legacy apps cannot overdraw the
     * wallpaper that init painted when the window was hidden. */
    if (wm_is_pid_minimized(task_current_pid())) return 0;
    cursor_hide();
    fb_blit(src, dx, dy, w, h, (u32)stride_bytes);
    cursor_show();
    return 0;
}

/* ── UART → key_event fallback (used when PL050 KMI is absent) ─────────── */

/*
 * Maps ASCII bytes from UART (+ ANSI escape sequences for arrow keys) to
 * packed key_event_t values.  This lets the shell run identically whether
 * input comes from real PS/2 hardware or a UART terminal emulator.
 */

static const keycode_t lc_to_kc[26] = {
    KEY_A,KEY_B,KEY_C,KEY_D,KEY_E,KEY_F,KEY_G,KEY_H,KEY_I,KEY_J,KEY_K,
    KEY_L,KEY_M,KEY_N,KEY_O,KEY_P,KEY_Q,KEY_R,KEY_S,KEY_T,KEY_U,KEY_V,
    KEY_W,KEY_X,KEY_Y,KEY_Z
};
static const keycode_t digit_to_kc[10] = {
    KEY_0,KEY_1,KEY_2,KEY_3,KEY_4,KEY_5,KEY_6,KEY_7,KEY_8,KEY_9
};

static unsigned long long uart_to_key_event(void)
{
    while (uart_rx_empty()) task_yield();
    u8 c = (u8)uart_getc_nowait();
    key_event_t ev = { KEY_NONE, 0, 1 };

    /* ANSI escape sequence: ESC [ A/B/C/D → arrow keys */
    if (c == 0x1B) {
        int t = 5000;
        while (uart_rx_empty() && --t) ;
        if (!uart_rx_empty()) {
            u8 c2 = (u8)uart_getc_nowait();
            if (c2 == '[') {
                t = 5000;
                while (uart_rx_empty() && --t) ;
                if (!uart_rx_empty()) {
                    u8 seq = (u8)uart_getc_nowait();
                    switch (seq) {
                    case 'A': ev.keycode = KEY_UP;    break;
                    case 'B': ev.keycode = KEY_DOWN;  break;
                    case 'C': ev.keycode = KEY_RIGHT; break;
                    case 'D': ev.keycode = KEY_LEFT;  break;
                    case 'H': ev.keycode = KEY_HOME;  break;
                    case 'F': ev.keycode = KEY_END;   break;
                    default:  ev.keycode = KEY_ESC;   break;
                    }
                    return key_event_pack(ev);
                }
            }
        }
        ev.keycode = KEY_ESC;
        return key_event_pack(ev);
    }

    /* Ctrl+C (0x03) */
    if (c == 0x03) { ev.keycode = KEY_C; ev.modifiers = MOD_CTRL; return key_event_pack(ev); }
    /* Enter */
    if (c == '\r' || c == '\n') { ev.keycode = KEY_ENTER; return key_event_pack(ev); }
    /* Backspace / DEL */
    if (c == '\b' || c == 127)  { ev.keycode = KEY_BACKSPACE; return key_event_pack(ev); }
    /* Tab */
    if (c == '\t')              { ev.keycode = KEY_TAB; return key_event_pack(ev); }
    /* Space */
    if (c == ' ')               { ev.keycode = KEY_SPACE; return key_event_pack(ev); }

    /* Lowercase letters */
    if (c >= 'a' && c <= 'z') { ev.keycode = lc_to_kc[c-'a']; return key_event_pack(ev); }
    /* Uppercase letters */
    if (c >= 'A' && c <= 'Z') { ev.keycode = lc_to_kc[c-'A']; ev.modifiers = MOD_SHIFT; return key_event_pack(ev); }
    /* Digits */
    if (c >= '0' && c <= '9') { ev.keycode = digit_to_kc[c-'0']; return key_event_pack(ev); }

    /* Punctuation — unshifted */
    switch (c) {
    case '-':  ev.keycode = KEY_MINUS;       break;
    case '=':  ev.keycode = KEY_EQUALS;      break;
    case '[':  ev.keycode = KEY_LBRACKET;    break;
    case ']':  ev.keycode = KEY_RBRACKET;    break;
    case '\\': ev.keycode = KEY_BACKSLASH;   break;
    case ';':  ev.keycode = KEY_SEMICOLON;   break;
    case '\'': ev.keycode = KEY_APOSTROPHE;  break;
    case ',':  ev.keycode = KEY_COMMA;       break;
    case '.':  ev.keycode = KEY_DOT;         break;
    case '/':  ev.keycode = KEY_SLASH;       break;
    case '`':  ev.keycode = KEY_GRAVE;       break;
    /* Shifted punctuation */
    case '_':  ev.keycode = KEY_MINUS;      ev.modifiers = MOD_SHIFT; break;
    case '+':  ev.keycode = KEY_EQUALS;     ev.modifiers = MOD_SHIFT; break;
    case '{':  ev.keycode = KEY_LBRACKET;   ev.modifiers = MOD_SHIFT; break;
    case '}':  ev.keycode = KEY_RBRACKET;   ev.modifiers = MOD_SHIFT; break;
    case '|':  ev.keycode = KEY_BACKSLASH;  ev.modifiers = MOD_SHIFT; break;
    case ':':  ev.keycode = KEY_SEMICOLON;  ev.modifiers = MOD_SHIFT; break;
    case '"':  ev.keycode = KEY_APOSTROPHE; ev.modifiers = MOD_SHIFT; break;
    case '<':  ev.keycode = KEY_COMMA;      ev.modifiers = MOD_SHIFT; break;
    case '>':  ev.keycode = KEY_DOT;        ev.modifiers = MOD_SHIFT; break;
    case '?':  ev.keycode = KEY_SLASH;      ev.modifiers = MOD_SHIFT; break;
    case '~':  ev.keycode = KEY_GRAVE;      ev.modifiers = MOD_SHIFT; break;
    /* Shifted digits */
    case '!': ev.keycode = KEY_1; ev.modifiers = MOD_SHIFT; break;
    case '@': ev.keycode = KEY_2; ev.modifiers = MOD_SHIFT; break;
    case '#': ev.keycode = KEY_3; ev.modifiers = MOD_SHIFT; break;
    case '$': ev.keycode = KEY_4; ev.modifiers = MOD_SHIFT; break;
    case '%': ev.keycode = KEY_5; ev.modifiers = MOD_SHIFT; break;
    case '^': ev.keycode = KEY_6; ev.modifiers = MOD_SHIFT; break;
    case '&': ev.keycode = KEY_7; ev.modifiers = MOD_SHIFT; break;
    case '*': ev.keycode = KEY_8; ev.modifiers = MOD_SHIFT; break;
    case '(': ev.keycode = KEY_9; ev.modifiers = MOD_SHIFT; break;
    case ')': ev.keycode = KEY_0; ev.modifiers = MOD_SHIFT; break;
    default:  break;
    }
    return key_event_pack(ev);
}

/* ── Input syscalls (Phase 4.5) ─────────────────────────────────────────── */

static long do_sys_key_read(void)
{
    /* Prefer PS/2 hardware events; fall back to UART when KMI is absent */
    if (!kbd_event_empty())
        return (long)kbd_get_event();
    return (long)uart_to_key_event();
}

static long do_sys_key_poll(void)
{
    if (!kbd_event_empty()) return (long)kbd_get_event();
    return 0L;   /* Non-blocking: return 0 if no PS/2 event pending */
}

static long do_sys_mouse_read(void)
{
    while (mouse_event_empty())
        task_yield();
    return (long)mouse_get_event();
}

static long do_sys_mouse_poll(void)
{
    if (mouse_event_empty()) return 0L;
    unsigned long long ev = mouse_get_event();
    static int dbg = 0;
    if (dbg++ < 10)
        kinfo("[mouse_poll] x=%u y=%u\n",
              (unsigned)((ev >> 48) & 0xFFFFu),
              (unsigned)((ev >> 32) & 0xFFFFu));
    return (long)ev;
}

static long do_sys_cursor_move(u64 xy)
{
    u32 x = (u32)(xy >> 32);
    u32 y = (u32)(xy & 0xFFFFFFFFu);
    cursor_move(x, y);
    return 0;
}

static long do_sys_cursor_show(u64 visible)
{
    if (visible) cursor_show();
    else         cursor_hide();
    return 0;
}

/* ── Window Manager syscalls (Phase 4.6) ────────────────────────────────── */

static long do_sys_wm_register(u64 xy, u64 wh, const char *title)
{
    int x = (int)(u32)(xy >> 32);
    int y = (int)(u32)(xy & 0xFFFFFFFFu);
    int w = (int)(u32)(wh >> 32);
    int h = (int)(u32)(wh & 0xFFFFFFFFu);
    return (long)wm_register(task_current_pid(), x, y, w, h, title);
}

/*
 * SYS_WM_KEY_RECV — block until a key/WM event arrives in this process's
 * per-PID ring.  Each iteration drains the hardware keyboard ring into the
 * focused PID's FIFO (consistent with the cooperative scheduling model —
 * whoever runs drains the hardware, routes to focused, checks own queue).
 */
static long do_sys_wm_key_recv(void)
{
    u32 pid = task_current_pid();

    for (;;) {
        /* Drain PS/2 / virtio-input hardware events → focused PID's FIFO */
        while (!kbd_event_empty())
            wm_deliver_key(kbd_get_event());

        /* UART fallback: only when hardware ring is empty and UART has data */
        if (kbd_event_empty() && !uart_rx_empty())
            wm_deliver_key(uart_to_key_event());

        /* Return if our FIFO has an event (may or may not be focused) */
        u64 ev = wm_key_dequeue(pid);
        if (ev)
            return (long)ev;

        task_yield();
    }
}

static long do_sys_wm_unregister(long win_id)
{
    wm_unregister((int)win_id);
    return 0;
}

static long do_sys_wm_focus_set(u64 pid)
{
    wm_focus_set((u32)pid);
    return 0;
}

static long do_sys_wm_focus_get(void)  { return (long)wm_focus_get(); }

static long do_sys_wm_move(long win_id, u64 xy)
{
    int x = (int)(u32)(xy >> 32);
    int y = (int)(u32)(xy & 0xFFFFFFFFu);
    wm_move((int)win_id, x, y);
    return 0;
}

static long do_sys_wm_get_pos(long win_id)  { return wm_get_pos((int)win_id); }
static long do_sys_wm_get_size(long win_id) { return wm_get_size((int)win_id); }
static long do_sys_wm_get_pid(long win_id)  { return (long)wm_get_pid((int)win_id); }

/* SYS_WM_CLOSE — privileged: only PID 1 (init) may call this.
 * With compositor: triggers close animation, compositor calls SYS_WM_CLOSE_DONE.
 * Without compositor: hard-kills immediately (legacy fallback). */
static long do_sys_wm_close(long win_id)
{
    if (task_current_pid() != 1) {
        kwarn("[SYS] SYS_WM_CLOSE: caller pid=%lu is not PID 1\n",
              (unsigned long)task_current_pid());
        return -1;
    }
    u32 owner = wm_get_pid((int)win_id);
    if (!owner) return -1;
    if (wm_get_compositor()) {
        wm_request_close((int)win_id);   /* compositor handles animation + close_done */
    } else {
        task_kill(owner, -1);            /* no compositor: hard-kill immediately */
    }
    return 0;
}

/* SYS_WM_EVENT_POLL — non-blocking dequeue from the calling process's WM ring.
 * Also drains the hardware keyboard buffer so that widget apps that never call
 * the blocking SYS_WM_KEY_RECV still get key events routed to the focused PID. */
static long do_sys_wm_event_poll(void)
{
    while (!kbd_event_empty())
        wm_deliver_key(kbd_get_event());
    if (kbd_event_empty() && !uart_rx_empty())
        wm_deliver_key(uart_to_key_event());
    return (long)wm_key_dequeue(task_current_pid());
}

/* SYS_WM_PUSH_EVENT — inject a packed event into any PID's WM ring.
 * Used by init to forward mouse events to the focused/hit window so that
 * widget apps receive mouse input via their sys_wm_event_poll() loop. */
static long do_sys_wm_push_event(u64 pid, u64 event)
{
    wm_deliver_to_pid((u32)pid, event);
    return 0;
}

/* ── Compositing WM syscalls (wmanager branch) ───────────────────────────── */

static long do_sys_wm_set_zindex(long win_id, long z)
{
    wm_set_zindex((int)win_id, (s32)z);
    return 0;
}

static long do_sys_wm_set_opacity(long win_id, long opacity)
{
    wm_set_opacity((int)win_id, (u8)(opacity & 0xFFu));
    return 0;
}

static long do_sys_wm_set_blur(long win_id, long radius)
{
    wm_set_blur((int)win_id, (u8)(radius & 0xFFu));
    return 0;
}

static long do_sys_wm_set_visible(long win_id, long visible)
{
    wm_set_visible((int)win_id, (u8)(visible ? 1 : 0));
    return 0;
}

static long do_sys_wm_set_buffer(long win_id, long buf_handle)
{
    wm_set_buffer((int)win_id, (u32)buf_handle);
    return 0;
}

static long do_sys_wm_get_buffer(long win_id)
{
    return (long)wm_get_buffer((int)win_id);
}

static long do_sys_wm_damage(long win_id)
{
    wm_damage((int)win_id);
    return 0;
}

static long do_sys_wm_damage_clear(long win_id)
{
    wm_damage_clear((int)win_id);
    return 0;
}

static long do_sys_wm_set_compositor(long pid)
{
    wm_set_compositor((u32)pid);
    return 0;
}

static long do_sys_wm_get_zindex(long win_id)
{
    return (long)wm_get_zindex((int)win_id);
}

/* WM5.5 — close-animation protocol handlers */
static long do_sys_wm_request_close(long win_id)
{
    wm_request_close((int)win_id);
    return 0;
}

static long do_sys_wm_close_done(long win_id)
{
    u32 owner = wm_get_pid((int)win_id);   /* snapshot pid before wm_unregister clears it */
    wm_close_done((int)win_id);
    if (owner > 1)
        task_kill(owner, -1);              /* kill app if still alive (no-op if already exited) */
    return 0;
}

/* WM7a — minimize/restore protocol handlers */
static long do_sys_wm_minimize(long win_id)
{
    wm_minimize((int)win_id);
    return 0;
}

static long do_sys_wm_restore(long win_id)
{
    wm_restore((int)win_id);
    return 0;
}

/* WM9 — window behaviour flags */
static long do_sys_wm_set_flags(long win_id, long flags)
{
    wm_set_flags((int)win_id, (u8)(flags & 0xFFu));
    return 0;
}

/* WM7b — live-resize protocol handler */
static long do_sys_wm_resize(long win_id, u64 wh)
{
    int new_w = (int)(u32)(wh >> 32);
    int new_h = (int)(u32)(wh & 0xFFFFFFFFu);
    wm_resize((int)win_id, new_w, new_h);
    return 0;
}

/* SYS_WM_ENUM — fill user buffer with wm_entry_t snapshots.
 * arg0 = pointer to wm_entry_t array, arg1 = max entries. */
static long do_sys_wm_enum(u64 entries_ptr, long max)
{
    wm_entry_t *entries = (wm_entry_t *)entries_ptr;
    if (!entries || max <= 0) return 0;
    return (long)wm_enum(entries, (int)max);
}

/* ── Clipboard (Phase 5.3) ───────────────────────────────────────────────── */

#define CLIPBOARD_SIZE 4096

static char g_clipboard[CLIPBOARD_SIZE];
static u32  g_clipboard_len;

static long do_sys_clipboard_write(long buf_ptr, long len)
{
    const char *src = (const char *)buf_ptr;
    if (!src || len <= 0) return -1;
    u32 n = (u32)len;
    if (n >= CLIPBOARD_SIZE) n = CLIPBOARD_SIZE - 1;
    for (u32 i = 0; i < n; i++) g_clipboard[i] = src[i];
    g_clipboard[n] = '\0';
    g_clipboard_len = n;
    return 0;
}

static long do_sys_clipboard_read(long buf_ptr, long max_len)
{
    char *dst = (char *)buf_ptr;
    if (!dst || max_len <= 0) return -1;
    u32 n = g_clipboard_len;
    if ((u32)max_len - 1 < n) n = (u32)max_len - 1;
    for (u32 i = 0; i < n; i++) dst[i] = g_clipboard[i];
    dst[n] = '\0';
    return (long)n;
}

/* ── Dispatcher ─────────────────────────────────────────────────────────── */

/*
 * syscall_dispatch — called from el0_sync_handler.
 *
 * Reads syscall number from frame->x[8], arguments from frame->x[0..5],
 * calls the appropriate handler, and returns the result.
 * The caller writes the result back into frame->x[0].
 */
long syscall_dispatch(trap_frame_t *frame)
{
    u64 nr   = frame->x[8];   /* syscall number */
    u64 arg0 = frame->x[0];
    u64 arg1 = frame->x[1];
    u64 arg2 = frame->x[2];

    switch (nr) {
    case SYS_EXIT:
        do_sys_exit((long)arg0);
        return 0;

    case SYS_SPAWN:
        return do_sys_spawn((const char *)arg0);

    case SYS_SPAWN_ARGS:
        return do_sys_spawn_args((const char *)arg0,
                                  (const char *const *)arg1, (u32)arg2);

    case SYS_KILL:
        return do_sys_kill((long)arg0);

    case SYS_WAITPID:
        return do_sys_waitpid((long)arg0, (int *)arg1);

    case SYS_WAITPID_NB:
        return do_sys_waitpid_nb((long)arg0, (int *)arg1);

    case SYS_GETPID:
        return do_sys_getpid();

    case SYS_PS:
        return do_sys_ps((ps_entry_t *)arg0, (long)arg1);

    case SYS_PIPE:
        return do_sys_pipe((int *)arg0);

    case SYS_DUP2:
        return do_sys_dup2((long)arg0, (long)arg1);

    case SYS_CLOSE:
        task_close_fd((u32)arg0);
        return 0;

    case SYS_READ:
        return do_sys_read((long)arg0, (char *)arg1, (long)arg2);

    case SYS_WRITE:
        return do_sys_write((long)arg0, (const char *)arg1, (long)arg2);

    case SYS_INITRD_LS:
        return do_sys_initrd_ls((char *)arg0, (long)arg1);

    case SYS_INITRD_READ:
        return do_sys_initrd_read((const char *)arg0, (char *)arg1, (long)arg2);

    case SYS_PMM_STATS:
        return do_sys_pmm_stats();

    case SYS_KMALLOC_STATS: {
        kmalloc_stats_t *s = (kmalloc_stats_t *)arg0;
        if (!s) return -1;
        kmalloc_get_stats(s);
        return 0;
    }

    case SYS_SCHED_YIELD:
        return do_sys_sched_yield();

    case SYS_SLEEP_TICKS:
        task_sleep((u64)arg0);
        return 0;

    case SYS_KEY_READ:    return do_sys_key_read();
    case SYS_KEY_POLL:    return do_sys_key_poll();
    case SYS_MOUSE_READ:  return do_sys_mouse_read();
    case SYS_MOUSE_POLL:  return do_sys_mouse_poll();
    case SYS_CURSOR_MOVE: return do_sys_cursor_move(arg0);
    case SYS_CURSOR_SHOW: return do_sys_cursor_show(arg0);

    case SYS_FB_INFO:
        return ((long)fb_width << 32) | (long)fb_height;

    case SYS_FB_FILL:
        return do_sys_fb_fill(arg0, arg1, arg2);

    case SYS_FB_CHAR:
        return do_sys_fb_char(arg0, arg1, arg2);

    case SYS_FB_CHAR_NOBG: {
        u32 x  = (u32)((u64)arg0 >> 32);
        u32 y  = (u32)((u64)arg0 & 0xFFFFFFFFu);
        u32 ch = (u32)((u64)arg1 >> 32) & 0xFFu;
        u32 fg = (u32)((u64)arg1 & 0x00FFFFFFu);
        font_draw_char_nobg(x, y, (unsigned char)ch, fg);
        return 0;
    }

    case SYS_GET_TICKS:
        return do_sys_get_ticks();

    case SYS_RTC_GET:
        return (long)pl031_read();

    case SYS_FB_CLAIM:
        return do_sys_fb_claim();

    case SYS_FB_BLIT: {
        u64 arg3 = frame->x[3];
        return do_sys_fb_blit(arg0, arg1, arg2, arg3);
    }

    case SYS_WM_REGISTER:
        return do_sys_wm_register(arg0, arg1, (const char *)arg2);
    case SYS_WM_KEY_RECV:
        return do_sys_wm_key_recv();
    case SYS_WM_UNREGISTER:
        return do_sys_wm_unregister((long)arg0);
    case SYS_WM_FOCUS_SET:
        return do_sys_wm_focus_set(arg0);
    case SYS_WM_FOCUS_GET:
        return do_sys_wm_focus_get();
    case SYS_WM_MOVE:
        return do_sys_wm_move((long)arg0, arg1);
    case SYS_WM_GET_POS:
        return do_sys_wm_get_pos((long)arg0);
    case SYS_WM_GET_SIZE:
        return do_sys_wm_get_size((long)arg0);
    case SYS_WM_GET_PID:
        return do_sys_wm_get_pid((long)arg0);
    case SYS_WM_CLOSE:
        return do_sys_wm_close((long)arg0);
    case SYS_WM_EVENT_POLL:
        return do_sys_wm_event_poll();
    case SYS_WM_PUSH_EVENT:
        return do_sys_wm_push_event(arg0, arg1);

    /* Compositing WM (wmanager branch) */
    case SYS_WM_SET_ZINDEX:     return do_sys_wm_set_zindex((long)arg0, (long)arg1);
    case SYS_WM_SET_OPACITY:    return do_sys_wm_set_opacity((long)arg0, (long)arg1);
    case SYS_WM_SET_BLUR:       return do_sys_wm_set_blur((long)arg0, (long)arg1);
    case SYS_WM_SET_VISIBLE:    return do_sys_wm_set_visible((long)arg0, (long)arg1);
    case SYS_WM_SET_BUFFER:     return do_sys_wm_set_buffer((long)arg0, (long)arg1);
    case SYS_WM_GET_BUFFER:     return do_sys_wm_get_buffer((long)arg0);
    case SYS_WM_DAMAGE:         return do_sys_wm_damage((long)arg0);
    case SYS_WM_DAMAGE_CLEAR:   return do_sys_wm_damage_clear((long)arg0);
    case SYS_WM_SET_COMPOSITOR: return do_sys_wm_set_compositor((long)arg0);
    case SYS_WM_GET_ZINDEX:     return do_sys_wm_get_zindex((long)arg0);
    case SYS_WM_ENUM:           return do_sys_wm_enum(arg0, (long)arg1);
    case SYS_WM_REQUEST_CLOSE:  return do_sys_wm_request_close((long)arg0);
    case SYS_WM_CLOSE_DONE:     return do_sys_wm_close_done((long)arg0);
    case SYS_WM_MINIMIZE:       return do_sys_wm_minimize((long)arg0);
    case SYS_WM_RESTORE:        return do_sys_wm_restore((long)arg0);
    case SYS_WM_RESIZE:         return do_sys_wm_resize((long)arg0, arg1);
    case SYS_WM_SET_FLAGS:      return do_sys_wm_set_flags((long)arg0, (long)arg1);

    /* Clipboard (Phase 5.3) */
    case SYS_CLIPBOARD_WRITE: return do_sys_clipboard_write(arg0, arg1);
    case SYS_CLIPBOARD_READ:  return do_sys_clipboard_read(arg0, arg1);

    /* Filesystem (Phase 5.2 + write prerequisite for 5.5) */
    case SYS_FS_OPEN:    return (long)vfs_open   ((const char *)arg0);
    case SYS_FS_READ:    return (long)vfs_read   ((int)arg0, (u8 *)arg1, (u32)arg2);
    case SYS_FS_CLOSE:   vfs_close((int)arg0); return 0;
    case SYS_FS_READDIR: return (long)vfs_readdir((const char *)arg0, (char *)arg1, (u32)arg2);
    case SYS_FS_CREATE:  return (long)vfs_create ((const char *)arg0);
    case SYS_FS_WRITE:   return (long)vfs_write  ((int)arg0, (const u8 *)arg1, (u32)arg2);
    case SYS_FS_MKDIR:   return (long)vfs_mkdir  ((const char *)arg0);
    case SYS_FS_RM:      return (long)vfs_rm     ((const char *)arg0);

    /* Networking (Phase 5.1) */
    case SYS_NET_STATUS: return do_sys_net_status(arg0);
    case SYS_NET_PING:   return do_sys_net_ping(arg0);
    case SYS_NET_DNS:    return do_sys_net_dns(arg0);
    case SYS_SOCKET:     return do_sys_socket(arg0);
    case SYS_CONNECT:    return do_sys_connect(arg0, arg1, arg2);
    case SYS_NET_SEND:   return do_sys_net_send(arg0, arg1, arg2);
    case SYS_NET_RECV:   return do_sys_net_recv(arg0, arg1, arg2);
    case SYS_NET_CLOSE:  return do_sys_net_close(arg0);

    /* GPU / V3D (Phase 6.1) */
    case SYS_VSYNC_WAIT: {
        /* Block until the next 60Hz frame boundary.
         * 60 vsyncs per 100 timer ticks: boundary N starts at tick floor(N*5/3).
         * Given current tick `now`, next boundary = floor((gen+1)*5/3)
         * where gen = floor(now*3/5). */
        u64 now  = timer_get_ticks();
        u64 gen  = (now * 3u) / 5u;
        u64 next = ((gen + 1u) * 5u) / 3u;
        if (next <= now)
            next = ((gen + 2u) * 5u) / 3u;
        task_sleep(next > now ? next - now : 1u);
        return 0;
    }

    case SYS_GPU_ALLOC:
        return (long)v3d_bo_alloc((u32)arg0);

    case SYS_GPU_FREE:
        return (long)v3d_bo_free((u32)arg0);

    case SYS_GPU_MAP: {
        /* arg0 = bo_handle, arg1 = pointer to uintptr_t in user space */
        uintptr_t phys = v3d_bo_phys((u32)arg0);
        if (!phys) return -1;

        /* Map BO physical pages into the calling process's user VA space.
         * GPU BOs live in kernel RAM (AP=EL1_RW); EL0 cannot access them
         * directly.  We allocate VA from the task's BO bump allocator
         * (0x74000000+) and map the pages there with AP=BOTH_RW. */
        u32 size = v3d_bo_size((u32)arg0);
        u32 n_pages = (u32)((size + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE);

        uintptr_t va = task_alloc_bo_va(n_pages);
        uintptr_t l1 = task_current_l1();

        if (vmm_map_user_pages(l1, va, phys, n_pages) != 0) return -1;

        /* Flush TLB so EL0 sees the new mapping immediately after eret */
        __asm__ volatile(
            "dsb ish\n"
            "tlbi vmalle1\n"
            "dsb ish\n"
            "isb\n"
            ::: "memory"
        );

        *(uintptr_t *)arg1 = va;
        return 0;
    }

    case SYS_GPU_INFO: {
        gpu_caps_t *caps = (gpu_caps_t *)arg0;
        if (!caps) return -1;
        v3d_get_caps(caps);
        return 0;
    }

    case SYS_GPU_BLUR: {
        /* arg0 = (src_handle << 32) | dst_handle
         * arg1 = (width << 32) | height
         * arg2 = radius */
        u32 src_h  = (u32)((u64)arg0 >> 32);
        u32 dst_h  = (u32)((u64)arg0 & 0xFFFFFFFFu);
        u32 width  = (u32)((u64)arg1 >> 32);
        u32 height = (u32)((u64)arg1 & 0xFFFFFFFFu);
        u32 radius = (u32)arg2;
        uintptr_t src_phys = v3d_bo_phys(src_h);
        uintptr_t dst_phys = v3d_bo_phys(dst_h);
        if (!src_phys || !dst_phys) return -1;
        return (long)v3d_blur(src_phys, dst_phys, width, height, radius);
    }

    /* Wallpaper sharing (Phase 6.1.x) */
    case SYS_WP_REGISTER: {
        uintptr_t user_va  = (uintptr_t)arg0;
        u32 bw = (u32)((u64)arg1 >> 32);
        u32 bh = (u32)((u64)arg1 & 0xFFFFFFFFu);
        u32 byte_size = bw * bh * sizeof(u32);
        u32 n_pages   = (byte_size + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE;

        /* Free a previous registration if any */
        if (g_wp_ptr && g_wp_pages) {
            for (u32 _i = 0; _i < g_wp_pages; _i++)
                pmm_free_page(g_wp_ptr + (uintptr_t)_i * PMM_PAGE_SIZE);
        }

        /* Allocate a kernel-range buffer (PA in 0x40000000–0x6FFFFFFF,
         * always EL1-accessible regardless of which process's PT is loaded) */
        uintptr_t kbuf = pmm_alloc_pages(n_pages);
        if (!kbuf) { g_wp_ptr = 0; return -1; }

        /* Copy from user VA (init's global PT is active here — EL1 can read
         * through AP=BOTH_RW 2MB blocks covering 0x70000000+) */
        const u8 *src = (const u8 *)user_va;
        u8       *dst = (u8 *)kbuf;
        for (u32 _i = 0; _i < byte_size; _i++) dst[_i] = src[_i];

        g_wp_ptr   = kbuf;
        g_wp_bmpw  = bw;
        g_wp_bmph  = bh;
        g_wp_pages = n_pages;
        return 0;
    }

    case SYS_WP_GET:
        return (long)g_wp_ptr;

    case SYS_WP_SIZE:
        return (long)(((u64)g_wp_bmpw << 32) | (u64)g_wp_bmph);

    case SYS_WP_BLEND_FILL: {
        u32 bx    = (u32)((u64)arg0 >> 32);
        u32 by    = (u32)((u64)arg0 & 0xFFFFFFFFu);
        u32 bw    = (u32)((u64)arg1 >> 32);
        u32 bh    = (u32)((u64)arg1 & 0xFFFFFFFFu);
        u32 color = (u32)arg2;

        if (bx >= fb_width || by >= fb_height) return 0;
        if (bx + bw > fb_width)  bw = fb_width  - bx;
        if (by + bh > fb_height) bh = fb_height - by;
        if (!bw || !bh) return 0;

        if (!g_wp_ptr) {
            fb_fill_rect(bx, by, bw, bh, color);
            return 0;
        }

        u32 tr = (color >> 16) & 0xFFu;
        u32 tg = (color >>  8) & 0xFFu;
        u32 tb =  color        & 0xFFu;
        u32 crop_x = (g_wp_bmpw > fb_width)  ? (g_wp_bmpw - fb_width)  / 2u : 0u;
        u32 crop_y = (g_wp_bmph > fb_height) ? (g_wp_bmph - fb_height) / 2u : 0u;
        const u32 *wp = (const u32 *)g_wp_ptr;
        u32 stride_px = fb_stride / 4u;

        for (u32 row = 0; row < bh; row++) {
            u32 sy    = by + row;
            u32 src_y = sy + crop_y;
            volatile u32 *dst = fb_base + sy * stride_px + bx;

            if (src_y >= g_wp_bmph) {
                for (u32 c = 0; c < bw; c++) dst[c] = color;
                continue;
            }
            const u32 *wp_row = wp + src_y * g_wp_bmpw;
            u32 src_x0 = bx + crop_x;
            for (u32 c = 0; c < bw; c++) {
                u32 src_x = src_x0 + c;
                if (src_x < g_wp_bmpw) {
                    u32 p  = wp_row[src_x];
                    u32 wr = (p >> 16) & 0xFFu;
                    u32 wg = (p >>  8) & 0xFFu;
                    u32 wb =  p        & 0xFFu;
                    dst[c] = (((tr*4u+wr)/5u) << 16)
                           | (((tg*4u+wg)/5u) <<  8)
                           |  ((tb*4u+wb)/5u);
                } else {
                    dst[c] = color;
                }
            }
        }
        return 0;
    }

    /* GPU blit / FB capture — window animation (Phase 6.1.7) */
    case SYS_FB_CAPTURE: {
        /* arg0 = bo_handle, arg1 = (src_x<<32|src_y), arg2 = (w<<32|h) */
        u32 bo   = (u32)arg0;
        u32 sx   = (u32)((u64)arg1 >> 32);
        u32 sy   = (u32)((u64)arg1 & 0xFFFFFFFFu);
        u32 w    = (u32)((u64)arg2 >> 32);
        u32 h    = (u32)((u64)arg2 & 0xFFFFFFFFu);
        return (long)v3d_fb_capture(bo, sx, sy, w, h);
    }

    case SYS_GPU_BLIT: {
        /* arg0 = bo_handle
         * arg1 = (src_w<<32|src_h)
         * arg2 = (dst_x<<32|dst_y)
         * arg3 = (dst_w<<32|dst_h)
         * arg4 = alpha (0-255) */
        u64 arg3 = frame->x[3];
        u64 arg4 = frame->x[4];
        u32 bo     = (u32)arg0;
        u32 src_w  = (u32)((u64)arg1 >> 32);
        u32 src_h  = (u32)((u64)arg1 & 0xFFFFFFFFu);
        u32 dst_x  = (u32)((u64)arg2 >> 32);
        u32 dst_y  = (u32)((u64)arg2 & 0xFFFFFFFFu);
        u32 dst_w  = (u32)((u64)arg3 >> 32);
        u32 dst_h  = (u32)((u64)arg3 & 0xFFFFFFFFu);
        u8  alpha  = (u8)((u64)arg4 & 0xFFu);
        return (long)v3d_blit_to_fb(bo, src_w, src_h, dst_x, dst_y, dst_w, dst_h, alpha);
    }

    case SYS_COMPOSITE_ANIM: {
        /* arg0 = bo_handle
         * arg1 = (nat_x<<32|nat_y)    — natural window position
         * arg2 = (nat_w<<32|nat_h)    — natural window size = BO source dims
         * arg3 = (anim_x<<32|anim_y)  — scaled rect position
         * arg4 = (anim_w<<32|(anim_h<<16)|alpha) */
        u64 a3 = frame->x[3];
        u64 a4 = frame->x[4];
        u32 bo     = (u32)arg0;
        u32 nat_x  = (u32)((u64)arg1 >> 32);
        u32 nat_y  = (u32)((u64)arg1 & 0xFFFFFFFFu);
        u32 nat_w  = (u32)((u64)arg2 >> 32);
        u32 nat_h  = (u32)((u64)arg2 & 0xFFFFFFFFu);
        u32 anim_x = (u32)((u64)a3 >> 32);
        u32 anim_y = (u32)((u64)a3 & 0xFFFFFFFFu);
        u32 anim_w = (u32)((u64)a4 >> 32);
        u32 anim_h = (u32)(((u64)a4 >> 16) & 0xFFFFu);
        u8  alpha  = (u8)((u64)a4 & 0xFFu);
        return (long)v3d_composite_anim(bo,
                                        nat_x, nat_y, nat_w, nat_h,
                                        anim_x, anim_y, anim_w, anim_h,
                                        alpha,
                                        g_wp_ptr, g_wp_bmpw, g_wp_bmph);
    }

    /* ── WM3: batch compositor + double-buffer flip ─────────────────────── */

    case SYS_GPU_COMPOSITE_FRAME: {
        /* arg0 = pointer to v3d_layer_t[] in user memory
         * arg1 = count (may be 0 for wallpaper-only frame) */
        const v3d_layer_t *layers = (const v3d_layer_t *)arg0;
        int n = (int)(long)arg1;
        return (long)v3d_composite_layers(layers, n,
                                          g_wp_ptr, g_wp_bmpw, g_wp_bmph);
    }

    case SYS_FB_FLIP:
        v3d_dbl_flip();
        return 0;

    /* ── Power management (Phase 6.2) ───────────────────────────────────── */

    case SYS_POWER_CPUFREQ_GET:
        return (long)cpufreq_get_current_hz();

    case SYS_POWER_CPUFREQ_GOV: {
        long gov = (long)arg0;
        if (gov < 0)
            return (long)cpufreq_get_governor();
        cpufreq_set_governor((int)gov);
        return 0;
    }

    case SYS_POWER_THERMAL:
        return (long)thermal_get_mc();

    case SYS_POWER_DPMS: {
        long cmd = (long)arg0;
        if (cmd == 0) { dpms_force_blank(); return 0; }
        if (cmd == 1) { dpms_force_wake();  return 0; }
        /* cmd == 2: status query */
        return dpms_is_blanked() ? 1L : 0L;
    }

    default:
        kwarn("[SYS] unknown syscall #%lu from PID %lu\n",
              (unsigned long)nr, (unsigned long)task_current_pid());
        return -1;
    }
}
