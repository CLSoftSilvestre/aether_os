/*
 * AetherOS — SMP Bringup
 * File: kernel/core/smp.c
 *
 * Wakes the three secondary Cortex-A76 cores (1-3) via PSCI CPU_ON and
 * provides the C entry point (secondary_main) that each secondary calls after
 * the boot.S stub sets up its kernel stack.
 *
 * Secondary core boot sequence
 * ─────────────────────────────
 *   _secondary_start  (boot.S)
 *     → check / drop from EL2 to EL1
 *     → enable FP/SIMD
 *     → switch to SP_EL1
 *     → load per-core stack from g_secondary_stack_tops[core_id]
 *     → bl secondary_main(core_id)
 *
 *   secondary_main(core_id)
 *     → exceptions_init()           — install VBAR_EL1 on this core
 *     → gic_cpu_interface_init()    — enable GICC on this core
 *     → timer_secondary_init()      — enable per-core timer PPI 30
 *     → scheduler_secondary_init()  — register per-core idle task
 *     → spin on g_smp_ready (WFE)   — wait for core 0 to finish boot
 *     → enable IRQs (DAIFCLR)
 *     → idle loop: task_yield() + WFI
 *
 * Synchronisation
 * ───────────────
 * g_secondary_stack_tops[] is written by smp_init() and a DSB is issued
 * before the PSCI calls so secondaries always see valid stack pointers.
 *
 * g_smp_ready is set by smp_signal_ready() (called from kernel_main just
 * before enabling IRQs on core 0).  Secondary cores use WFE/WFI to avoid
 * burning cycles while waiting.  core 0 issues SEV after setting the flag.
 */

#include "aether/smp.h"
#include "aether/sched.h"
#include "aether/scheduler.h"
#include "aether/exceptions.h"
#include "aether/printk.h"
#include "aether/vmm.h"
#include "drivers/power/psci.h"
#include "drivers/irq/gic_v2.h"
#include "drivers/timer/arm_timer.h"

/* ── Per-core kernel stacks for cores 1-3 ───────────────────────────────── */

#define SECONDARY_STACK_SIZE  (16u * 1024u)   /* 16 KB per secondary core */

/*
 * Placed in BSS (zero-initialised).  16-byte aligned as required by AArch64
 * SP: the stack pointer must always be 16-byte aligned at any public call boundary.
 * Stack index 0 = core 1, index 1 = core 2, index 2 = core 3.
 */
static u8 s_secondary_stacks[NUM_CPUS - 1][SECONDARY_STACK_SIZE]
    __attribute__((aligned(16)));

/* ── Exported globals ────────────────────────────────────────────────────── */

/* Stack tops read by _secondary_start in boot.S via an LDR from the symbol. */
u64 g_secondary_stack_tops[NUM_CPUS];   /* [0] unused — core 0 uses __stack_top */

/* Set to 1 by smp_signal_ready(); secondary cores spin on this with WFE. */
volatile u32 g_smp_ready = 0;

/* Set to 1 by any core calling kpanic; all cores check this and halt. */
volatile u32 g_kernel_halted = 0;

/* ── Public: wake secondary cores (called from kernel_main on core 0) ────── */

void smp_init(void)
{
    /* Fill stack-top addresses: each secondary gets 16 KB of BSS. */
    for (u32 i = 1; i < NUM_CPUS; i++)
        g_secondary_stack_tops[i] =
            (u64)(uintptr_t)(s_secondary_stacks[i - 1] + SECONDARY_STACK_SIZE);

    /*
     * DSB SY — Data Synchronisation Barrier.
     * Ensures all writes to g_secondary_stack_tops[] are globally visible
     * before any secondary core reads them.
     */
    __asm__ volatile("dsb sy" ::: "memory");

    /*
     * Wake each secondary core via PSCI CPU_ON.
     * The entry point is secondary_boot_entry (_secondary_start in boot.S).
     * MPIDR Aff0 equals core_id on Pi 5 / QEMU virt flat topology.
     */
    for (u32 i = 1; i < NUM_CPUS; i++) {
        int rc = psci_cpu_on(i, (u64)(uintptr_t)secondary_boot_entry);
        if (rc == 0)
            kinfo("SMP: core %u woken via PSCI CPU_ON\n", i);
        else
            kwarn("SMP: core %u PSCI_CPU_ON returned %d — not available\n",
                  i, rc);
    }
}

void smp_signal_ready(void)
{
    /* Ensure all prior kernel init writes are globally visible. */
    __asm__ volatile("dsb sy" ::: "memory");
    g_smp_ready = 1;
    /* SEV — Send Event: wakes all cores sleeping in WFE. */
    __asm__ volatile("sev" ::: "memory");
}

void smp_halt_all_cores(void)
{
    __asm__ volatile("dsb sy" ::: "memory");
    g_kernel_halted = 1;
    __asm__ volatile("dsb sy\nsev" ::: "memory");
}

/* ── Entry point for secondary cores ────────────────────────────────────── */

void secondary_main(u32 core_id)
{
    /*
     * Enable the MMU on this core FIRST — before any printk or other call.
     * Without this, EL0 user tasks run with VA==PA (physical addressing),
     * which causes them to execute the wrong physical pages (init's code
     * instead of their own) → Address Size Fault and kernel panic.
     */
    vmm_secondary_mmu_init();

    /* Install the exception vector table on this core (sets VBAR_EL1). */
    exceptions_init();

    /* Enable this core's GIC CPU interface (GICC_PMR, GICC_BPR, GICC_CTLR).
     * The GIC distributor is already configured by core 0's gic_init(). */
    gic_cpu_interface_init();

    /*
     * Enable the per-CPU timer (PPI 30) on this core.
     * GICv2 PPIs are banked per CPU, so gic_enable_irq(30) affects only
     * the calling core's private copy of GICD_ISENABLER[0].
     * The timer fires at TIMER_HZ (100 Hz) to keep the idle loop responsive.
     */
    timer_secondary_init();

    /*
     * Register this core's idle task with the global scheduler.
     * The idle task is pinned to this core via cpu_affinity.
     * The current execution context (this function's stack frame) becomes
     * the idle context — just like scheduler_add_idle() does for core 0.
     */
    scheduler_secondary_init(core_id);

    kinfo("SMP: core %u online — waiting for kernel init\n", core_id);

    /*
     * Spin until core 0 signals that all kernel subsystems are ready.
     * WFE sleeps the core until an event (the SEV from smp_signal_ready).
     * Loop because WFE can wake spuriously.
     */
    while (!g_smp_ready)
        __asm__ volatile("wfe" ::: "memory");

    /* Memory barrier: ensure we see all writes made before g_smp_ready=1. */
    __asm__ volatile("dsb sy" ::: "memory");

    kinfo("SMP: core %u entering scheduler\n", core_id);

    /* Enable IRQs on this core. */
    __asm__ volatile("msr daifclr, #2" ::: "memory");

    /*
     * Idle loop.
     * task_yield() picks up any runnable task with affinity for this core.
     * WFI sleeps until an interrupt (timer PPI 30 fires every 10 ms),
     * at which point the IRQ is handled and we loop back to yield again.
     * g_kernel_halted is set by kpanic (via smp_halt_all_cores) — when set,
     * stop scheduling and park this core permanently.
     */
    for (;;) {
        if (g_kernel_halted)
            for (;;) __asm__ volatile("wfi" ::: "memory");
        task_yield();
        __asm__ volatile("wfi" ::: "memory");
    }
}
