#ifndef AETHER_SYSCALL_H
#define AETHER_SYSCALL_H

/*
 * AetherOS Syscall Interface — Phase 3 MVP
 *
 * Syscalls are the only way user-space code (EL0) can request kernel
 * services. The mechanism:
 *
 *   User code:                  Kernel (exception handler):
 *   ──────────────────────      ─────────────────────────────────────
 *   x8  = syscall number        ESR_EL1.EC = 0x15 (SVC from AArch64)
 *   x0  = arg0                  el0_sync_handler sees frame->x[8]
 *   x1  = arg1                  dispatches to syscall table
 *   x2  = arg2                  result goes into frame->x[0]
 *   svc #0          ──────────►  eret back to user with result in x0
 *
 * In x86 terms: SVC #0 is like INT 0x80 in Linux i386 ABI,
 * or SYSCALL in x86-64. x8 as the syscall number register matches
 * the Linux AArch64 ABI (compatible if we ever port apps).
 *
 * ── Syscall numbers (Phase 3 MVP) ──────────────────────────────────
 *
 * These match the AetherOS spec (see technical_specifications.md):
 *   0–15:   Process Management
 *   16–31:  Memory
 *   32–63:  Filesystem
 *   64–79:  IPC
 *   80–95:  Device
 */

/* Process management (Phase 4.3 / 4.4) */
#define SYS_EXIT         0    /* sys_exit(code)                              */
#define SYS_SPAWN        1    /* sys_spawn(path) → child PID or -1           */
#define SYS_SCHED_YIELD  3    /* sys_sched_yield()                           */
#define SYS_WAITPID      4    /* sys_waitpid(pid, *status) → pid or -1       */
#define SYS_GETPID       5    /* sys_getpid() → current PID                  */
#define SYS_SLEEP_TICKS  6    /* sys_sleep(ticks) — sleep N 100Hz ticks      */

/* IPC (Phase 4.3) */
#define SYS_PIPE        22    /* sys_pipe(fds[2]) → 0 or -1                  */
#define SYS_DUP2        24    /* sys_dup2(oldfd, newfd) → newfd or -1        */

/* Filesystem / I/O */
#define SYS_READ         63   /* sys_read(fd, buf, len)  → bytes read        */
#define SYS_WRITE        34   /* sys_write(fd, buf, len) → bytes written     */

/* AetherOS-specific extensions */
#define SYS_INITRD_LS   500   /* sys_initrd_ls(buf, len) → bytes written     */
#define SYS_INITRD_READ 501   /* sys_initrd_read(name, buf, len) → bytes     */
#define SYS_PMM_STATS   502   /* → (free_pages << 32) | total_pages          */

/* Graphics syscalls (Phase 4.1) — arg packing documented in syscall.c */
#define SYS_FB_FILL     601   /* fill rect:  (x<<32|y, w<<32|h, color)       */
#define SYS_FB_CHAR     602   /* draw char:  (x<<32|y, ch<<32|fg, bg)        */
#define SYS_GET_TICKS   603   /* → u64 100Hz tick count since boot            */
#define SYS_FB_CLAIM    604   /* user owns FB: disable kernel fb_console      */

/* File descriptor numbers */
#define FD_STDIN   0
#define FD_STDOUT  1

#include "aether/exceptions.h"   /* trap_frame_t */

/*
 * syscall_dispatch — called from el0_sync_handler when EC == 0x15 (SVC).
 * Returns the value to place in x0 (return value to user).
 */
long syscall_dispatch(trap_frame_t *frame);

#endif /* AETHER_SYSCALL_H */
