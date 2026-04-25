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

    # Filesystem (initrd + ELF loader)
    fs/initrd.c
    fs/elf.c

    # Drivers — character
    drivers/char/uart_pl011.c

    # Drivers — interrupt controller
    drivers/irq/gic_v2.c

    # Drivers — timer
    drivers/timer/arm_timer.c

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
