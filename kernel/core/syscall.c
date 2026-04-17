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
#include "aether/printk.h"
#include "aether/scheduler.h"
#include "aether/types.h"
#include "drivers/char/uart_pl011.h"

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

/*
 * sys_write — write bytes to a file descriptor.
 *
 * Phase 3 MVP: only fd=1 (stdout) is implemented — writes to UART.
 * Future: VFS layer will dispatch to the appropriate driver/file.
 *
 * Returns number of bytes written, or -1 on error.
 *
 * Security note: 'buf' is a user-space pointer. In Phase 3.1 we will
 * add copy_from_user() to safely copy data across the privilege boundary.
 * For now, the identity map means kernel can read user pointers directly.
 */
static long do_sys_write(long fd, const char *buf, long len)
{
    if (fd != FD_STDOUT) {
        kwarn("[SYS] sys_write: unsupported fd=%ld\n", fd);
        return -1;
    }
    if (!buf || len <= 0) return 0;

    /* Cap at 4KB to prevent a buggy user program from stalling the kernel */
    if (len > 4096) len = 4096;

    for (long i = 0; i < len; i++)
        uart_putc(buf[i]);

    return len;
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
        do_sys_exit((long)arg0);   /* noreturn */
        return 0;                  /* unreachable, but silences warning */

    case SYS_WRITE:
        return do_sys_write((long)arg0, (const char *)arg1, (long)arg2);

    case SYS_SCHED_YIELD:
        return do_sys_sched_yield();

    default:
        kwarn("[SYS] unknown syscall #%lu from PID %lu\n",
              (unsigned long)nr, (unsigned long)task_current_pid());
        return -1;   /* ENOSYS */
    }
}
