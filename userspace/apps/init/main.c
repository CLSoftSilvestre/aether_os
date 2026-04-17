/*
 * AetherOS — First user-space process
 * File: userspace/apps/init/main.c
 *
 * PID 1 — the first process loaded from the initrd.
 * Compiled to a static ELF64, loaded by the kernel ELF loader.
 *
 * This replaces the hardcoded init_task.S blob from Phase 3.0.
 * Unlike init_task.S, this is real C code linked against crt0.S.
 */

#include "sys.h"

int main(void)
{
    sys_puts("┌─────────────────────────────────────┐\n");
    sys_puts("│  Hello from C user space!           │\n");
    sys_puts("│  AetherOS init — loaded from ELF    │\n");
    sys_puts("│  Running at EL0 via initrd loader   │\n");
    sys_puts("└─────────────────────────────────────┘\n");

    return 0;
}
