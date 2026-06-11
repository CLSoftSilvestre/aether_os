/*
 * AetherOS — SMP-Aware Round-Robin Scheduler
 * File: kernel/core/scheduler.c
 *
 * Manages a fixed-size array of tasks and switches between them when a task
 * calls task_yield() or task_sleep().
 *
 * SMP model (4-core Pi 5 / QEMU virt)
 * ─────────────────────────────────────
 * Each core maintains its own "currently running task" index:
 *   g_current_idx[core_id]
 *
 * All task-table mutations are protected by g_sched_lock (spinlock).
 * Context switches use context_switch_smp() which releases the spinlock
 * atomically after saving 'from' registers, closing the race window where
 * another core could try to load stale register state.
 *
 * Task selection (find_next)
 * ──────────────────────────
 * A task is eligible for scheduling on core N if:
 *   1. state == TASK_READY  (not RUNNING / SLEEPING / DEAD / …)
 *   2. cpu_affinity & (1 << N)  (task is allowed on this core)
 * The RT picker (sched_rt_find_next) is tried first; if nothing qualifies,
 * a SCHED_NORMAL round-robin scan follows.
 *
 * Idle tasks
 * ──────────
 * Core 0: registered by scheduler_add_idle(), pinned to CPU_MASK_CORE0.
 * Cores 1-3: registered by scheduler_secondary_init(), pinned to their own
 *   core mask.  The secondary idle runs in smp.c:secondary_main()'s loop.
 */

#include "aether/scheduler.h"
#include "aether/sched.h"
#include "aether/spinlock.h"
#include "aether/smp.h"
#include "aether/mm.h"
#include "aether/pipe.h"
#include "aether/vmm.h"
#include "aether/wm.h"
#include "aether/printk.h"
#include "drivers/timer/arm_timer.h"

/* Task table — statically allocated, shared across all cores */
static task_t     g_tasks[MAX_TASKS];
static u32        g_num_tasks              = 0;
static u32        g_current_idx[NUM_CPUS]  = {0};  /* per-core current task index */

/* Scheduler spinlock — must be held for any task-table read-modify-write */
static spinlock_t g_sched_lock = SPINLOCK_INIT;

/* ── Internal helpers ───────────────────────────────────────────────────── */

static task_t *current_task(void)
{
    return &g_tasks[g_current_idx[cpu_id()]];
}

/*
 * find_next — select the best runnable task for the calling core.
 *
 * Called with g_sched_lock held.
 *
 * Priority order:
 *   1. RT tasks (SCHED_FIFO / SCHED_RR) with affinity for this core.
 *   2. Normal round-robin among SCHED_NORMAL tasks with affinity for this core.
 *   3. Stay on the current task (no-op yield).
 *
 * A task is skipped if:
 *   - state != TASK_READY  (RUNNING means it is on another core, SLEEPING is
 *     waiting for a timer, DEAD/ZOMBIE/WAITING are not schedulable)
 *   - cpu_affinity does not include the calling core's bit
 */
static u32 find_next(void)
{
    u32 my_core  = cpu_id();
    u8  my_mask  = (u8)(1u << my_core);
    u64 now      = timer_get_ticks();

    /* Wake sleeping tasks whose timer has expired */
    for (u32 i = 0; i < g_num_tasks; i++) {
        task_t *t = &g_tasks[i];
        if (t->state == TASK_SLEEPING && now >= t->wake_tick)
            t->state = TASK_READY;
    }

    /* RT tasks first (sched_rt_find_next also checks affinity now) */
    int rt_idx = sched_rt_find_next();
    if (rt_idx >= 0)
        return (u32)rt_idx;

    /* Normal round-robin with affinity filter */
    u32 cur = g_current_idx[my_core];
    for (u32 i = 1; i < g_num_tasks; i++) {
        u32    idx = (cur + i) % g_num_tasks;
        task_t *t  = &g_tasks[idx];
        if (t->state == TASK_READY &&
            t->sched_policy == SCHED_NORMAL &&
            (t->cpu_affinity & my_mask))
            return idx;
    }

    return cur;   /* stay: nothing else runnable on this core */
}

