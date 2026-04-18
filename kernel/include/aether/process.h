#ifndef AETHER_PROCESS_H
#define AETHER_PROCESS_H

#include "aether/types.h"

/*
 * process_spawn — load an ELF from initrd into the global (shared) user
 * address space.  Used for the first init process only.
 */
int process_spawn(const char *path);

/*
 * process_spawn_child — load an ELF from initrd into a new isolated
 * address space (per-process page tables).  Sets *child_pid_out to the
 * new PID.  Returns 0 on success, -1 on error.
 *
 * Called from the SYS_SPAWN handler (syscall.c).
 */
int process_spawn_child(const char *path, u32 ppid, u32 *child_pid_out);

#endif /* AETHER_PROCESS_H */
