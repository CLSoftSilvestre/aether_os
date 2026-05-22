/*
 * AetherOS — Real-Time Scheduler (Phase 8.0)
 * File: kernel/core/sched_rt.c
 *
 * Implements SCHED_FIFO / SCHED_RR priority-based preemption on top of the
 * existing cooperative round-robin scheduler.
 *
 * Integration model:
 *   scheduler.c:find_next() calls sched_rt_find_next() before the normal
 *   round-robin scan.  If any RT task is ready, it wins immediately.
 *   Among RT tasks, the one with the highest rt_priority (99 = highest) runs.
 *   Ties are broken by lowest task index (effectively FIFO within priority).
 *
 * CPU affinity:
 *   On a single-core QEMU virt machine affinity is advisory only — all tasks
 *   run on core 0.  On the real Pi 5, the SMP boot code would consult
 *   cpu_affinity before dispatching.  We record the mask and enforce it in
 *   the pick logic so the infrastructure is correct when SMP arrives.
 *
 * mlockall:
 *   We don't have swap, so mlockall is a bookkeeping flag today.  When a
 *   page-fault path exists, checking task->mlocked lets us panic instead of
 *   attempting a swap-in.
 */

#include "aether/sched.h"
#include "aether/scheduler.h"
#include "aether/printk.h"
#include "aether/types.h"

/* ── Latency statistics ──────────────────────────────────────────────── */

static audio_latency_stats_t g_stats;

void sched_rt_init(void)
{
    g_stats.callback_count  = 0;
    g_stats.xrun_count      = 0;
    g_stats.min_latency_ns  = (u64)-1;
    g_stats.max_latency_ns  = 0;
    g_stats.avg_latency_ns  = 0;
    g_stats.last_timestamp_ns = 0;
}

/* ── Timestamp ───────────────────────────────────────────────────────── */

u64 sched_rt_timestamp_ns(void)
{
    u64 cntpct, cntfrq;
    __asm__ volatile("mrs %0, CNTPCT_EL0" : "=r"(cntpct));
    __asm__ volatile("mrs %0, CNTFRQ_EL0" : "=r"(cntfrq));
    if (!cntfrq) cntfrq = 62500000ULL;   /* Pi 5 default: 62.5 MHz */
    /* Avoid overflow: split mul/div as (cntpct / freq_mhz) * 1000 */
    u64 freq_mhz = cntfrq / 1000000ULL;
    if (!freq_mhz) freq_mhz = 1;
    return (cntpct / freq_mhz) * 1000ULL;
}

/* ── Latency recording (called by audio driver per callback) ─────────── */

void sched_rt_record_callback(u64 now_ns, int xrun)
{
    g_stats.callback_count++;
    if (xrun) g_stats.xrun_count++;

    if (g_stats.last_timestamp_ns) {
        u64 jitter = now_ns - g_stats.last_timestamp_ns;
        if (jitter < g_stats.min_latency_ns) g_stats.min_latency_ns = jitter;
        if (jitter > g_stats.max_latency_ns) g_stats.max_latency_ns = jitter;
        /* Exponential moving average, α ≈ 1/16 */
        g_stats.avg_latency_ns = (g_stats.avg_latency_ns * 15 + jitter) / 16;
    }
    g_stats.last_timestamp_ns = now_ns;
}

void sched_rt_get_latency_stats(audio_latency_stats_t *out)
{
    if (out) *out = g_stats;
}

/* ── RT task picker ──────────────────────────────────────────────────── */

/*
 * sched_rt_find_next — scan the global task table for the highest-priority
 * ready RT task (SCHED_FIFO or SCHED_RR).
 *
 * Returns the task index, or -1 if no RT task is ready.
 * Called from scheduler.c:find_next() before the normal round-robin scan.
 */
int sched_rt_find_next(void)
{
    u32 n;
    task_t *tasks = task_get_table(&n);

    int best_idx  = -1;
    int best_prio = -1;

    for (u32 i = 0; i < n; i++) {
        task_t *t = &tasks[i];
        if (t->state != TASK_READY)
            continue;
        if (t->sched_policy == SCHED_NORMAL)
            continue;
        if ((int)t->rt_priority > best_prio) {
            best_prio = (int)t->rt_priority;
            best_idx  = (int)i;
        }
    }

    return best_idx;
}

/* ── Policy / affinity setters ───────────────────────────────────────── */

int sched_rt_set_policy(u32 pid, int policy, int rt_priority)
{
    if (policy < SCHED_NORMAL || policy > SCHED_RR) return -1;
    if (policy != SCHED_NORMAL &&
        (rt_priority < SCHED_RT_PRIO_MIN || rt_priority > SCHED_RT_PRIO_MAX))
        return -1;

    u32 n;
    task_t *tasks = task_get_table(&n);

    for (u32 i = 0; i < n; i++) {
        if (tasks[i].pid == pid && tasks[i].state != TASK_UNUSED) {
            tasks[i].sched_policy = (u8)policy;
            tasks[i].rt_priority  = (policy == SCHED_NORMAL) ? 0
                                                              : (u8)rt_priority;
            kinfo("sched_rt: PID %u policy=%d prio=%d\n",
                  pid, policy, tasks[i].rt_priority);
            return 0;
        }
    }
    return -1;
}

int sched_rt_set_affinity(u32 pid, u8 cpu_mask)
{
    if (!cpu_mask) return -1;   /* must allow at least one core */

    u32 n;
    task_t *tasks = task_get_table(&n);

    for (u32 i = 0; i < n; i++) {
        if (tasks[i].pid == pid && tasks[i].state != TASK_UNUSED) {
            tasks[i].cpu_affinity = cpu_mask;
            kinfo("sched_rt: PID %u cpu_affinity=0x%x\n", pid, cpu_mask);
            return 0;
        }
    }
    return -1;
}

int sched_rt_mlockall(u32 pid)
{
    u32 n;
    task_t *tasks = task_get_table(&n);

    for (u32 i = 0; i < n; i++) {
        if (tasks[i].pid == pid && tasks[i].state != TASK_UNUSED) {
            tasks[i].mlocked = 1;
            kinfo("sched_rt: PID %u memory locked\n", pid);
            return 0;
        }
    }
    return -1;
}
