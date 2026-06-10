/*
 * AetherOS — PSCI Driver
 * File: kernel/drivers/power/psci.c
 *
 * Implements system shutdown and reboot via the ARM PSCI HVC interface.
 * QEMU -M virt exposes PSCI 0.2 at EL2 via HVC (Hypervisor Call).
 *
 * The HVC calling convention matches SMC32 (x0 = function ID).
 * We use the 32-bit PSCI function IDs (0x84xxxxxx prefix):
 *   SYSTEM_OFF   = 0x84000008
 *   SYSTEM_RESET = 0x84000009
 *
 * Both calls do not return on QEMU (the VM stops or resets).
 * The infinite wfi loop after each hvc is a safety fallback in case
 * the hypervisor returns unexpectedly (bare-metal with no EL2 present).
 */

#include "drivers/power/psci.h"
#include "aether/printk.h"

#define PSCI_SYSTEM_OFF   0x84000008u
#define PSCI_SYSTEM_RESET 0x84000009u

/*
 * PSCI_CPU_ON_64 — PSCI 0.2 CPU_ON for AArch64.
 * Function ID 0xC4000003 (SMC64 convention, 0xC4 prefix = 64-bit call).
 * Arguments (via x1, x2, x3):
 *   x1 = target_affinity  — MPIDR of the target CPU (Aff0 = core_id on flat topology)
 *   x2 = entry_point_address — physical address to jump to on the target core
 *   x3 = context_id       — arbitrary value delivered in x0 on the target core
 * Return value in x0: 0 = success, negative = PSCI error code.
 */
#define PSCI_CPU_ON_64    0xC4000003u

int psci_cpu_on(u32 target_cpu, u64 entry_pa)
{
    register u64 x0 asm("x0") = PSCI_CPU_ON_64;
    register u64 x1 asm("x1") = (u64)target_cpu;   /* MPIDR target affinity */
    register u64 x2 asm("x2") = entry_pa;            /* physical entry address */
    register u64 x3 asm("x3") = (u64)target_cpu;   /* context_id — reuse core id */
    __asm__ volatile(
        "hvc #0"
        : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3)
        :
        : "memory"
    );
    /* PSCI returns a 32-bit signed status in x0[31:0]; sign-extend for int. */
    return (int)(s32)(u32)x0;
}

__attribute__((noreturn)) void psci_system_off(void)
{
    kinfo("[PSCI] System OFF\n");
    register unsigned long x0 asm("x0") = PSCI_SYSTEM_OFF;
    __asm__ volatile(
        "hvc #0\n"
        "1: wfi\n"
        "   b 1b\n"
        : "+r"(x0)
        :
        : "memory"
    );
    __builtin_unreachable();
}

__attribute__((noreturn)) void psci_system_reset(void)
{
    kinfo("[PSCI] System RESET\n");
    register unsigned long x0 asm("x0") = PSCI_SYSTEM_RESET;
    __asm__ volatile(
        "hvc #0\n"
        "1: wfi\n"
        "   b 1b\n"
        : "+r"(x0)
        :
        : "memory"
    );
    __builtin_unreachable();
}
