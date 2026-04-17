# cmake/toolchain-aarch64.cmake
# Cross-compilation toolchain file for AArch64 bare-metal.
# Tells CMake to use aarch64-elf-* tools instead of the host (x86_64) tools.

set(CMAKE_SYSTEM_NAME Generic)       # "Generic" = no OS (bare metal)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

# Toolchain prefix installed by Homebrew
set(TOOLCHAIN_PREFIX aarch64-elf)

set(CMAKE_C_COMPILER   ${TOOLCHAIN_PREFIX}-gcc)
set(CMAKE_ASM_COMPILER ${TOOLCHAIN_PREFIX}-gcc)
set(CMAKE_OBJCOPY      ${TOOLCHAIN_PREFIX}-objcopy)
set(CMAKE_OBJDUMP      ${TOOLCHAIN_PREFIX}-objdump)
set(CMAKE_SIZE         ${TOOLCHAIN_PREFIX}-size)

# Tell CMake not to try to link test executables against the host system
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# Core compiler flags for AArch64 bare-metal
set(ARCH_FLAGS
    "-march=armv8-a"           # Target ARMv8-A (Cortex-A72/A76 compatible)
    "-mtune=cortex-a76"        # Optimise instruction scheduling for Pi 5 CPU
    "-mgeneral-regs-only"      # No SIMD/FP registers in kernel (avoids saving them on exceptions)
    "-ffreestanding"           # No standard library assumptions
    "-fno-common"              # No tentative definitions — explicit is safer
    "-fno-builtin"             # Don't replace our functions with builtins
    "-fno-stack-protector"     # We implement our own later
    "-fno-pie" "-fno-pic"      # No position-independent code in kernel
    "-nostdlib"                # Don't link against any standard library
)

string(JOIN " " ARCH_FLAGS_STR ${ARCH_FLAGS})

set(CMAKE_C_FLAGS_INIT   "${ARCH_FLAGS_STR} -std=gnu17")
set(CMAKE_ASM_FLAGS_INIT "${ARCH_FLAGS_STR}")
