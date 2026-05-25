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
