#ifndef AETHER_SCHEDULER_H
#define AETHER_SCHEDULER_H

/*
 * AetherOS — Round-Robin Scheduler
 *
 * Phase 2 implements cooperative scheduling:
 *   - Tasks voluntarily yield the CPU by calling task_yield()
 *   - No preemption yet — a task runs until it yields or sleeps
 *   - Preemptive scheduling (timer-driven) comes in Phase 3
 *
 * Why cooperative first?
 *   Preemptive scheduling requires saving the FULL CPU state (including
 *   the exception-level frame) from within an IRQ handler — it is correct
 *   but complex to implement safely. Cooperative scheduling uses a simple
 *   function-call-level context switch (only callee-saved registers), which
 *   is much easier to reason about and debug.
 *
 * Context switch (AArch64):
 *   Only callee-saved registers need to be saved/restored across a function
 *   call. These are: x19–x28, x29 (frame pointer), x30 (link register), sp.
 *   The caller-saved registers (x0–x18) are the task's responsibility.
 *   (Same concept as x86: ebx/esi/edi are callee-saved across CALL/RET.)
 */

#include "aether/types.h"

/* ── Task states ─────────────────────────────────────────────────────── */
#define TASK_UNUSED   0
#define TASK_READY    1    /* runnable, waiting for CPU */
#define TASK_RUNNING  2    /* currently executing */
#define TASK_SLEEPING 3    /* waiting for a timer event */
#define TASK_DEAD     4    /* exited (no parent waiting) */
#define TASK_ZOMBIE   5    /* exited, waiting for parent to collect */
#define TASK_WAITING  6    /* blocked in task_waitpid() */

#define MAX_TASKS        32  /* raised from 16 for multi-process support */
#define TASK_STACK_PAGES 2   /* 8KB kernel stack per task (2 × 4KB) */

/* ── Per-process file descriptor table ───────────────────────────────── */
#define PROC_MAX_FD   8

#define FD_TYPE_CLOSED  0   /* slot unused */
#define FD_TYPE_UART    1   /* stdin / stdout / stderr */
#define FD_TYPE_PIPE_R  2   /* pipe read end  */
#define FD_TYPE_PIPE_W  3   /* pipe write end */

typedef struct {
    u8  type;       /* FD_TYPE_* */
    u16 pipe_idx;   /* pipe index, valid for PIPE_R / PIPE_W */
} fd_entry_t;

/*
 * cpu_context_t — saved callee-preserved registers.
 *
 * Layout MUST match context_switch.S exactly.
 * Offsets (used in ASM):
 *   x19 @  0, x20 @  8, x21 @ 16, x22 @ 24
 *   x23 @ 32, x24 @ 40, x25 @ 48, x26 @ 56
 *   x27 @ 64, x28 @ 72, x29 @ 80, x30 @ 88
 *   sp  @ 96
 */
typedef struct {
    u64 x19, x20, x21, x22;   /* callee-saved general registers */
    u64 x23, x24, x25, x26;
    u64 x27, x28;
    u64 x29;                   /* frame pointer */
    u64 x30;                   /* link register — next PC after ret */
    u64 sp;                    /* stack pointer */
} cpu_context_t;               /* total: 13 × 8 = 104 bytes */

/*
 * task_t — task control block (TCB)
 *
 * Every task (kernel thread) has one of these describing its identity
 * and saved CPU state. Analogous to the "process descriptor" in Linux
 * or the "TCB" in most RTOS literature.
 */
