cmake_minimum_required(VERSION 3.20)

# -mgeneral-regs-only is set globally by the toolchain for the kernel.
# Userspace processes may use FP/SIMD registers, so strip it here.
string(REPLACE "-mgeneral-regs-only" "" CMAKE_C_FLAGS   "${CMAKE_C_FLAGS}")
string(REPLACE "-mgeneral-regs-only" "" CMAKE_ASM_FLAGS "${CMAKE_ASM_FLAGS}")

# ── Shared compile flags for all userspace targets ───────────────────────────
set(USER_COMPILE_FLAGS
    -ffreestanding
    -fno-builtin
    -nostdinc
    -fno-stack-protector
    -fno-pic
)

set(USER_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/lib/include)

# ── Userspace linker script ──────────────────────────────────────────────────
set(USER_LINKER_SCRIPT ${CMAKE_CURRENT_SOURCE_DIR}/lib/user.ld)

# ── libaether — freestanding C runtime ───────────────────────────────────────
add_library(libaether STATIC
    lib/libaether/string.c
    lib/libaether/stdio.c
    lib/libaether/stdlib.c
    lib/libaether/math.c
    lib/libaether/gfx.c
    lib/libaether/gpu.c
    lib/libaether/manifest.c
    lib/libaether/vfs_mounts.c
    lib/libaether/setjmp.S
)

target_include_directories(libaether PUBLIC
    lib/include
)

target_compile_options(libaether PRIVATE ${USER_COMPILE_FLAGS})

# ── libwidget — retained widget toolkit (Phase 5.3) ──────────────────────────
add_library(libwidget STATIC
    lib/libwidget/widget.c
    lib/libwidget/panel.c
    lib/libwidget/button.c
    lib/libwidget/label.c
    lib/libwidget/textinput.c
    lib/libwidget/textarea.c
    lib/libwidget/listview.c
    lib/libwidget/scrollbar.c
    lib/libwidget/checkbox.c
    lib/libwidget/treeview.c
)

target_include_directories(libwidget PUBLIC
    lib/include
)

target_compile_options(libwidget PRIVATE ${USER_COMPILE_FLAGS})

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

# ── aether_term — Phase 4.4 terminal process ─────────────────────────────────
add_executable(user_aether_term
    apps/aether_term/main.c
    lib/crt0.S
)
target_include_directories(user_aether_term PRIVATE lib/include)
target_compile_options(user_aether_term PRIVATE ${USER_COMPILE_FLAGS})
target_link_libraries(user_aether_term PRIVATE libaether)
target_link_options(user_aether_term PRIVATE
    -T ${USER_LINKER_SCRIPT} -nostdlib -static -Wl,--build-id=none
)
set_target_properties(user_aether_term PROPERTIES
    OUTPUT_NAME "aether_term" SUFFIX ""
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/initrd_staging
)

# ── statusbar — Phase 4.4 sidebar daemon ─────────────────────────────────────
add_executable(user_statusbar
    apps/statusbar/main.c
    lib/crt0.S
)
target_include_directories(user_statusbar PRIVATE lib/include)
target_compile_options(user_statusbar PRIVATE ${USER_COMPILE_FLAGS})
target_link_libraries(user_statusbar PRIVATE libaether)
target_link_options(user_statusbar PRIVATE
    -T ${USER_LINKER_SCRIPT} -nostdlib -static -Wl,--build-id=none
)
set_target_properties(user_statusbar PROPERTIES
    OUTPUT_NAME "statusbar" SUFFIX ""
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/initrd_staging
)

# ── files — Phase 5.6 two-pane file explorer ─────────────────────────────────
add_executable(user_files
    apps/files/main.c
    lib/crt0.S
)
target_include_directories(user_files PRIVATE lib/include)
target_compile_options(user_files PRIVATE ${USER_COMPILE_FLAGS})
target_link_libraries(user_files PRIVATE libwidget libaether)
target_link_options(user_files PRIVATE
    -T ${USER_LINKER_SCRIPT} -nostdlib -static -Wl,--build-id=none
)
set_target_properties(user_files PROPERTIES
    OUTPUT_NAME "files" SUFFIX ""
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/initrd_staging
)

