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
#include "aether/pipe.h"
#include "aether/vmm.h"
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

/* ── Internal: allocate a task slot and initialise its kernel stack ─ */

static task_t *alloc_task(void (*entry_fn)(void), const char *name)
{
    if (g_num_tasks >= MAX_TASKS) {
        kerror("Scheduler: too many tasks (max %d)\n", MAX_TASKS);
        return NULL;
    }

    uintptr_t stack_phys = pmm_alloc_pages(TASK_STACK_PAGES);
    if (!stack_phys) {
        kerror("Scheduler: cannot allocate stack for '%s'\n", name);
        return NULL;
    }

    uintptr_t stack_top = stack_phys + TASK_STACK_PAGES * PMM_PAGE_SIZE;

    task_t *t = &g_tasks[g_num_tasks];

    t->ctx.x19 = t->ctx.x20 = t->ctx.x21 = t->ctx.x22 = 0;
    t->ctx.x23 = t->ctx.x24 = t->ctx.x25 = t->ctx.x26 = 0;
    t->ctx.x27 = t->ctx.x28 = t->ctx.x29 = 0;
    t->ctx.x30 = (u64)entry_fn;
    t->ctx.sp  = stack_top;

    t->pid              = g_num_tasks;
    t->ppid             = 0;
    t->state            = TASK_READY;
    t->exit_code        = 0;
    t->wait_pid         = 0;
    t->wake_tick        = 0;
    t->name             = name;
    t->stack_phys       = stack_phys;
    t->el0_entry        = 0;
    t->el0_sp           = 0;
    t->l1_table_phys    = 0;
    t->user_code_phys   = 0;
    t->user_code_pages  = 0;
    t->user_stack_phys  = 0;
    t->user_stack_pages = 0;

    for (u32 i = 0; i < PROC_MAX_FD; i++) {
        t->fd_table[i].type     = FD_TYPE_CLOSED;
        t->fd_table[i].pipe_idx = 0;
    }

    return t;
}

static void init_uart_fds(task_t *t)
{
    t->fd_table[0].type = FD_TYPE_UART;   /* stdin  */
    t->fd_table[1].type = FD_TYPE_UART;   /* stdout */
    t->fd_table[2].type = FD_TYPE_UART;   /* stderr */
}

/* Wake any task in TASK_WAITING state that is waiting for 'child_pid' */
static void wake_waiting_parent(u32 child_pid)
{
    for (u32 i = 0; i < g_num_tasks; i++) {
        task_t *t = &g_tasks[i];
        if (t->state == TASK_WAITING && t->wait_pid == child_pid) {
            t->state    = TASK_READY;
            t->wait_pid = 0;
            break;
        }
    }
}

int task_create(void (*entry)(void), const char *name)
{
    task_t *t = alloc_task(entry, name);
    if (!t) return -1;

    kinfo("Scheduler: created task[%lu] '%s' entry=%p stack=%p\n",
          (unsigned long)g_num_tasks, name,
          (void *)entry, (void *)t->stack_phys);

    g_num_tasks++;
    return 0;
}

int task_create_user(uintptr_t el0_entry, uintptr_t el0_sp,
                     const char *name, void (*trampoline)(void))
{
    task_t *t = alloc_task(trampoline, name);
    if (!t) return -1;

    t->el0_entry = el0_entry;
    t->el0_sp    = el0_sp;
    init_uart_fds(t);

    kinfo("Scheduler: created user task[%lu] '%s' el0_entry=%p\n",
          (unsigned long)g_num_tasks, name, (void *)el0_entry);

    g_num_tasks++;
    return 0;
}

int task_create_isolated(uintptr_t el0_entry, uintptr_t el0_sp,
                         const char *name, void (*trampoline)(void),
                         uintptr_t l1_phys, u32 ppid,
                         uintptr_t user_code_phys, u32 user_code_pages,
                         uintptr_t user_stack_phys, u32 user_stack_pages,
                         u32 *pid_out)
{
    task_t *t = alloc_task(trampoline, name);
    if (!t) return -1;

    t->el0_entry        = el0_entry;
    t->el0_sp           = el0_sp;
    t->l1_table_phys    = l1_phys;
    t->ppid             = ppid;
    t->user_code_phys   = user_code_phys;
    t->user_code_pages  = user_code_pages;
    t->user_stack_phys  = user_stack_phys;
    t->user_stack_pages = user_stack_pages;

    /* Inherit parent's fd_table */
    task_t *parent = NULL;
    for (u32 i = 0; i < g_num_tasks; i++) {
        if (g_tasks[i].pid == ppid) { parent = &g_tasks[i]; break; }
    }
    if (parent) {
        for (u32 i = 0; i < PROC_MAX_FD; i++)
            t->fd_table[i] = parent->fd_table[i];
    } else {
        init_uart_fds(t);
    }

    if (pid_out) *pid_out = t->pid;

    kinfo("Scheduler: created isolated task[%lu] '%s' ppid=%lu l1=%p\n",
          (unsigned long)g_num_tasks, name,
          (unsigned long)ppid, (void *)l1_phys);

    g_num_tasks++;
    return 0;
}

