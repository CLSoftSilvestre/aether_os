cmake_minimum_required(VERSION 3.20)

# ── Shared compile flags for all userspace targets ───────────────────────────
set(USER_COMPILE_FLAGS
    -ffreestanding
    -fno-builtin
    -nostdinc
    -fno-stack-protector
    -fpic
)

set(USER_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/lib/include)

# ── Userspace linker script ──────────────────────────────────────────────────
set(USER_LINKER_SCRIPT ${CMAKE_CURRENT_SOURCE_DIR}/lib/user.ld)

# ── libaether — freestanding C runtime ───────────────────────────────────────
add_library(libaether STATIC
    lib/libaether/string.c
    lib/libaether/stdio.c
    lib/libaether/stdlib.c
    lib/libaether/gfx.c
)

target_include_directories(libaether PUBLIC
    lib/include
)

target_compile_options(libaether PRIVATE ${USER_COMPILE_FLAGS})

# ── init / aesh — first user-space process (PID 1) ───────────────────────────
add_executable(user_init
    apps/init/main.c
    lib/crt0.S
)

target_include_directories(user_init PRIVATE
    lib/include
)

target_compile_options(user_init PRIVATE ${USER_COMPILE_FLAGS})

target_link_libraries(user_init PRIVATE libaether)

target_link_options(user_init PRIVATE
    -T ${USER_LINKER_SCRIPT}
    -nostdlib
    -static
    -Wl,--build-id=none
)

set_target_properties(user_init PROPERTIES
    OUTPUT_NAME "init"
    SUFFIX      ""
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/initrd_staging
)

# ── initrd CPIO archive ───────────────────────────────────────────────────────
#
# Step 1 — package all files from initrd_staging/ into a CPIO newc archive.
# Step 2 — convert the CPIO binary to a linkable AArch64 object file using
#          objcopy -I binary.  This embeds the archive in the kernel ELF with:
#            _binary_initrd_cpio_start
#            _binary_initrd_cpio_end

set(INITRD_STAGING ${CMAKE_BINARY_DIR}/initrd_staging)
set(INITRD_CPIO    ${CMAKE_BINARY_DIR}/initrd.cpio)
set(INITRD_OBJ     ${CMAKE_BINARY_DIR}/initrd.o)

# Copy static initrd files (motd.txt, etc.) into the staging directory.
set(INITRD_FILES_SRC ${CMAKE_CURRENT_SOURCE_DIR}/initrd_files)
add_custom_command(
    OUTPUT  ${INITRD_STAGING}/motd.txt
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${INITRD_FILES_SRC} ${INITRD_STAGING}
    DEPENDS ${INITRD_FILES_SRC}/motd.txt
    COMMENT "Copying initrd static files"
)

# Step 1: build the CPIO archive from the staging directory
add_custom_command(
    OUTPUT  ${INITRD_CPIO}
    COMMAND ${CMAKE_SOURCE_DIR}/scripts/make_initrd.sh
                ${INITRD_STAGING}
                ${INITRD_CPIO}
    DEPENDS user_init ${INITRD_STAGING}/motd.txt
    COMMENT "Packaging initrd.cpio"
)

# Step 2: convert CPIO binary → AArch64 object file.
# IMPORTANT: pass basename paths so objcopy generates short symbol names:
#   _binary_initrd_cpio_start / _binary_initrd_cpio_end
# (Using full absolute paths would bake the whole build path into the names.)
add_custom_command(
    OUTPUT  ${INITRD_OBJ}
    COMMAND ${CMAKE_OBJCOPY}
                -I binary
                -O elf64-littleaarch64
                -B aarch64
                --rename-section .data=.rodata,alloc,load,readonly,data,contents
                initrd.cpio
                initrd.o
    DEPENDS ${INITRD_CPIO}
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT "Generating initrd.o (embedded CPIO)"
)

add_custom_target(initrd ALL DEPENDS ${INITRD_OBJ})
