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
#include "drivers/char/uart_pl011.h"
#include "drivers/timer/arm_timer.h"
#include "drivers/video/fb.h"
#include "drivers/video/font.h"
#include "drivers/video/fb_console.h"
#include "drivers/video/cursor.h"
#include "drivers/input/pl050_kbd.h"
#include "drivers/input/pl050_mouse.h"
#include "drivers/input/keycodes.h"

/* ── Individual syscall implementations ─────────────────────────────────── */

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

static long do_sys_waitpid(long pid, int *status)
{
    return (long)task_waitpid((u32)pid, status);
}

static long do_sys_getpid(void)
{
    return (long)task_current_pid();
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
    return (long)mouse_get_event();
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

    case SYS_WAITPID:
        return do_sys_waitpid((long)arg0, (int *)arg1);

    case SYS_GETPID:
        return do_sys_getpid();

    case SYS_PIPE:
        return do_sys_pipe((int *)arg0);

    case SYS_DUP2:
        return do_sys_dup2((long)arg0, (long)arg1);

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

    case SYS_FB_FILL:
        return do_sys_fb_fill(arg0, arg1, arg2);

    case SYS_FB_CHAR:
        return do_sys_fb_char(arg0, arg1, arg2);

    case SYS_GET_TICKS:
        return do_sys_get_ticks();

    case SYS_FB_CLAIM:
        return do_sys_fb_claim();

    default:
        kwarn("[SYS] unknown syscall #%lu from PID %lu\n",
              (unsigned long)nr, (unsigned long)task_current_pid());
        return -1;
    }
}
