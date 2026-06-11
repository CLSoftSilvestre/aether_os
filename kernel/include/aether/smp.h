#ifndef AETHER_SMP_H
#define AETHER_SMP_H

/*
 * AetherOS — SMP (Symmetric Multi-Processing) Subsystem
 * File: kernel/include/aether/smp.h
 *
 * Pi 5 / QEMU virt: 4 × Cortex-A76 cores.
 *
 * Boot sequence:
 *   Core 0: full kernel init → smp_init() → smp_signal_ready()
 *   Core N: secondary_boot_entry → secondary_main(N) → idle loop
 *
 * cpu_id() is used throughout the scheduler and drivers to identify
 * which core is currently executing, without any external state.
 */

#include "aether/types.h"

#define NUM_CPUS  4

/*
 * cpu_id — return the hardware ID (0-3) of the calling core.
 *
 * Reads MPIDR_EL1 bits [7:0] (Aff0 field).  On Pi 5 and QEMU virt
 * with a flat core topology, Aff0 == core number.
 *
 * Declared inline so it compiles to a single MRS instruction wherever used.
 */
static inline u32 cpu_id(void)
{
    u64 mpidr;
    __asm__ volatile("mrs %0, MPIDR_EL1" : "=r"(mpidr));
    return (u32)(mpidr & 0xFFu);
}

/*
 * g_secondary_stack_tops[i] — kernel stack top for core i (i = 1-3).
 * Written by smp_init() before PSCI CPU_ON; read by _secondary_start in boot.S.
 * Index 0 is unused (core 0 uses __stack_top from linker.ld).
 */
extern u64 g_secondary_stack_tops[NUM_CPUS];

/*
 * g_smp_ready — set to 1 by core 0 (via smp_signal_ready) when all kernel
 * subsystems are initialised and it is safe for secondary cores to schedule
 * user tasks.  Secondary cores spin on this with WFE before entering their
 * idle loop.
 */
extern volatile u32 g_smp_ready;

/*
 * g_kernel_halted — set to 1 by kpanic (via smp_halt_all_cores) to signal
 * all cores to stop scheduling and park.  Checked in secondary idle loops.
 */
extern volatile u32 g_kernel_halted;

/*
 * secondary_boot_entry — assembly entry point for secondary cores.
 * Defined in boot.S; PSCI CPU_ON is told to jump here.
 * Sets up EL1, switches to SP_EL1, loads the per-core stack, then calls
 * secondary_main(core_id).
 */
extern void secondary_boot_entry(void);

/*
 * smp_init — wake secondary cores via PSCI CPU_ON.
 * Must be called after scheduler_init() and all shared subsystems are up.
 * Called once from kernel_main (core 0).
 */
void smp_init(void);

/*
 * smp_signal_ready — mark kernel init complete and wake secondary cores
 * from their WFE spin loop.  Call just before enabling IRQs on core 0.
 */
void smp_signal_ready(void);

/*
 * smp_halt_all_cores — set g_kernel_halted=1 and SEV to wake all WFE-sleeping
 * cores so they stop scheduling and park.  Called from kpanic.
 */
void smp_halt_all_cores(void);

/*
 * secondary_main — C entry point executed by each secondary core after
 * _secondary_start has set up the stack.  Never returns.
 */
void secondary_main(u32 core_id);

#endif /* AETHER_SMP_H */
