cmake_minimum_required(VERSION 3.20)

# Project declaration — C and ASM languages
project(aether_os C ASM)

# ── Build type default ──────────────────────────────────────────────────────
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

# ── Warning flags ───────────────────────────────────────────────────────────
add_compile_options(
    -Wall
    -Wextra
    -Wshadow
    -Wundef
    -Wno-unused-parameter     # normal in early stubs
)

# ── Debug vs Release ────────────────────────────────────────────────────────
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_compile_options(-O0 -g3)
else()
    add_compile_options(-O2)
endif()

# ── Global include paths ────────────────────────────────────────────────────
include_directories(
    ${CMAKE_SOURCE_DIR}/kernel/include
)

# ── Subdirectories ──────────────────────────────────────────────────────────
# userspace must be added first — its initrd.o is linked into the kernel.
add_subdirectory(userspace)
add_subdirectory(kernel)

# ── Wire initrd.o into the kernel binary ────────────────────────────────────
# The CPIO archive (built by the userspace target) is embedded via an object
# file generated with objcopy -I binary.  The kernel references these symbols:
#   _binary_initrd_cpio_start / _binary_initrd_cpio_end
set(INITRD_OBJ ${CMAKE_BINARY_DIR}/initrd.o)
add_dependencies(aether_kernel initrd)
target_link_options(aether_kernel PRIVATE ${INITRD_OBJ})
# Tell CMake to re-link the kernel whenever initrd.o changes on disk.
set_property(TARGET aether_kernel APPEND PROPERTY LINK_DEPENDS ${INITRD_OBJ})

# ── Bootable image target ───────────────────────────────────────────────────
# After building, convert the ELF to a raw binary that QEMU / Pi can load
add_custom_target(image ALL
    COMMAND ${CMAKE_OBJCOPY} -O binary
            ${CMAKE_BINARY_DIR}/kernel/aether_kernel.elf
            ${CMAKE_BINARY_DIR}/kernel8.img
    DEPENDS aether_kernel
    COMMENT "Generating kernel8.img"
)