void task_get_user_regs(uintptr_t *entry_out, uintptr_t *sp_out,
                        uintptr_t *l1_phys_out)
{
    task_t *t = &g_tasks[g_current_idx];
    if (entry_out)   *entry_out   = t->el0_entry;
    if (sp_out)      *sp_out      = t->el0_sp;
    if (l1_phys_out) *l1_phys_out = t->l1_table_phys;
}

void task_yield(void)
{
    u32    from_idx = g_current_idx;
    u32    to_idx   = find_next();

    if (from_idx == to_idx)
        return;

    task_t *from = &g_tasks[from_idx];
    task_t *to   = &g_tasks[to_idx];

    if (from->state == TASK_RUNNING)
        from->state = TASK_READY;
    to->state = TASK_RUNNING;

    g_current_idx = to_idx;

    context_switch(&from->ctx, &to->ctx);

    /*
     * After context_switch returns we are the *resumed* task (which is
     * 'from' in the call above, but now g_current_idx points to us).
     * Switch TTBR0_EL1 to match the task now executing.
     */
    vmm_switch_user_pt(g_tasks[g_current_idx].l1_table_phys);
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
    kinfo("Scheduler: task[%lu] '%s' exited (ppid=%lu)\n",
          (unsigned long)t->pid, t->name, (unsigned long)t->ppid);

    /* Switch back to global PT before freeing process-specific tables.
     * This closes the window where we'd hold TTBR0 pointing to freed pages. */
    if (t->l1_table_phys) {
        vmm_switch_user_pt(0);
        vmm_free_process_pt(t->l1_table_phys);
        t->l1_table_phys = 0;
    }
    if (t->user_code_phys) {
        for (u32 i = 0; i < t->user_code_pages; i++)
            pmm_free_page(t->user_code_phys + (uintptr_t)i * PMM_PAGE_SIZE);
        t->user_code_phys = 0;
    }
    if (t->user_stack_phys) {
        for (u32 i = 0; i < t->user_stack_pages; i++)
            pmm_free_page(t->user_stack_phys + (uintptr_t)i * PMM_PAGE_SIZE);
        t->user_stack_phys = 0;
    }

    if (t->ppid) {
        t->state = TASK_ZOMBIE;     /* parent may call waitpid */
        wake_waiting_parent(t->pid);
    } else {
        t->state = TASK_DEAD;
    }

    task_yield();
    for (;;) __asm__ volatile("wfi");
}

int task_waitpid(u32 pid, int *status)
{
    task_t *cur = current_task();

    task_t *child = NULL;
    for (u32 i = 0; i < g_num_tasks; i++) {
        if (g_tasks[i].pid == pid && g_tasks[i].ppid == cur->pid) {
            child = &g_tasks[i];
            break;
        }
    }
    if (!child) return -1;

    while (child->state != TASK_ZOMBIE) {
        cur->state    = TASK_WAITING;
        cur->wait_pid = pid;
        task_yield();
    }

    if (status) *status = child->exit_code;
    child->state = TASK_DEAD;
    return (int)pid;
}

/*
 * task_waitpid_nb — non-blocking waitpid.
 * Returns child PID if zombie (and reaps it), 0 if still running, -1 if not found.
 */
int task_waitpid_nb(u32 pid, int *status)
{
    task_t *cur = current_task();

    task_t *child = NULL;
    for (u32 i = 0; i < g_num_tasks; i++) {
        if (g_tasks[i].pid == pid && g_tasks[i].ppid == cur->pid) {
            child = &g_tasks[i];
            break;
        }
    }
    if (!child) return -1;
    if (child->state != TASK_ZOMBIE) return 0;

    if (status) *status = child->exit_code;
    child->state = TASK_DEAD;
    return (int)pid;
}