# ── textviewer — Phase 5.4 text editor ───────────────────────────────────────
add_executable(user_textviewer
    apps/textviewer/main.c
    lib/crt0.S
)
target_include_directories(user_textviewer PRIVATE lib/include)
target_compile_options(user_textviewer PRIVATE ${USER_COMPILE_FLAGS})
target_link_libraries(user_textviewer PRIVATE libwidget libaether)
target_link_options(user_textviewer PRIVATE
    -T ${USER_LINKER_SCRIPT} -nostdlib -static -Wl,--build-id=none
)
set_target_properties(user_textviewer PROPERTIES
    OUTPUT_NAME "textviewer" SUFFIX ""
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/initrd_staging
)

# ── calculator — basic four-function calculator ───────────────────────────────
add_executable(user_calculator
    apps/calculator/main.c
    lib/crt0.S
)
target_include_directories(user_calculator PRIVATE lib/include)
target_compile_options(user_calculator PRIVATE ${USER_COMPILE_FLAGS})
target_link_libraries(user_calculator PRIVATE libwidget libaether)
target_link_options(user_calculator PRIVATE
    -T ${USER_LINKER_SCRIPT} -nostdlib -static -Wl,--build-id=none
)
set_target_properties(user_calculator PROPERTIES
    OUTPUT_NAME "calculator" SUFFIX ""
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/initrd_staging
)

# ── tictactoe — Tic-Tac-Toe game (Human vs Human / Human vs CPU) ─────────────
add_executable(user_tictactoe
    apps/tictactoe/main.c
    lib/crt0.S
)
target_include_directories(user_tictactoe PRIVATE lib/include)
target_compile_options(user_tictactoe PRIVATE ${USER_COMPILE_FLAGS})
target_link_libraries(user_tictactoe PRIVATE libwidget libaether)
target_link_options(user_tictactoe PRIVATE
    -T ${USER_LINKER_SCRIPT} -nostdlib -static -Wl,--build-id=none
)
set_target_properties(user_tictactoe PROPERTIES
    OUTPUT_NAME "tictactoe" SUFFIX ""
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/initrd_staging
)

# ── widget_demo — Phase 5.3 widget toolkit demo ───────────────────────────────
add_executable(user_widget_demo
    apps/widget_demo/main.c
    lib/crt0.S
)
target_include_directories(user_widget_demo PRIVATE lib/include)
target_compile_options(user_widget_demo PRIVATE ${USER_COMPILE_FLAGS})
target_link_libraries(user_widget_demo PRIVATE libwidget libaether)
target_link_options(user_widget_demo PRIVATE
    -T ${USER_LINKER_SCRIPT} -nostdlib -static -Wl,--build-id=none
)
set_target_properties(user_widget_demo PROPERTIES
    OUTPUT_NAME "widget_demo" SUFFIX ""
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/initrd_staging
)

# ── aether_editor — Phase 5.5 libwidget script editor ────────────────────────
add_executable(user_aether_editor
    apps/aether_editor/main.c
    lib/crt0.S
)
target_include_directories(user_aether_editor PRIVATE lib/include)
target_compile_options(user_aether_editor PRIVATE ${USER_COMPILE_FLAGS})
target_link_libraries(user_aether_editor PRIVATE libwidget libaether)
target_link_options(user_aether_editor PRIVATE
    -T ${USER_LINKER_SCRIPT} -nostdlib -static -Wl,--build-id=none
)
set_target_properties(user_aether_editor PROPERTIES
    OUTPUT_NAME "aether_editor" SUFFIX ""
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/initrd_staging
)

# ── liblua54 + aether_interp — Phase 5.5 Lua 5.4 interpreter ─────────────────
#
# Prerequisites: run  scripts/fetch_lua.sh  once to populate vendor/lua54/.
# If the Lua source is absent the targets are skipped (no hard error) so the
# rest of the build still works.

set(LUA_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/vendor/lua54)
set(LUA_CFG_DIR ${LUA_SRC_DIR}/aether)  # contains our luaconf.h override

