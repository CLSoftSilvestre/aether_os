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

    # Memory management
    mm/pmm.c
    mm/kmalloc.c

    # Drivers
    drivers/char/uart_pl011.c
    drivers/irq/gic_v2.c
    drivers/timer/arm_timer.c
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
