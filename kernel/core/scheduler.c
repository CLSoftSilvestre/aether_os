/*
 * AetherOS — Round-Robin Cooperative Scheduler
 * File: kernel/core/scheduler.c
 *
 * Manages a fixed-size array of tasks and switches between them when
 * a task calls task_yield() or task_sleep().
 *
 * Scheduling policy: round-robin (circular scan from current task).
 *   - Pick the next READY task after the current one.
 *   - Skip SLEEPING tasks unless their wake_tick has passed.
 *   - If no other task is ready, stay in the current task (idle behaviour).
 *
 * This file handles the high-level policy (which task runs next).
 * The low-level mechanism (register save/restore) is in context_switch.S.
 */

#include "aether/scheduler.h"
#include "aether/mm.h"
#include "aether/printk.h"
#include "drivers/timer/arm_timer.h"

/* Task table — statically allocated, no heap dependency */
static task_t g_tasks[MAX_TASKS];
static u32    g_num_tasks   = 0;
static u32    g_current_idx = 0;   /* index into g_tasks[] of running task */

/* ── Internal helpers ───────────────────────────────────────────────────── */

static task_t *current_task(void)
{
    return &g_tasks[g_current_idx];
}

/*
 * find_next — scan for the next runnable task.
 *
 * Starts searching AFTER the current task (round-robin).
 * Wakes sleeping tasks whose wake_tick has passed.
 * Returns the index of the next task to run, or g_current_idx if none found.
 */
static u32 find_next(void)
{
    u64 now = timer_get_ticks();

    for (u32 i = 1; i < g_num_tasks; i++) {
        u32 idx = (g_current_idx + i) % g_num_tasks;
        task_t *t = &g_tasks[idx];

        /* Wake sleeping task if its time has come */
        if (t->state == TASK_SLEEPING && now >= t->wake_tick)
            t->state = TASK_READY;

        if (t->state == TASK_READY)
            return idx;
    }

    return g_current_idx;   /* no other task ready — stay current */
}

/* ── Public API ─────────────────────────────────────────────────────────── */

void scheduler_init(void)
{
    for (u32 i = 0; i < MAX_TASKS; i++)
        g_tasks[i].state = TASK_UNUSED;

    g_num_tasks   = 0;
    g_current_idx = 0;

    kinfo("Scheduler: initialised (max %d tasks)\n", MAX_TASKS);
}

/*
 * scheduler_add_idle — register the current execution context as task 0 (idle).
 *
 * Called from kernel_main. The idle task's context is "whatever the CPU
 * currently is" — we don't pre-set its registers because it's already running.
 * When another task yields back to idle, context_switch() will restore the
 * cpu_context_t that was saved when idle called task_yield().
 */
void scheduler_add_idle(void)
{
    task_t *idle = &g_tasks[0];
    idle->pid   = 0;
    idle->state = TASK_RUNNING;
    idle->name  = "idle";
    idle->stack_phys = 0;   /* uses boot stack — not PMM-managed */
    /* ctx is zeroed; it will be filled on the first call to task_yield() */

    g_num_tasks   = 1;
    g_current_idx = 0;
}

int task_create(void (*entry)(void), const char *name)
{
    if (g_num_tasks >= MAX_TASKS) {
        kerror("Scheduler: too many tasks (max %d)\n", MAX_TASKS);
        return -1;
    }

    task_t *t = &g_tasks[g_num_tasks];

    /* Allocate a stack for this task */
    uintptr_t stack_phys = pmm_alloc_pages(TASK_STACK_PAGES);
    if (!stack_phys) {
        kerror("Scheduler: cannot allocate stack for '%s'\n", name);
        return -1;
    }

    /*
     * Stack top: physical address of top of stack region.
     * Stack grows downward — we start SP at the highest address.
     * AArch64 ABI requires SP to be 16-byte aligned.
     */
    uintptr_t stack_top = stack_phys + TASK_STACK_PAGES * PMM_PAGE_SIZE;

    /*
     * Initialise cpu_context_t so that when context_switch() loads this
     * task and executes 'ret', it jumps to the entry function.
     *
     * x19–x29 = 0 (a new task starts with no callee-saved state)
     * x30     = entry function address (ret → entry())
     * sp      = top of stack (AArch64: full-descending)
     */
    t->ctx.x19 = t->ctx.x20 = t->ctx.x21 = t->ctx.x22 = 0;
    t->ctx.x23 = t->ctx.x24 = t->ctx.x25 = t->ctx.x26 = 0;
    t->ctx.x27 = t->ctx.x28 = t->ctx.x29 = 0;
    t->ctx.x30 = (u64)entry;      /* ret will jump here on first schedule */
    t->ctx.sp  = stack_top;

    t->pid        = g_num_tasks;
    t->state      = TASK_READY;
    t->wake_tick  = 0;
    t->name       = name;
    t->stack_phys = stack_phys;

    kinfo("Scheduler: created task[%lu] '%s' entry=%p stack=%p–%p\n",
          (unsigned long)g_num_tasks, name,
          (void *)entry,
          (void *)stack_phys,
          (void *)stack_top);

    g_num_tasks++;
    return 0;
}

void task_yield(void)
{
    u32    from_idx = g_current_idx;
    u32    to_idx   = find_next();

    if (from_idx == to_idx)
        return;   /* nothing else to run */

    task_t *from = &g_tasks[from_idx];
    task_t *to   = &g_tasks[to_idx];

    /* Update states */
    if (from->state == TASK_RUNNING)
        from->state = TASK_READY;
    to->state = TASK_RUNNING;

    g_current_idx = to_idx;

    /*
     * context_switch() saves from's callee-saved registers into from->ctx
     * and loads to's callee-saved registers from to->ctx, then ret.
     *
     * From 'from' task's perspective: context_switch() is just a slow
     * function call that returns when 'from' is scheduled again.
     *
     * From 'to' task's perspective: it resumes wherever it last called
     * context_switch() (or starts at its entry function if new).
     */
    context_switch(&from->ctx, &to->ctx);
}

void task_sleep(u64 ticks)
{
    task_t *t = current_task();
    t->state     = TASK_SLEEPING;
    t->wake_tick = timer_get_ticks() + ticks;
    task_yield();   /* give up CPU; find_next() will wake us when ready */
}

__attribute__((noreturn))
void task_exit(void)
{
    task_t *t = current_task();
    kinfo("Scheduler: task[%lu] '%s' exited\n",
          (unsigned long)t->pid, t->name);
    t->state = TASK_DEAD;
    task_yield();   /* should not return since this task is dead */

    /* Safety net — if somehow we get here */
    for (;;)
        __asm__ volatile("wfi");
}

u32 task_current_pid(void)
{
    return g_tasks[g_current_idx].pid;
}

void scheduler_print_tasks(void)
{
    static const char *state_names[] = {
        [TASK_UNUSED]   = "UNUSED",
        [TASK_READY]    = "READY",
        [TASK_RUNNING]  = "RUNNING",
        [TASK_SLEEPING] = "SLEEPING",
        [TASK_DEAD]     = "DEAD",
    };

    kinfo("─── Task List ──────────────────────────\n");
    for (u32 i = 0; i < g_num_tasks; i++) {
        task_t *t = &g_tasks[i];
        kinfo("  [%lu] %s  (%s)\n",
              (unsigned long)t->pid, t->name, state_names[t->state]);
    }
    kinfo("────────────────────────────────────────\n");
}