/* Phase 8.0 — expose task table for RT scheduler (sched_rt.c) */
task_t *task_get_table(u32 *count_out)
{
    if (count_out)
        *count_out = g_num_tasks;
    return g_tasks;
}

/* ── Public API ─────────────────────────────────────────────────────────── */

void scheduler_init(void)
{
    for (u32 i = 0; i < MAX_TASKS; i++)
        g_tasks[i].state = TASK_UNUSED;

    g_num_tasks = 0;
    for (u32 c = 0; c < NUM_CPUS; c++)
        g_current_idx[c] = 0;

    sched_rt_init();
    kinfo("Scheduler: initialised (max %d tasks, %d cores, RT enabled)\n",
          MAX_TASKS, NUM_CPUS);
}

/*
 * scheduler_add_idle — register the current core 0 context as task 0 (idle).
 * Pinned to CPU_MASK_CORE0 so secondary cores do not schedule it.
 */
void scheduler_add_idle(void)
{
    task_t *idle = &g_tasks[0];
    idle->pid          = 0;
    idle->state        = TASK_RUNNING;
    idle->cpu_ticks    = 0;
    idle->stack_phys   = 0;   /* uses boot stack — not PMM-managed */
    idle->sched_policy = SCHED_NORMAL;
    idle->rt_priority  = 0;
    idle->cpu_affinity = CPU_MASK_CORE0;   /* pinned: only runs on core 0 */
    idle->mlocked      = 0;
    for (u32 i = 0; i < PROC_MAX_FD; i++)
        idle->fd_table[i].type = FD_TYPE_CLOSED;
    { const char *s = "idle"; int i = 0;
      while (i < PROC_NAME_MAX - 1 && s[i]) { idle->name[i] = s[i]; i++; }
      idle->name[i] = '\0'; }

    g_num_tasks         = 1;
    g_current_idx[0]    = 0;
}

/*
 * scheduler_secondary_init — called by secondary_main() on cores 1-3.
 *
 * Registers a per-core idle task pinned to core_id.  The task represents
 * the current execution context (the idle loop in secondary_main).
 * Must be called with g_sched_lock NOT held (takes it internally).
 */
void scheduler_secondary_init(u32 core_id)
{
    spin_lock(&g_sched_lock);

    if (g_num_tasks >= MAX_TASKS) {
        spin_unlock(&g_sched_lock);
        kpanic("scheduler_secondary_init: task table full\n");
    }

    task_t *idle = &g_tasks[g_num_tasks];

    /* Zero out the slot cleanly */
    for (u32 i = 0; i < sizeof(task_t) / sizeof(u32); i++)
        ((u32 *)idle)[i] = 0;

    idle->pid          = g_num_tasks;
    idle->state        = TASK_RUNNING;
    idle->cpu_ticks    = 0;
    idle->stack_phys   = 0;   /* secondary stack is in smp.c BSS — not PMM */
    idle->sched_policy = SCHED_NORMAL;
    idle->rt_priority  = 0;
    idle->cpu_affinity = (u8)(1u << core_id);   /* pinned to this core */
    idle->mlocked      = 0;
    idle->bo_va_next   = VMM_USER_BO_BASE;

    for (u32 i = 0; i < PROC_MAX_FD; i++)
        idle->fd_table[i].type = FD_TYPE_CLOSED;

    /* Name: "idle0" … "idle3" */
    const char *prefix = "idle";
    u32 j = 0;
    while (prefix[j] && j < PROC_NAME_MAX - 2u) { idle->name[j] = prefix[j]; j++; }
    idle->name[j++] = (char)('0' + core_id);
    idle->name[j]   = '\0';

    g_current_idx[core_id] = g_num_tasks;
    g_num_tasks++;

    spin_unlock(&g_sched_lock);

    kinfo("Scheduler: core %u idle task registered (PID %u)\n",
          core_id, idle->pid);
}

