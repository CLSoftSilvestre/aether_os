#ifndef AETHER_PSCI_H
#define AETHER_PSCI_H

/*
 * AetherOS — PSCI (Power State Coordination Interface) Driver
 * File: kernel/drivers/power/psci.h
 *
 * PSCI is the ARM standard mechanism for system-level power control.
 * On QEMU -M virt the firmware exposes PSCI via HVC (Hypervisor Call).
 *
 * Relevant PSCI function IDs (SMC32 calling convention, 0x84xxxxxx):
 *   SYSTEM_OFF   0x84000008  — power off the machine
 *   SYSTEM_RESET 0x84000009  — reboot the machine
 */

/* Halt the machine immediately — does not return. */
__attribute__((noreturn)) void psci_system_off(void);

/* Reset (reboot) the machine immediately — does not return. */
__attribute__((noreturn)) void psci_system_reset(void);

#endif /* AETHER_PSCI_H */
