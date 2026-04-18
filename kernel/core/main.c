/*
 * AetherOS — Kernel Entry Point
 * File: kernel/core/main.c
 *
 * Phase 3.1 initialisation sequence:
 *   UART → exceptions → PMM → kmalloc → VMM (MMU on)
 *   → GIC → timer → scheduler → initrd → process_spawn → IRQs → idle
 */

#include "aether/printk.h"
#include "aether/exceptions.h"
#include "aether/mm.h"
#include "aether/kmalloc.h"
#include "aether/pipe.h"
#include "aether/scheduler.h"
#include "aether/vmm.h"
#include "aether/initrd.h"
#include "aether/process.h"
#include "drivers/char/uart_pl011.h"
#include "drivers/irq/gic_v2.h"
#include "drivers/timer/arm_timer.h"
#include "drivers/video/ramfb.h"
#include "drivers/video/fb_console.h"
#include "aether/types.h"

extern u8 __stack_top[];

/* ── Kernel entry ────────────────────────────────────────────────────────── */

__attribute__((noreturn))
void kernel_main(void)
{
    /* ── 1. UART — first, so we can see output immediately ─────────── */
    uart_init();
    uart_puts("\r\n");
    uart_puts("╔══════════════════════════════════════╗\r\n");
    uart_puts("║         AetherOS v0.0.5              ║\r\n");
    uart_puts("║   Phase 4.0 — Framebuffer + Shell    ║\r\n");
    uart_puts("╚══════════════════════════════════════╝\r\n");
    uart_puts("\r\n");

    /* ── 2. Exception vector table ──────────────────────────────────── */
    exceptions_init();

    /* ── 3. Physical Memory Manager ─────────────────────────────────── */
    pmm_init();
    pmm_print_stats();

    /* ── 4. Kernel heap ──────────────────────────────────────────────── */
    kmalloc_init();

    /* ── 5. MMU ─────────────────────────────────────────────────────── */
    /*
     * Identity map, caches on.
     * User region: 0x70000000–0x7FFFFFFF (AP=BOTH_RW, EL0 can execute).
     * Kernel region: 0x40000000–0x6FFFFFFF (AP=EL1_RW only).
     */
    vmm_init();

    /* ── 5b. Framebuffer (after MMU so caches are on) ───────────────── */
    /*
     * ramfb_init() configures QEMU's ramfb device via fw_cfg,
     * allocates 3MB of contiguous RAM for the pixel buffer, and
     * populates fb_base/fb_width/fb_height/fb_stride.
     * fb_console_init() sets up the scrolling text console on the FB.
     * After this point, printk() outputs to both UART and screen.
     */
    ramfb_init();
    fb_console_init();

    /* ── 6. GIC + Timer ─────────────────────────────────────────────── */
    gic_init();
    uart_enable_rx_irq();
    timer_init();

    /* ── 7. Scheduler + Pipe subsystem ─────────────────────────────── */
    pipe_init();
    scheduler_init();
    scheduler_add_idle();

    /* ── 8. initrd ──────────────────────────────────────────────────── */
    initrd_init();

    /* ── 9. Spawn first user process from initrd ELF ───────────────── */
    /*
     * process_spawn() locates "init" in the CPIO archive, loads its ELF
     * segments into the user region (0x70000000+), and creates a kernel
     * task whose entry is user_task_trampoline → launch_el0(entry, sp).
     */
    if (process_spawn("/init") != 0)
        kpanic("kernel_main: failed to spawn /init\n");

    /* ── 10. Enable IRQs and enter idle loop ────────────────────────── */
    kinfo("Enabling IRQs — entering idle loop\n");
    kinfo("────────────────────────────────────────────\n");
    __asm__ volatile("msr daifclr, #2" ::: "memory");

    /*
     * The scheduler will yield from idle to the init task on the next
     * timer tick.  Until then, spin here.
     */
    for (;;) {
        task_yield();
        __asm__ volatile("wfi" ::: "memory");
    }
}
