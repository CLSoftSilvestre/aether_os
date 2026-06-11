#ifndef AETHER_PSCI_H
#define AETHER_PSCI_H

/*
 * AetherOS — PSCI (Power State Coordination Interface) Driver
 * File: kernel/drivers/power/psci.h
 *
 * PSCI is the ARM standard mechanism for system-level power control.
 * On QEMU -M virt the firmware exposes PSCI via HVC (Hypervisor Call).
 *
 * Relevant PSCI function IDs:
 *   SYSTEM_OFF   0x84000008  — power off the machine      (SMC32)
 *   SYSTEM_RESET 0x84000009  — reboot the machine         (SMC32)
 *   CPU_ON       0xC4000003  — wake a secondary core      (SMC64)
 */

#include "aether/types.h"

/*
 * psci_cpu_on — wake a secondary core via PSCI CPU_ON (function 0xC4000003).
 *
 * target_cpu : MPIDR Aff0 value of the target core (0-3 on Pi 5 / QEMU virt).
 * entry_pa   : physical address of the secondary core's entry point.
 *
 * Returns 0 on success, negative PSCI error code on failure
 * (e.g. PSCI_RET_ALREADY_ON = -4 if the core is already running).
 */
int psci_cpu_on(u32 target_cpu, u64 entry_pa);

/* Halt the machine immediately — does not return. */
__attribute__((noreturn)) void psci_system_off(void);

/* Reset (reboot) the machine immediately — does not return. */
__attribute__((noreturn)) void psci_system_reset(void);

#endif /* AETHER_PSCI_H */
