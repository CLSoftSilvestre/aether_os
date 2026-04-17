#ifndef AETHER_PROCESS_H
#define AETHER_PROCESS_H

/*
 * AetherOS — Process Manager
 *
 * process_spawn() is the high-level entry point for launching a new
 * user-space process:
 *
 *   1. Locate the ELF executable in the initrd by path
 *   2. Parse and load the ELF (PT_LOAD segments → user memory)
 *   3. Create a kernel task whose entry calls launch_el0(entry, stack)
 *
 * Phase 3.1 limitations (to be lifted in Phase 3.2+):
 *   - Single active user process (no fork/exec)
 *   - Shared identity-mapped address space (no per-process TTBR0)
 *   - User stack is a fixed region at VMM_USER_STACK_TOP
 */

/*
 * process_spawn — load an ELF from the initrd and start it as EL0 task.
 *
 * path: file path in the initrd (e.g. "/init" or "init")
 *
 * Returns 0 on success, -1 on error.
 * On success, a new kernel task is ready-queued and will run when
 * the scheduler selects it.
 */
int process_spawn(const char *path);

#endif /* AETHER_PROCESS_H */
