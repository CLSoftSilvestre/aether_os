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
#include "aether/wm.h"
#include "drivers/char/uart_pl011.h"
#include "drivers/irq/gic_v2.h"
#include "drivers/timer/arm_timer.h"
#include "drivers/video/ramfb.h"
#include "drivers/video/fb_console.h"
#include "drivers/video/cursor.h"
#include "drivers/input/pl050_kbd.h"
#include "drivers/input/pl050_mouse.h"
#include "drivers/input/virtio_input.h"
#include "drivers/usb/ohci.h"
#include "aether/net.h"
#include "aether/vfs.h"
#include "aether/fat32.h"
#include "aether/aetherfs.h"
#include "aether/types.h"
#include "drivers/block/virtio_blk.h"
#include "drivers/pci/pci_ecam.h"
#include "drivers/gpu/mailbox.h"
#include "drivers/gpu/v3d.h"

extern u8 __stack_top[];

/* ── Kernel entry ────────────────────────────────────────────────────────── */

__attribute__((noreturn))
void kernel_main(void)
{
    /* ── 1. UART — first, so we can see output immediately ─────────── */
    uart_init();
    uart_puts("\r\n");
    uart_puts("╔══════════════════════════════════════╗\r\n");
    uart_puts("║         AetherOS v0.0.6              ║\r\n");
    uart_puts("║   Phase 5.2 — Filesystem & Storage   ║\r\n");
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

    /*
     * Input subsystem — Phase 4.5.
     * PL050 KMI (PS/2) is present on real Pi hardware and on older QEMU.
     * QEMU 10.x virt machine does NOT include KMI devices by default;
     * accessing unmapped MMIO generates a Synchronous External Abort.
     * pl050_kbd_init() / pl050_mouse_init() are called only when the
     * device is confirmed present (Phase 5 — real hardware bring-up).
     *
     * For QEMU development, sys_key_read() falls back to UART → key_event
     * translation so the shell remains fully functional.
     */
    cursor_init();
    virtio_input_init();   /* kept for pl050/PS2 fallback; USB replaces it */
    usb_hid_init();

    /* ── 6b. Network (after framebuffer, before scheduler) ─────────── */
    /*
     * net_init() runs VirtIO net setup + DHCP (busy-poll, no IRQs needed).
     * DHCP must complete before IRQs are enabled so g_our_ip is valid by
     * the time userspace runs.  The timer IRQ path calls net_rx_poll() at
     * 100 Hz for ongoing packet delivery after DHCP.
     */
    net_init();

    /* ── 6b.5 GPU / V3D (Phase 6.1) ────────────────────────────────── */
    mailbox_init();
    v3d_init();

    /* ── 6c. Block storage + VFS (Phase 5.2) ───────────────────────── */
    /*
     * virtio_blk_init() probes the PCI bus for a virtio-blk-pci device.
     * Accepts both modern (0x1042) and transitional (0x1001/subsys=2) forms.
     * pci_list_devices() is called first to log all visible PCI devices.
     */
    pci_list_devices();
    virtio_blk_init();
    fat32_mount();
    aetherfs_mount();   /* device 1; no-op if second disk not attached */
    vfs_init();

    /* ── 7. Scheduler + Pipe + WM subsystem ────────────────────────── */
    pipe_init();
    wm_init();
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
    /* Seed the tick counter with the real elapsed boot time so that
     * userspace uptime counts from QEMU launch, not from this moment. */
    timer_seed_from_cntpct();
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