typedef struct {
    cpu_context_t ctx;              /* saved CPU state — MUST be first field */
    u32           pid;              /* process ID (1-based) */
    u32           ppid;             /* parent PID (0 = no parent) */
    int           state;            /* TASK_* constant above */
    int           exit_code;        /* valid when TASK_ZOMBIE */
    u32           wait_pid;         /* PID being waited on (TASK_WAITING) */
    u64           wake_tick;        /* tick when TASK_SLEEPING wakes */
    const char   *name;
    uintptr_t     stack_phys;       /* physical address of kernel stack */
    /* User-process fields (0 for kernel tasks) */
    uintptr_t     el0_entry;        /* EL0 entry point virtual address */
    uintptr_t     el0_sp;           /* EL0 initial stack pointer */
    uintptr_t     l1_table_phys;    /* per-process L1 page table PA (0 = global) */
    uintptr_t     user_code_phys;   /* physical base of ELF pages (0 = identity) */
    u32           user_code_pages;  /* page count of ELF allocation */
    uintptr_t     user_stack_phys;  /* physical base of user stack pages */
    u32           user_stack_pages;
    fd_entry_t    fd_table[PROC_MAX_FD];
} task_t;

/* ── Public API ─────────────────────────────────────────────────────── */

void scheduler_init(void);
void scheduler_add_idle(void);

int task_create(void (*entry)(void), const char *name);

/*
 * task_create_user — create a task for an EL0 process using the global
 * page table (init only).  Returns 0 on success, -1 on error.
 */
int task_create_user(uintptr_t el0_entry, uintptr_t el0_sp,
                     const char *name, void (*trampoline)(void));

/*
 * task_create_isolated — create a task for a spawned child process with
 * its own per-process page table.
 *
 * l1_phys: physical address of the child's L1 table (from vmm_create_process_pt)
 * ppid:    parent PID
 * pid_out: receives the new task's PID
 */
int task_create_isolated(uintptr_t el0_entry, uintptr_t el0_sp,
                         const char *name, void (*trampoline)(void),
                         uintptr_t l1_phys, u32 ppid,
                         uintptr_t user_code_phys, u32 user_code_pages,
                         uintptr_t user_stack_phys, u32 user_stack_pages,
                         u32 *pid_out);

/*
 * task_get_user_regs — retrieve EL0 entry, stack, and page-table PA of
 * the current task.  Called from the user-task trampoline before eret.
 */
void task_get_user_regs(uintptr_t *entry_out, uintptr_t *sp_out,
                        uintptr_t *l1_phys_out);

/*
 * task_waitpid — block until child PID exits or is already a zombie.
 * Writes exit code to *status (may be NULL).
 * Returns child PID on success, -1 if no such child.
 */
int task_waitpid(u32 pid, int *status);

/* ── fd_table helpers (for syscall.c) ──────────────────────────────── */

/* Return pointer to fd slot for the current task (NULL if out of range). */
fd_entry_t *task_get_fd(u32 fd);

/* Find a free fd slot, fill it, return the fd index or -1. */
int task_alloc_fd(u8 type, u16 pipe_idx);

/* Close (free) an fd slot; closes the pipe end if applicable. */
void task_close_fd(u32 fd);

/* Copy fd[oldfd] → fd[newfd]; close newfd's old pipe end first. */
long task_dup2_fd(u32 oldfd, u32 newfd);

/*
 * task_yield — voluntarily surrender the CPU to the next ready task.
 * Returns when the scheduler gives this task the CPU again.
 * Equivalent to sched_yield() in POSIX.
 */
void task_yield(void);

/*
 * task_sleep — sleep for N timer ticks (cooperative, polled on yield).
 * At 100Hz, 100 ticks = 1 second.
 */
void task_sleep(u64 ticks);

/*
 * task_exit — terminate the current task.
 * Does not return.
 */
__attribute__((noreturn)) void task_exit(void);

/* Return PID of currently running task */
u32 task_current_pid(void);

/* Return name of currently running task (never NULL) */
const char *task_current_name(void);

/* Print all task states (for debugging) */
void scheduler_print_tasks(void);

/* Context switch (implemented in context_switch.S) */
void context_switch(cpu_context_t *from, cpu_context_t *to);

#endif /* AETHER_SCHEDULER_H */