# Lua VM + safe standard library source files.
# We exclude liolib/loadlib/loslib/ldblib (POSIX-heavy OS interfaces) and
# replace linit.c with linit_aether.c which only opens the safe subset.
set(LUA_CORE_SOURCES
    ${LUA_SRC_DIR}/lapi.c
    ${LUA_SRC_DIR}/lcode.c
    ${LUA_SRC_DIR}/lctype.c
    ${LUA_SRC_DIR}/ldebug.c
    ${LUA_SRC_DIR}/ldo.c
    ${LUA_SRC_DIR}/ldump.c
    ${LUA_SRC_DIR}/lfunc.c
    ${LUA_SRC_DIR}/lgc.c
    ${LUA_SRC_DIR}/llex.c
    ${LUA_SRC_DIR}/lmem.c
    ${LUA_SRC_DIR}/lobject.c
    ${LUA_SRC_DIR}/lopcodes.c
    ${LUA_SRC_DIR}/lparser.c
    ${LUA_SRC_DIR}/lstate.c
    ${LUA_SRC_DIR}/lstring.c
    ${LUA_SRC_DIR}/ltable.c
    ${LUA_SRC_DIR}/ltm.c
    ${LUA_SRC_DIR}/lundump.c
    ${LUA_SRC_DIR}/lvm.c
    ${LUA_SRC_DIR}/lzio.c
    ${LUA_SRC_DIR}/lauxlib.c
    ${LUA_SRC_DIR}/lbaselib.c
    ${LUA_SRC_DIR}/lcorolib.c
    ${LUA_SRC_DIR}/lmathlib.c
    ${LUA_SRC_DIR}/lstrlib.c
    ${LUA_SRC_DIR}/ltablib.c
    ${LUA_SRC_DIR}/lutf8lib.c
    ${LUA_CFG_DIR}/linit_aether.c
)

# Check if Lua source is present
if(EXISTS ${LUA_SRC_DIR}/lapi.c)

    add_library(liblua54 STATIC ${LUA_CORE_SOURCES})

    # Include order: aether overrides first, then Lua headers, then our
    # freestanding C headers (limits.h, float.h, setjmp.h, math.h, ctype.h, ...).
    target_include_directories(liblua54 PUBLIC
        ${LUA_CFG_DIR}  # luaconf.h override + stub headers (errno.h, locale.h, ...)
        ${LUA_SRC_DIR}  # lua.h, lauxlib.h, lualib.h, etc.
        lib/include     # limits.h, float.h, setjmp.h, math.h, ctype.h, stdint.h, ...
    )

    target_compile_options(liblua54 PRIVATE
        ${USER_COMPILE_FLAGS}
        -nostdinc
        -Wno-implicit-function-declaration
        -Wno-return-type
        -Wno-int-conversion
        -Wno-sign-compare
        -Wno-undef
        -DLUA_COMPAT_5_3
    )

    # aether_interp binary
    add_executable(user_aether_interp
        apps/aether_interp/main.c
        lib/crt0.S
    )
    target_include_directories(user_aether_interp PRIVATE
        ${LUA_CFG_DIR}
        ${LUA_SRC_DIR}
        lib/include
    )
    target_compile_options(user_aether_interp PRIVATE
        ${USER_COMPILE_FLAGS}
        -nostdinc
    )
    target_link_libraries(user_aether_interp PRIVATE liblua54 libaether)
    target_link_options(user_aether_interp PRIVATE
        -T ${USER_LINKER_SCRIPT} -nostdlib -static -Wl,--build-id=none
    )
    set_target_properties(user_aether_interp PROPERTIES
        OUTPUT_NAME "aether_interp" SUFFIX ""
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/initrd_staging
    )

    set(INTERP_TARGET user_aether_interp)

else()
    message(STATUS
        "Lua 5.4 source not found in vendor/lua54/ — "
        "run scripts/fetch_lua.sh then re-configure.  "
        "aether_interp will NOT be built.")
    set(INTERP_TARGET "")
endif()

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
            ${INITRD_FILES_SRC}/apps/tictactoe.app
    COMMENT "Copying initrd static files"
)

# Step 1: build the CPIO archive from the staging directory
add_custom_command(
    OUTPUT  ${INITRD_CPIO}
    COMMAND ${CMAKE_SOURCE_DIR}/scripts/make_initrd.sh
                ${INITRD_STAGING}
                ${INITRD_CPIO}
    DEPENDS user_init user_aether_term user_statusbar user_files user_textviewer
            user_calculator user_tictactoe user_widget_demo user_aether_editor
            ${INTERP_TARGET}
            ${INITRD_STAGING}/motd.txt
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