/* ── Internal: allocate a task slot and initialise its kernel stack ──────── */

static task_t *alloc_task(void (*entry_fn)(void), const char *name)
{
    /* Caller holds g_sched_lock */
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
    t->ctx.daif = 0x80;   /* DAIF.I=1: IRQs masked until trampoline eret */

    t->pid              = g_num_tasks;
    t->ppid             = 0;
    t->state            = TASK_READY;
    t->exit_code        = 0;
    t->wait_pid         = 0;
    t->wake_tick        = 0;
    t->stack_phys       = stack_phys;
    { u32 i = 0;
      if (name) while (i < PROC_NAME_MAX - 1 && name[i]) { t->name[i] = name[i]; i++; }
      t->name[i] = '\0'; }
    t->el0_entry        = 0;
    t->el0_sp           = 0;
    t->l1_table_phys    = 0;
    t->cpu_ticks        = 0;
    t->user_code_phys   = 0;
    t->user_code_pages  = 0;
    t->user_stack_phys  = 0;
    t->user_stack_pages = 0;
    t->bo_va_next       = VMM_USER_BO_BASE;

    for (u32 i = 0; i < PROC_MAX_FD; i++) {
        t->fd_table[i].type     = FD_TYPE_CLOSED;
        t->fd_table[i].pipe_idx = 0;
    }

    /* Phase 8.0: RT defaults — new tasks run on any core */
    t->sched_policy  = SCHED_NORMAL;
    t->rt_priority   = 0;
    t->cpu_affinity  = CPU_MASK_ALL;
    t->mlocked       = 0;

    return t;
}

static void init_uart_fds(task_t *t)
{
    t->fd_table[0].type = FD_TYPE_UART;
    t->fd_table[1].type = FD_TYPE_UART;
    t->fd_table[2].type = FD_TYPE_UART;
}

/* Caller MUST hold g_sched_lock — mutates task state shared across cores. */
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
    spin_lock(&g_sched_lock);
    task_t *t = alloc_task(entry, name);
    if (!t) { spin_unlock(&g_sched_lock); return -1; }

    kinfo("Scheduler: created task[%lu] '%s' entry=%p stack=%p\n",
          (unsigned long)g_num_tasks, name,
          (void *)entry, (void *)t->stack_phys);

    g_num_tasks++;
    spin_unlock(&g_sched_lock);
    return 0;
}

int task_create_user(uintptr_t el0_entry, uintptr_t el0_sp,
                     const char *name, void (*trampoline)(void))
{
    spin_lock(&g_sched_lock);
    task_t *t = alloc_task(trampoline, name);
    if (!t) { spin_unlock(&g_sched_lock); return -1; }

    t->el0_entry = el0_entry;
    t->el0_sp    = el0_sp;
    init_uart_fds(t);

    kinfo("Scheduler: created user task[%lu] '%s' el0_entry=%p\n",
          (unsigned long)g_num_tasks, name, (void *)el0_entry);

    g_num_tasks++;
    spin_unlock(&g_sched_lock);
    return 0;
}

