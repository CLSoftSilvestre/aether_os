cmake_minimum_required(VERSION 3.20)

# ── Source files ─────────────────────────────────────────────────────────────
set(KERNEL_SOURCES
    # Boot — must be first so _start lands at load address 0x40000000
    arch/arm64/boot/boot.S

    # Exception vector table — must come early (2KB alignment enforced by .align 11)
    arch/arm64/exceptions.S

    # Core kernel
    core/main.c
    core/printk.c
    core/exceptions.c

    # Drivers
    drivers/char/uart_pl011.c
    drivers/irq/gic_v2.c
    drivers/timer/arm_timer.c
)

# ── Linker script ─────────────────────────────────────────────────────────────
set(LINKER_SCRIPT ${CMAKE_CURRENT_SOURCE_DIR}/arch/arm64/boot/linker.ld)

# ── Kernel ELF target ─────────────────────────────────────────────────────────
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
