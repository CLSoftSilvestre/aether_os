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
add_subdirectory(kernel)

# ── Bootable image target ───────────────────────────────────────────────────
# After building, convert the ELF to a raw binary that QEMU / Pi can load
add_custom_target(image ALL
    COMMAND ${CMAKE_OBJCOPY} -O binary
            ${CMAKE_BINARY_DIR}/kernel/aether_kernel.elf
            ${CMAKE_BINARY_DIR}/kernel8.img
    DEPENDS aether_kernel
    COMMENT "Generating kernel8.img"
)