int task_create_isolated(uintptr_t el0_entry, uintptr_t el0_sp,
                         const char *name, void (*trampoline)(void),
                         uintptr_t l1_phys, u32 ppid,
                         uintptr_t user_code_phys, u32 user_code_pages,
                         uintptr_t user_stack_phys, u32 user_stack_pages,
                         u32 argc, uintptr_t argv_user_va,
                         u32 *pid_out)
{
    spin_lock(&g_sched_lock);
    task_t *t = alloc_task(trampoline, name);
    if (!t) { spin_unlock(&g_sched_lock); return -1; }

    t->el0_entry        = el0_entry;
    t->el0_sp           = el0_sp;
    t->el0_argc         = argc;
    t->el0_argv         = argv_user_va;
    t->l1_table_phys    = l1_phys;
    t->ppid             = ppid;
    t->user_code_phys   = user_code_phys;
    t->user_code_pages  = user_code_pages;
    t->user_stack_phys  = user_stack_phys;
    t->user_stack_pages = user_stack_pages;

    task_t *parent = NULL;
    for (u32 i = 0; i < g_num_tasks; i++) {
        if (g_tasks[i].pid == ppid) { parent = &g_tasks[i]; break; }
    }
    if (parent) {
        for (u32 i = 0; i < PROC_MAX_FD; i++) {
            t->fd_table[i] = parent->fd_table[i];
            if (t->fd_table[i].type == FD_TYPE_PIPE_R)
                pipe_open_read((int)t->fd_table[i].pipe_idx);
            if (t->fd_table[i].type == FD_TYPE_PIPE_W)
                pipe_open_write((int)t->fd_table[i].pipe_idx);
        }
    } else {
        init_uart_fds(t);
    }

    if (pid_out) *pid_out = t->pid;

    kinfo("Scheduler: created isolated task[%lu] '%s' ppid=%lu l1=%p\n",
          (unsigned long)g_num_tasks, name,
          (unsigned long)ppid, (void *)l1_phys);

    g_num_tasks++;
    spin_unlock(&g_sched_lock);
    return 0;
}

void task_get_user_regs(uintptr_t *entry_out, uintptr_t *sp_out,
                        uintptr_t *l1_phys_out,
                        u32 *argc_out, uintptr_t *argv_out)
{
    task_t *t = current_task();
    if (entry_out)   *entry_out   = t->el0_entry;
    if (sp_out)      *sp_out      = t->el0_sp;
    if (l1_phys_out) *l1_phys_out = t->l1_table_phys;
    if (argc_out)    *argc_out    = t->el0_argc;
    if (argv_out)    *argv_out    = t->el0_argv;
}

/*
 * task_yield — surrender the CPU to the next eligible task on this core.
 *
 * SMP-safe design:
 *   1. Acquire g_sched_lock.
 *   2. find_next() selects the best READY task with affinity for this core.
 *   3. Mark 'from' TASK_READY, 'to' TASK_RUNNING.
 *   4. Update g_current_idx[my_core].
 *   5. Call context_switch_smp() which releases the lock AFTER saving 'from'
 *      but BEFORE loading 'to' — no race window remains.
 *   6. When we are RESUMED later, the lock is already released; continue.
 */
/*
 * task_switch_away — common scheduler core for task_yield() and task_sleep().
 *
 * If `sleep` is non-zero, the calling task is put to sleep until `wake_tick`;
 * otherwise it is simply yielded (left READY).  CRITICALLY, the state change
 * is applied here, while g_sched_lock is held, immediately before find_next()
 * and the context switch.  context_switch_smp() then saves the caller's
 * registers and releases the lock atomically.
 *
 * Why the state MUST change under the lock (SMP correctness):
 *   If task_sleep() set state=SLEEPING before acquiring the lock (the old
 *   design), another core running find_next() could observe the task as
 *   SLEEPING-with-an-expired-wake_tick, revive it to READY, and resume it
 *   from its STALE saved context — while this core is still executing on the
 *   same kernel stack.  Two cores on one kernel stack corrupt each other's
 *   frames; a later `ret` jumps to a stack data word (EC=0 undefined-instr
 *   panic).  Mutating state here, after the lock and before the register
 *   save, closes that window: by the time the task is schedulable again its
 *   context is already committed and this core has switched away.
 */
