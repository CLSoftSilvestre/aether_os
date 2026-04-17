/*
 * AetherOS — Process Manager
 * File: kernel/core/process.c
 *
 * High-level user process creation: finds an ELF in the initrd,
 * loads it, and creates a kernel task that launches EL0 execution.
 *
 * Flow:
 *   process_spawn("/init")
 *     → initrd_find("/init")        find ELF bytes in embedded CPIO
 *     → elf_load(data, size)        copy PT_LOAD segments to user VA
 *     → task_create_user(entry, sp) create kernel task → user_task_trampoline
 *     [later, when scheduled]
 *     → user_task_trampoline()      retrieves entry+sp from task_t
 *     → launch_el0(entry, sp)       eret to EL0
 *
 * The trampoline lives here rather than in scheduler.c to keep the
 * vmm.h dependency out of the scheduler.
 */

#include "aether/process.h"
#include "aether/initrd.h"
#include "aether/elf.h"
#include "aether/scheduler.h"
#include "aether/vmm.h"
#include "aether/printk.h"

/*
 * user_task_trampoline — kernel-side entry for every user process.
 *
 * Called by the scheduler on the first time a user task is scheduled.
 * Retrieves the EL0 entry point and stack from the current task_t,
 * then does an `eret` into EL0 via launch_el0().  Does not return.
 */
static void user_task_trampoline(void)
{
    uintptr_t entry, sp;
    task_get_user_regs(&entry, &sp);
    kinfo("Process: entering EL0 — entry=%p  sp=%p\n",
          (void *)entry, (void *)sp);
    launch_el0(entry, sp);
}

int process_spawn(const char *path)
{
    kinfo("Process: spawning '%s'\n", path);

    /* ── 1. Find ELF in initrd ────────────────────────────────────── */
    u32 elf_size = 0;
    const void *elf_data = initrd_find(path, &elf_size);
    if (!elf_data) {
        kerror("Process: '%s' not found in initrd\n", path);
        return -1;
    }
    kinfo("Process: found '%s' (%lu bytes)\n", path, (unsigned long)elf_size);

    /* ── 2. Load ELF into user memory ─────────────────────────────── */
    uintptr_t entry = elf_load(elf_data, elf_size);
    if (!entry) {
        kerror("Process: ELF load failed for '%s'\n", path);
        return -1;
    }

    /* ── 3. Create kernel task that will launch EL0 ──────────────── */
    int rc = task_create_user(entry, VMM_USER_STACK_TOP,
                              path, user_task_trampoline);
    if (rc != 0) {
        kerror("Process: task_create_user failed\n");
        return -1;
    }

    kinfo("Process: '%s' queued (entry=%p  stack=%p)\n",
          path, (void *)entry, (void *)VMM_USER_STACK_TOP);
    return 0;
}
