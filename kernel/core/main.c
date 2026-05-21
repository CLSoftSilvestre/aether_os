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
#include "aether/config.h"
#include "aether/users.h"
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
#include "drivers/power/boot_prof.h"
#include "drivers/power/cpufreq.h"
#include "drivers/power/thermal.h"
#include "drivers/power/dpms.h"

extern u8 __stack_top[];

/* ── Kernel entry ────────────────────────────────────────────────────────── */

__attribute__((noreturn))
void kernel_main(void)
{
    /* ── 1. UART — first, so we can see output immediately ─────────── */
    uart_init();
    boot_prof_stamp("uart");
    uart_puts("\r\n");
    uart_puts("╔══════════════════════════════════════╗\r\n");
    uart_puts("║         AetherOS v0.0.7              ║\r\n");
    uart_puts("║  Phase 6.2 — Power Management        ║\r\n");
    uart_puts("╚══════════════════════════════════════╝\r\n");
    uart_puts("\r\n");

    /* ── 2. Exception vector table ──────────────────────────────────── */
    exceptions_init();
    boot_prof_stamp("exceptions");

    /* ── 3. Physical Memory Manager ─────────────────────────────────── */
    pmm_init();
    pmm_print_stats();
    boot_prof_stamp("pmm");

    /* ── 4. Kernel heap ──────────────────────────────────────────────── */
    kmalloc_init();
    boot_prof_stamp("kmalloc");

    /* ── 5. MMU ─────────────────────────────────────────────────────── */
    /*
     * Identity map, caches on.
     * User region: 0x70000000–0x7FFFFFFF (AP=BOTH_RW, EL0 can execute).
     * Kernel region: 0x40000000–0x6FFFFFFF (AP=EL1_RW only).
     */
    vmm_init();
    boot_prof_stamp("vmm");

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
    boot_prof_stamp("framebuffer");

    /* ── 6. GIC + Timer ─────────────────────────────────────────────── */
    gic_init();
    uart_enable_rx_irq();
    timer_init();
    boot_prof_stamp("gic+timer");

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
    boot_prof_stamp("input+usb");

    /* ── 6b.5 GPU / V3D + Power (Phase 6.1 / 6.2) ──────────────────── */
    mailbox_init();
    v3d_init();
    cpufreq_init();
    thermal_init();
    dpms_init();
    boot_prof_stamp("gpu+power");

    /* ── 6c. Block storage + VFS (Phase 5.2) ───────────────────────── */
    /*
     * Moved before net_init so the network config can be read from FAT32
     * (/config/network.conf) before deciding DHCP vs. static IP, and so
     * the display config (/config/display.conf) can be applied before
     * spawning init.
     */
    pci_list_devices();
    virtio_blk_init();
    fat32_mount();
    aetherfs_mount();   /* device 1; no-op if second disk not attached */
    vfs_init();
    boot_prof_stamp("storage+vfs");

    /* ── 6c.5 System config — display resolution ────────────────────── */
    /*
     * Apply the stored display resolution (if different from default 1280×720)
     * before any app runs.  Done after VFS is up so we can read FAT32.
     */
    {
        char wbuf[8], hbuf[8];
        if (kconfig_read("/config/display.conf", "width",  wbuf, 8) == 0 &&
            kconfig_read("/config/display.conf", "height", hbuf, 8) == 0) {
            u32 cw = (u32)katoi(wbuf);
            u32 ch = (u32)katoi(hbuf);
            ramfb_reconfigure(cw, ch);
        }
    }

    /* ── 6c.6 User accounts ─────────────────────────────────────────── */
    users_init();
    boot_prof_stamp("users");

    /* ── 6d. Network (after storage so it can read /config/network.conf) */
    /*
     * net_init() runs VirtIO net setup, reads /config/network.conf to
     * decide DHCP vs. static IP, then busy-polls DHCP if needed.
     * DHCP completes before IRQs are enabled so g_our_ip is valid when
     * userspace starts.  The timer IRQ calls net_rx_poll() at 100 Hz.
     */
    net_init();
    boot_prof_stamp("net+dhcp");

    /* ── 7. Scheduler + Pipe + WM subsystem ────────────────────────── */
    pipe_init();
    wm_init();
    scheduler_init();
    scheduler_add_idle();
    boot_prof_stamp("scheduler+wm");

    /* ── 8. initrd ──────────────────────────────────────────────────── */
    initrd_init();
    boot_prof_stamp("initrd");

    /* ── 9. Spawn first user process from initrd ELF ───────────────── */
    /*
     * process_spawn() locates "init" in the CPIO archive, loads its ELF
     * segments into the user region (0x70000000+), and creates a kernel
     * task whose entry is user_task_trampoline → launch_el0(entry, sp).
     */
    if (process_spawn("/init") != 0)
        kpanic("kernel_main: failed to spawn /init\n");
    boot_prof_stamp("spawn_init");

    /* ── 10. Enable IRQs and enter idle loop ────────────────────────── */
    /* Seed the tick counter with the real elapsed boot time so that
     * userspace uptime counts from QEMU launch, not from this moment. */
    timer_seed_from_cntpct();
    boot_prof_print();   /* print boot timeline before entering idle */
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