static void task_switch_away(u8 next_state, u64 wake_tick, u32 wait_pid)
{
    u32 my_core = cpu_id();

    spin_lock(&g_sched_lock);

    u32 from_idx = g_current_idx[my_core];
    task_t *from = &g_tasks[from_idx];

    /*
     * A task killed while RUNNING on this core (task_kill's deferred-free path)
     * is left ZOMBIE/DEAD but keeps executing in EL0 until its next syscall.
     * When it reaches here it must NOT be revived into a schedulable state —
     * doing so would resurrect a reaped task (whose resources may be freed)
     * back into the run queue.  Treat any non-RUNNING caller as a terminal
     * task that simply needs to be switched off this core: skip the blocking-
     * state change and just pick the next task below.
     */
    int terminal = (from->state != TASK_RUNNING);

    /*
     * WAIT mode: re-test the wake condition under the lock before blocking.
     * If the child already became a ZOMBIE we must NOT block, or we would
     * miss the wake_waiting_parent() that ran before we set TASK_WAITING —
     * a lost-wakeup hang.  Aborting here keeps check-and-block atomic.
     */
    if (next_state == TASK_WAITING && !terminal) {
        for (u32 i = 0; i < g_num_tasks; i++) {
            if (g_tasks[i].pid == wait_pid &&
                g_tasks[i].state == TASK_ZOMBIE) {
                spin_unlock(&g_sched_lock);
                return;   /* caller's loop will collect the zombie */
            }
        }
    }

    /* Apply the caller's requested blocking state under the lock (live tasks only). */
    if (!terminal && next_state == TASK_SLEEPING) {
        from->state     = TASK_SLEEPING;
        from->wake_tick = wake_tick;
    } else if (!terminal && next_state == TASK_WAITING) {
        from->state     = TASK_WAITING;
        from->wait_pid  = wait_pid;
    }
    /* next_state == TASK_READY (plain yield): handled after find_next below. */

    u32 to_idx = find_next();

    if (from_idx == to_idx) {
        /*
         * Nothing else is runnable on this core.  A task that asked to block
         * (SLEEPING/WAITING) with no replacement must not keep executing while
         * flagged blocked, so restore it to RUNNING.  (In practice the per-core
         * idle task is always READY, so this is only hit by idle yielding.)
         * Plain-yield (TASK_READY) leaves from->state untouched here, matching
         * the original semantics — important so task_exit()'s ZOMBIE/DEAD set
         * before a yield is never clobbered back to RUNNING.
         */
        if (!terminal && (next_state == TASK_SLEEPING || next_state == TASK_WAITING))
            from->state = TASK_RUNNING;
        spin_unlock(&g_sched_lock);
        return;
    }

    task_t *to = &g_tasks[to_idx];

    /* A plain-yielding RUNNING task returns to the READY pool. */
    if (next_state == TASK_READY && from->state == TASK_RUNNING)
        from->state = TASK_READY;
    to->state    = TASK_RUNNING;
    to->cpu_ticks++;

    g_current_idx[my_core] = to_idx;

    /*
     * context_switch_smp releases g_sched_lock after committing 'from'
     * registers to from->ctx, then loads 'to' registers and branches.
     * When we return (we've been resumed), the lock is NOT held.
     */
    context_switch_smp(&from->ctx, &to->ctx, &g_sched_lock);
    /* Resume point: lock is released. TTBR0 switch deferred to _el0_sync exit. */
}

/*
 * task_yield — surrender the CPU to the next eligible task on this core.
 * The caller is left READY and may be rescheduled immediately.
 */
void task_yield(void)
{
    task_switch_away(TASK_READY, 0, 0);
}

/*
 * vmm_switch_to_current_pt — switch TTBR0_EL1 to the page table of the task
 * currently assigned to this core.  Called from _el0_sync just before eret,
 * after IRQs are re-masked, so no preemption can occur between this switch
 * and the eret.  Replacing the old task_yield call-site avoids concurrent
 * TTBR0 switches when many tasks wake from the same vsync tick simultaneously.
 */
void vmm_switch_to_current_pt(void)
{
    vmm_switch_user_pt(task_current_l1());
}