/*
 * task_kill — forcefully terminate a child process from the parent.
 * Frees the child's resources and marks it ZOMBIE so the parent can reap.
 * Only the child's parent (or PID 1) may call this.
 */
int task_kill(u32 pid, int exit_code)
{
    task_t *cur = current_task();

    task_t *t = NULL;
    for (u32 i = 0; i < g_num_tasks; i++) {
        if (g_tasks[i].pid == pid) { t = &g_tasks[i]; break; }
    }
    if (!t) return -1;
    if (t->ppid != cur->pid && cur->pid != 1) return -1;
    if (t->state == TASK_ZOMBIE || t->state == TASK_DEAD) return 0;

    kinfo("Scheduler: task_kill pid=%lu by pid=%lu\n",
          (unsigned long)pid, (unsigned long)cur->pid);

    /* Free process page tables (safe: we're on the caller's PT, not the target's) */
    if (t->l1_table_phys) {
        vmm_free_process_pt(t->l1_table_phys);
        t->l1_table_phys = 0;
    }
    if (t->user_code_phys) {
        for (u32 i = 0; i < t->user_code_pages; i++)
            pmm_free_page(t->user_code_phys + (uintptr_t)i * PMM_PAGE_SIZE);
        t->user_code_phys = 0;
    }
    if (t->user_stack_phys) {
        for (u32 i = 0; i < t->user_stack_pages; i++)
            pmm_free_page(t->user_stack_phys + (uintptr_t)i * PMM_PAGE_SIZE);
        t->user_stack_phys = 0;
    }

    t->exit_code = exit_code;
    if (t->ppid) {
        t->state = TASK_ZOMBIE;
        wake_waiting_parent(t->pid);
    } else {
        t->state = TASK_DEAD;
    }
    return 0;
}

u32 task_current_pid(void)
{
    return g_tasks[g_current_idx].pid;
}

const char *task_current_name(void)
{
    const char *n = g_tasks[g_current_idx].name;
    return n ? n : "?";
}

fd_entry_t *task_get_fd(u32 fd)
{
    if (fd >= PROC_MAX_FD) return NULL;
    return &g_tasks[g_current_idx].fd_table[fd];
}

int task_alloc_fd(u8 type, u16 pipe_idx)
{
    task_t *t = current_task();
    for (u32 i = 0; i < PROC_MAX_FD; i++) {
        if (t->fd_table[i].type == FD_TYPE_CLOSED) {
            t->fd_table[i].type     = type;
            t->fd_table[i].pipe_idx = pipe_idx;
            return (int)i;
        }
    }
    return -1;
}

void task_close_fd(u32 fd)
{
    if (fd >= PROC_MAX_FD) return;
    task_t *t = current_task();
    fd_entry_t *e = &t->fd_table[fd];
    if (e->type == FD_TYPE_PIPE_R) pipe_close_read((int)e->pipe_idx);
    if (e->type == FD_TYPE_PIPE_W) pipe_close_write((int)e->pipe_idx);
    e->type     = FD_TYPE_CLOSED;
    e->pipe_idx = 0;
}

long task_dup2_fd(u32 oldfd, u32 newfd)
{
    if (oldfd >= PROC_MAX_FD || newfd >= PROC_MAX_FD) return -1;
    task_t *t = current_task();
    if (t->fd_table[oldfd].type == FD_TYPE_CLOSED) return -1;

    /* Close newfd's existing pipe end */
    fd_entry_t *ne = &t->fd_table[newfd];
    if (ne->type == FD_TYPE_PIPE_R) pipe_close_read((int)ne->pipe_idx);
    if (ne->type == FD_TYPE_PIPE_W) pipe_close_write((int)ne->pipe_idx);

    *ne = t->fd_table[oldfd];
    return (long)newfd;
}

void scheduler_print_tasks(void)
{
    static const char *state_names[] = {
        [TASK_UNUSED]   = "UNUSED",
        [TASK_READY]    = "READY",
        [TASK_RUNNING]  = "RUNNING",
        [TASK_SLEEPING] = "SLEEPING",
        [TASK_DEAD]     = "DEAD",
        [TASK_ZOMBIE]   = "ZOMBIE",
        [TASK_WAITING]  = "WAITING",
    };

    kinfo("─── Task List ──────────────────────────\n");
    for (u32 i = 0; i < g_num_tasks; i++) {
        task_t *t = &g_tasks[i];
        kinfo("  [%lu] %s  (%s)\n",
              (unsigned long)t->pid, t->name, state_names[t->state]);
    }
    kinfo("────────────────────────────────────────\n");
}
