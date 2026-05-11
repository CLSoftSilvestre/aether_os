/*
 * AetherOS — Boot Time Profiler (Phase 6.2.5)
 * File: kernel/drivers/power/boot_prof.c
 *
 * Uses CNTPCT_EL0 (the ARM physical counter) directly for microsecond
 * resolution — timer_get_ticks() only gives 10 ms (100 Hz) granularity.
 */

#include "drivers/power/boot_prof.h"
#include "aether/printk.h"
#include "aether/types.h"

/* ── ARM counter accessors (inline ASM, same registers as arm_timer.c) ── */

static inline u64 bp_cntpct(void)
{
    u64 v;
    __asm__ volatile("mrs %0, CNTPCT_EL0" : "=r"(v));
    return v;
}

static inline u64 bp_cntfrq(void)
{
    u64 v;
    __asm__ volatile("mrs %0, CNTFRQ_EL0" : "=r"(v));
    return v ? v : 62500000ULL;
}

/* ── Stamp table ─────────────────────────────────────────────────────── */

typedef struct {
    const char *label;
    u64         cntpct;
} stamp_t;

static stamp_t g_stamps[BOOT_PROF_MAX_STAMPS];
static int     g_nstamps = 0;

void boot_prof_stamp(const char *label)
{
    if (g_nstamps >= BOOT_PROF_MAX_STAMPS) return;
    g_stamps[g_nstamps].label  = label;
    g_stamps[g_nstamps].cntpct = bp_cntpct();
    g_nstamps++;
}

void boot_prof_print(void)
{
    if (g_nstamps < 1) return;

    u64 freq = bp_cntfrq();

    kinfo("────── Boot Profile ─────────────────────────────────\n");
    kinfo("  %-18s  %8s  %10s\n", "phase", "Δ µs", "total µs");
    kinfo("  %-18s  %8s  %10s\n", "──────────────────", "────────", "──────────");

    u64 t0    = g_stamps[0].cntpct;
    u64 t_prev = t0;

    for (int i = 0; i < g_nstamps; i++) {
        u64 t    = g_stamps[i].cntpct;
        u64 dt   = ((t - t_prev) * 1000000ULL) / freq;  /* µs since last stamp */
        u64 tot  = ((t - t0)     * 1000000ULL) / freq;  /* µs since first stamp */
        kinfo("  %-18s  %8llu  %10llu\n",
              g_stamps[i].label,
              (unsigned long long)dt,
              (unsigned long long)tot);
        t_prev = t;
    }

    u64 total_us = ((g_stamps[g_nstamps - 1].cntpct - t0) * 1000000ULL) / freq;
    u64 total_s  = total_us / 1000000ULL;
    u64 total_ms = (total_us % 1000000ULL) / 1000ULL;
    kinfo("  %-18s  %8s  %10s\n", "──────────────────", "────────", "──────────");
    kinfo("  TOTAL                           %llu.%03llu s (%llu µs)\n",
          (unsigned long long)total_s,
          (unsigned long long)total_ms,
          (unsigned long long)total_us);
    kinfo("─────────────────────────────────────────────────────\n");
}