void task_sleep(u64 ticks)
{
    /*
     * Compute the wake deadline and hand off to task_switch_away(), which
     * applies the SLEEPING state under g_sched_lock.  Setting the state here
     * (outside the lock) would let another core revive and double-schedule
     * this task from a stale context — see task_switch_away() for details.
     */
    task_switch_away(TASK_SLEEPING, timer_get_ticks() + ticks, 0);
}

__attribute__((noreturn))
void task_exit(void)
{
    task_t *t = current_task();
    kinfo("Scheduler: task[%lu] '%s' exited (ppid=%lu)\n",
          (unsigned long)t->pid, t->name, (unsigned long)t->ppid);

    wm_unregister_by_pid(t->pid, 0);

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

    for (u32 i = 0; i < PROC_MAX_FD; i++) {
        fd_entry_t *e = &t->fd_table[i];
        if (e->type == FD_TYPE_PIPE_R) pipe_close_read((int)e->pipe_idx);
        if (e->type == FD_TYPE_PIPE_W) pipe_close_write((int)e->pipe_idx);
        e->type = FD_TYPE_CLOSED;
    }

    /*
     * Publish the terminal state and wake any waiting parent atomically under
     * g_sched_lock.  This pairs with the locked zombie-recheck in
     * task_switch_away(TASK_WAITING, …): the parent either sees ZOMBIE during
     * its recheck (and does not block) or is flipped to READY here — never
     * both-missed (lost wakeup) nor both-applied (double schedule).
     * ZOMBIE/DEAD are not READY, so find_next() never reselects us in the
     * window between unlocking and the task_yield() below.
     */
    spin_lock(&g_sched_lock);
    if (t->ppid) {
        t->state = TASK_ZOMBIE;
        wake_waiting_parent(t->pid);   /* sets parent READY (caller holds lock) */
    } else {
        t->state = TASK_DEAD;
    }
    spin_unlock(&g_sched_lock);

    task_yield();
    for (;;) __asm__ volatile("wfi");
}

int task_waitpid(u32 pid, int *status)
{
    task_t *cur = current_task();

    task_t *child = NULL;
    for (u32 i = 0; i < g_num_tasks; i++) {
        if (g_tasks[i].pid == pid && g_tasks[i].ppid == cur->pid) {
            child = &g_tasks[i]; break;
        }
    }
    if (!child) return -1;

    /*
     * Block until the child is a zombie.  task_switch_away(TASK_WAITING, …)
     * sets our state and saves our context under g_sched_lock, and re-tests
     * the zombie condition under the same lock — so we can neither be
     * double-scheduled from a stale context nor miss the child's wakeup.
     */
    while (child->state != TASK_ZOMBIE)
        task_switch_away(TASK_WAITING, 0, pid);

    if (status) *status = child->exit_code;
    child->state = TASK_DEAD;
    return (int)pid;
}

int task_waitpid_nb(u32 pid, int *status)
{
    task_t *cur = current_task();

    task_t *child = NULL;
    for (u32 i = 0; i < g_num_tasks; i++) {
        if (g_tasks[i].pid == pid && g_tasks[i].ppid == cur->pid) {
            child = &g_tasks[i]; break;
        }
    }
    if (!child) return -1;
    if (child->state != TASK_ZOMBIE) return 0;

    if (status) *status = child->exit_code;
    child->state = TASK_DEAD;
    return (int)pid;
}

int task_ps(ps_entry_t *entries, int max_entries)
{
    int n = 0;
    for (u32 i = 0; i < g_num_tasks && n < max_entries; i++) {
        task_t *t = &g_tasks[i];
        if (t->state == TASK_UNUSED || t->state == TASK_DEAD) continue;
        entries[n].pid       = t->pid;
        entries[n].ppid      = t->ppid;
        entries[n].state     = t->state;
        entries[n].mem_pages = t->user_code_pages + t->user_stack_pages;
        entries[n].cpu_ticks = t->cpu_ticks;
        int j = 0;
        while (j < PROC_NAME_MAX - 1 && t->name[j]) { entries[n].name[j] = t->name[j]; j++; }
        entries[n].name[j] = '\0';
        n++;
    }
    return n;
}

