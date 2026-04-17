/*
 * AetherOS — Kernel Entry Point
 * File: kernel/core/main.c
 *
 * Phase 3 initialisation sequence:
 *   UART → exceptions → PMM → kmalloc → VMM (MMU on)
 *   → GIC → timer → scheduler → IRQs → launch EL0 user process
 */

#include "aether/printk.h"
#include "aether/exceptions.h"
#include "aether/mm.h"
#include "aether/kmalloc.h"
#include "aether/scheduler.h"
#include "aether/vmm.h"
#include "drivers/char/uart_pl011.h"
#include "drivers/irq/gic_v2.h"
#include "drivers/timer/arm_timer.h"
#include "aether/types.h"

extern u8 __stack_top[];

/*
 * Symbols emitted by init_task.S — the first user-space program.
 * The code lives in kernel .text (EL1-only after MMU setup).
 * We copy it at runtime to VMM_USER_CODE_BASE (EL0-accessible region).
 */
extern u8 user_init_start[];
extern u8 user_init_end[];

/* ── Kernel entry ────────────────────────────────────────────────────────── */

__attribute__((noreturn))
void kernel_main(void)
{
    /* ── 1. UART — first, so we can see output immediately ─────────── */
    uart_init();
    uart_puts("\r\n");
    uart_puts("╔══════════════════════════════════════╗\r\n");
    uart_puts("║         AetherOS v0.0.3              ║\r\n");
    uart_puts("║     Phase 3 — MMU + User Space       ║\r\n");
    uart_puts("╚══════════════════════════════════════╝\r\n");
    uart_puts("\r\n");

    /* ── 2. Exception vector table ──────────────────────────────────── */
    /*
     * Must come before anything that could trigger an exception
     * (including printk, which does MMIO writes to the UART).
     * In practice it works without this, but it is the correct order.
     */
    exceptions_init();

    /* ── 3. Physical Memory Manager ─────────────────────────────────── */
    pmm_init();
    pmm_print_stats();

    /* ── 4. Kernel heap ──────────────────────────────────────────────── */
    kmalloc_init();

    /* ── 5. MMU ─────────────────────────────────────────────────────── */
    /*
     * Enable the MMU with an identity map (VA==PA).
     * After this call:
     *   - D-cache and I-cache are active
     *   - EL0 can access RAM (0x40000000–0x7FFFFFFF)
     *   - EL0 cannot access MMIO (0x00000000–0x3FFFFFFF)
     * Must come before GIC/timer so device-memory AP bits are in effect.
     */
    vmm_init();

    /* ── 6. GIC + Timer ─────────────────────────────────────────────── */
    gic_init();
    timer_init();

    /* ── 7. Scheduler ───────────────────────────────────────────────── */
    scheduler_init();
    scheduler_add_idle();

    /* ── 8. Enable IRQs ─────────────────────────────────────────────── */
    kinfo("Enabling IRQs\n");
    kinfo("────────────────────────────────────────────\n");
    __asm__ volatile("msr daifclr, #2" ::: "memory");

    /* ── 9. Copy user code to EL0-accessible region and launch ─────── */
    /*
     * After the MMU is enabled, the kernel binary lives in EL1-only pages
     * (AP=EL1_RW).  EL0 cannot execute from there.
     *
     * The last 4MB of RAM (0x7FC00000–0x7FFFFFFF) is mapped AP=BOTH_RW:
     *   - VMM_USER_CODE_BASE (0x7FC00000): we copy user_init_start here
     *   - VMM_USER_STACK_TOP (0x7FFFF000): initial SP_EL0
     *
     * user_init_start uses PC-relative addressing (adr instruction), so
     * copying the blob verbatim to any address works correctly.
     */
    uintptr_t code_size = (uintptr_t)user_init_end - (uintptr_t)user_init_start;
    u8 *dst = (u8 *)VMM_USER_CODE_BASE;
    for (uintptr_t i = 0; i < code_size; i++)
        dst[i] = user_init_start[i];

    kinfo("Phase 3: copied %lu bytes of user code to %p\n",
          (unsigned long)code_size, dst);
    kinfo("Phase 3: launching EL0 — entry=%p  stack=%p\n",
          (void *)VMM_USER_CODE_BASE, (void *)VMM_USER_STACK_TOP);

    launch_el0(VMM_USER_CODE_BASE, VMM_USER_STACK_TOP);
}
