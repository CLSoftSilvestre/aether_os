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
#define TASK_DEAD     4    /* exited */

#define MAX_TASKS     16
#define TASK_STACK_PAGES 2   /* 8KB stack per task (2 × 4KB) */

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
    cpu_context_t ctx;         /* saved CPU state — MUST be first field */
    u32           pid;         /* process ID (1-based) */
    int           state;       /* TASK_* constant above */
    u64           wake_tick;   /* tick counter when task should wake (for sleep) */
    const char   *name;        /* human-readable name for debugging */
    uintptr_t     stack_phys;  /* physical address of stack allocation */
    /* Phase 3.1: user-process fields (only valid for tasks created via
     * task_create_user; kernel tasks leave these as 0) */
    uintptr_t     el0_entry;   /* EL0 entry point (user-space VA) */
    uintptr_t     el0_sp;      /* EL0 initial stack pointer (SP_EL0) */
} task_t;

/* ── Public API ─────────────────────────────────────────────────────── */

/* Initialise scheduler — must be called before task_create() */
void scheduler_init(void);

/* Register the current boot context as the idle task (call once from kernel_main) */
void scheduler_add_idle(void);

/*
 * task_create — create a new kernel thread.
 * entry: function to run in the new task
 * name:  display name for debugging
 * Returns 0 on success, -1 if too many tasks.
 */
int task_create(void (*entry)(void), const char *name);

/*
 * task_create_user — create a kernel task that will launch a user (EL0) process.
 *
 * el0_entry:  virtual address of the EL0 entry point (from ELF e_entry)
 * el0_sp:     initial SP_EL0 (user stack top)
 * name:       display name for debugging
 * trampoline: kernel-mode function that calls launch_el0(el0_entry, el0_sp)
 *             (defined in process.c to keep vmm.h out of scheduler.c)
 *
 * Returns 0 on success, -1 on error.
 */
int task_create_user(uintptr_t el0_entry, uintptr_t el0_sp,
                     const char *name, void (*trampoline)(void));

/*
 * task_get_user_regs — retrieve the EL0 entry/stack of the current task.
 * Called from user_task_trampoline() in process.c just before eret.
 */
void task_get_user_regs(uintptr_t *entry_out, uintptr_t *sp_out);

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

/* Print all task states (for debugging) */
void scheduler_print_tasks(void);

/* Context switch (implemented in context_switch.S) */
void context_switch(cpu_context_t *from, cpu_context_t *to);

#endif /* AETHER_SCHEDULER_H */