int task_kill(u32 pid, int exit_code)
{
    task_t *cur = current_task();

    if (pid == 0)        return -1;
    if (pid == cur->pid) return -1;

    /* Snapshot was_running under the lock so we can decide whether to free. */
    spin_lock(&g_sched_lock);

    task_t *t = NULL;
    for (u32 i = 0; i < g_num_tasks; i++) {
        if (g_tasks[i].pid == pid) { t = &g_tasks[i]; break; }
    }
    if (!t) { spin_unlock(&g_sched_lock); return -1; }
    if (t->state == TASK_ZOMBIE || t->state == TASK_DEAD || t->state == TASK_UNUSED) {
        spin_unlock(&g_sched_lock); return 0;
    }

    bool was_running = (t->state == TASK_RUNNING);

    /*
     * Mark ZOMBIE/DEAD *before* releasing the lock and *before* freeing any
     * resources.  This prevents another core from scheduling the task again
     * and prevents a concurrent task_kill from double-freeing.
     */
    t->exit_code = exit_code;
    if (t->ppid)
        t->state = TASK_ZOMBIE;
    else
        t->state = TASK_DEAD;

    spin_unlock(&g_sched_lock);

    kinfo("Scheduler: task_kill pid=%lu by pid=%lu%s\n",
          (unsigned long)pid, (unsigned long)cur->pid,
          was_running ? " (was running — deferred free)" : "");

    wm_unregister_by_pid(pid, 1);

    /*
     * Only free physical resources if the task was NOT actively running on
     * another core at kill time.  If it was RUNNING, its TTBR0_EL1 and code
     * pages are still live on that core; freeing them immediately would cause
     * a use-after-free.  The resources leak until a proper deferred-cleanup
     * pass is added.  This is intentional and safer than the crash.
     */
    if (!was_running) {
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
    }

    spin_lock(&g_sched_lock);
    wake_waiting_parent(pid);
    spin_unlock(&g_sched_lock);

    return 0;
}

u32 task_current_pid(void)
{
    return g_tasks[g_current_idx[cpu_id()]].pid;
}

const char *task_current_name(void)
{
    return g_tasks[g_current_idx[cpu_id()]].name;
}

uintptr_t task_current_l1(void)
{
    uintptr_t l1 = g_tasks[g_current_idx[cpu_id()]].l1_table_phys;
    return l1 ? l1 : vmm_get_global_l1();
}

uintptr_t task_alloc_bo_va(u32 n_pages)
{
    task_t *t = current_task();
    uintptr_t va = t->bo_va_next;
    t->bo_va_next += (uintptr_t)n_pages * PMM_PAGE_SIZE;
    return va;
}

fd_entry_t *task_get_fd(u32 fd)
{
    if (fd >= PROC_MAX_FD) return NULL;
    return &g_tasks[g_current_idx[cpu_id()]].fd_table[fd];
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

    fd_entry_t *ne = &t->fd_table[newfd];
    if (ne->type == FD_TYPE_PIPE_R) pipe_close_read((int)ne->pipe_idx);
    if (ne->type == FD_TYPE_PIPE_W) pipe_close_write((int)ne->pipe_idx);

    *ne = t->fd_table[oldfd];
    if (ne->type == FD_TYPE_PIPE_R) pipe_open_read((int)ne->pipe_idx);
    if (ne->type == FD_TYPE_PIPE_W) pipe_open_write((int)ne->pipe_idx);
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
        kinfo("  [%lu] %s  (%s) aff=0x%x\n",
              (unsigned long)t->pid, t->name,
              state_names[t->state], t->cpu_affinity);
    }
    kinfo("────────────────────────────────────────\n");
}
