cmake_minimum_required(VERSION 3.20)

set(KERNEL_SOURCES
    # Boot — first in link order so _start lands at 0x40000000
    arch/arm64/boot/boot.S

    # Exception vector table
    arch/arm64/exceptions.S

    # Context switch (cooperative scheduler)
    arch/arm64/context_switch.S

    # Core kernel
    core/main.c
    core/printk.c
    core/exceptions.c
    core/scheduler.c
    core/syscall.c
    core/process.c
    core/pipe.c
    core/wm.c

    # Memory management
    mm/pmm.c
    mm/kmalloc.c
    mm/vmm.c

    # Filesystem (initrd + ELF loader + FAT32 + VFS)
    fs/initrd.c
    fs/elf.c
    fs/fat32.c
    fs/aetherfs.c
    fs/vfs.c

    # Drivers — block storage (Phase 5.2)
    drivers/block/virtio_blk.c

    # Drivers — character
    drivers/char/uart_pl011.c

    # Drivers — interrupt controller
    drivers/irq/gic_v2.c

    # Drivers — timer
    drivers/timer/arm_timer.c

    # Drivers — RTC (PL031 — QEMU virt, backed by host clock)
    drivers/rtc/pl031.c

    # Drivers — video (framebuffer)
    drivers/video/fw_cfg.c
    drivers/video/ramfb.c
    drivers/video/fb.c
    drivers/video/font.c
    drivers/video/fb_console.c
    drivers/video/cursor.c

    # Drivers — PCI (ECAM config-space access + BAR assignment)
    drivers/pci/pci_ecam.c

    # Drivers — input (Phase 4.5)
    drivers/input/pl050_kbd.c
    drivers/input/pl050_mouse.c
    drivers/input/virtio_input.c

    # Drivers — USB OHCI + HID (Phase 4.6)
    drivers/usb/ohci.c

    # Drivers — GPU / V3D (Phase 6.1)
    drivers/gpu/mailbox.c
    drivers/gpu/v3d.c

    # Drivers — Power Management (Phase 6.2)
    drivers/power/cpufreq.c
    drivers/power/thermal.c
    drivers/power/dpms.c
    drivers/power/boot_prof.c

    # Drivers — VirtIO net (Phase 5.1)
    drivers/net/virtio_net.c

    # Network stack (Phase 5.1)
    net/net.c
    net/ethernet.c
    net/ip.c
    net/udp.c
    net/tcp.c
    net/dhcp.c
    net/dns.c
    net/socket.c
)

set(LINKER_SCRIPT ${CMAKE_CURRENT_SOURCE_DIR}/arch/arm64/boot/linker.ld)

add_executable(aether_kernel ${KERNEL_SOURCES})

target_include_directories(aether_kernel PRIVATE
    ${CMAKE_SOURCE_DIR}/kernel/include
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_options(aether_kernel PRIVATE
    -T ${LINKER_SCRIPT}
    -nostdlib
    -static
    -Wl,--build-id=none
)

set_target_properties(aether_kernel PROPERTIES
    LINK_DEPENDS ${LINKER_SCRIPT}
    OUTPUT_NAME  aether_kernel
    SUFFIX       ".elf"
)
