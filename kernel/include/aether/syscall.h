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

/* Process management */
#define SYS_EXIT         0    /* sys_exit(code)                              */
#define SYS_SCHED_YIELD  3    /* sys_sched_yield()                           */

/* Filesystem / I/O (Phase 3 MVP: only stdout write) */
#define SYS_WRITE        34   /* sys_write(fd, buf, len) → bytes written     */

/* fd 1 = stdout (UART) — the only "file" that exists in Phase 3 */
#define FD_STDOUT  1

#include "aether/exceptions.h"   /* trap_frame_t */

/*
 * syscall_dispatch — called from el0_sync_handler when EC == 0x15 (SVC).
 * Returns the value to place in x0 (return value to user).
 */
long syscall_dispatch(trap_frame_t *frame);

#endif /* AETHER_SYSCALL_H */
