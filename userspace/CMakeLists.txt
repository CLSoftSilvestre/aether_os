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
    lib/libaether/gfx_toolbar_icons.c
    lib/libaether/gpu.c
    lib/libaether/manifest.c
    lib/libaether/vfs_mounts.c
    lib/libaether/icon_cache.c
    lib/libaether/setjmp.S
)

target_include_directories(libaether PUBLIC
    lib/include
)

target_compile_options(libaether PRIVATE ${USER_COMPILE_FLAGS})

# Phase 7.2.4: wire FreeType into gfx.c when the font wrapper is built.
# libaether_font is defined later in this file; we use a deferred hook via
# cmake_language(DEFER) so the dependency is added after the target exists.
# We also export -DGFX_HAVE_FREETYPE=1 so gfx.c can conditionally compile the
# FreeType paths, and add the aether_font header directory.
function(libaether_wire_freetype)
    if(TARGET libaether_font)
        target_compile_definitions(libaether PRIVATE GFX_HAVE_FREETYPE=1)
        target_include_directories(libaether PRIVATE lib/aether_font)
        target_link_libraries(libaether PUBLIC libaether_font libaether_posix)
        # libaether and libaether_posix both implement the standard C library
        # (string, stdio, stdlib).  libaether is linked first so its versions
        # win; the only exception is malloc/free/realloc which are __weak__ in
        # libaether so libaether_posix's proper free-list heap is used instead
        # of the 256 KB bump allocator (FreeType requires real free()).
        target_link_options(libaether INTERFACE -Wl,--allow-multiple-definition)
    endif()
endfunction()
cmake_language(DEFER CALL libaether_wire_freetype)

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
    lib/libwidget/progress.c
    lib/libwidget/anim.c
    lib/libwidget/filedlg.c
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

target_link_libraries(user_init PRIVATE libwidget libaether)

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

# ── dock — WM8 dock subprocess (spawned by init after compositor) ─────────────
add_executable(user_dock
    apps/dock/main.c
    lib/crt0.S
)
target_include_directories(user_dock PRIVATE lib/include)
target_compile_options(user_dock PRIVATE ${USER_COMPILE_FLAGS})
target_link_libraries(user_dock PRIVATE libwidget libaether)
target_link_options(user_dock PRIVATE
    -T ${USER_LINKER_SCRIPT} -nostdlib -static -Wl,--build-id=none
)
set_target_properties(user_dock PROPERTIES
    OUTPUT_NAME "dock" SUFFIX ""
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/initrd_staging
)

# ── topbar — macOS-style menu bar daemon (spawned by init after compositor) ───
add_executable(user_topbar
    apps/topbar/main.c
    lib/crt0.S
)
target_include_directories(user_topbar PRIVATE lib/include)
target_compile_options(user_topbar PRIVATE ${USER_COMPILE_FLAGS})
target_link_libraries(user_topbar PRIVATE libwidget libaether)
target_link_options(user_topbar PRIVATE
    -T ${USER_LINKER_SCRIPT} -nostdlib -static -Wl,--build-id=none
)
set_target_properties(user_topbar PROPERTIES
    OUTPUT_NAME "topbar" SUFFIX ""
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/initrd_staging
)

# ── notif_center — notification panel (spawned by topbar on bell click) ───────
add_executable(user_notif_center
    apps/notif_center/main.c
    lib/crt0.S
)
target_include_directories(user_notif_center PRIVATE lib/include)
target_compile_options(user_notif_center PRIVATE ${USER_COMPILE_FLAGS})
target_link_libraries(user_notif_center PRIVATE libwidget libaether)
target_link_options(user_notif_center PRIVATE
    -T ${USER_LINKER_SCRIPT} -nostdlib -static -Wl,--build-id=none
)
set_target_properties(user_notif_center PROPERTIES
    OUTPUT_NAME "notif_center" SUFFIX ""
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/initrd_staging
)

# ── desktop — Lumina desktop layer: wallpaper + icons (spawned by init) ───────
add_executable(user_desktop
    apps/desktop/main.c
    lib/crt0.S
)
target_include_directories(user_desktop PRIVATE lib/include)
target_compile_options(user_desktop PRIVATE ${USER_COMPILE_FLAGS})
target_link_libraries(user_desktop PRIVATE libwidget libaether)
target_link_options(user_desktop PRIVATE
    -T ${USER_LINKER_SCRIPT} -nostdlib -static -Wl,--build-id=none
)
set_target_properties(user_desktop PROPERTIES
    OUTPUT_NAME "desktop" SUFFIX ""
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/initrd_staging
)

# ── compositor — Lumina compositing window manager (wmanager branch) ─────────
add_executable(user_compositor
    apps/compositor/main.c
    lib/crt0.S
)
target_include_directories(user_compositor PRIVATE lib/include)
target_compile_options(user_compositor PRIVATE ${USER_COMPILE_FLAGS}
    -DAETHER_GPU_COMPOSITOR   # WM3: batch GPU compositing path
    -DAETHER_KAWASE_BLUR      # WM4: Dual Kawase blur (compositor passes blur_radius)
    -DAETHER_SPRING_ANIM      # WM5: spring-physics open animations (Q12 fixed-point)
)
target_link_libraries(user_compositor PRIVATE libaether)
target_link_options(user_compositor PRIVATE
    -T ${USER_LINKER_SCRIPT} -nostdlib -static -Wl,--build-id=none
)
set_target_properties(user_compositor PROPERTIES
    OUTPUT_NAME "compositor" SUFFIX ""
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/initrd_staging
)

# ── aether_term — Phase 4.4 terminal process ─────────────────────────────────
add_executable(user_aether_term
    apps/aether_term/main.c
    lib/crt0.S
)
target_include_directories(user_aether_term PRIVATE lib/include)
target_compile_options(user_aether_term PRIVATE ${USER_COMPILE_FLAGS})
target_link_libraries(user_aether_term PRIVATE libwidget libaether)
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

# ── login — Boot-time login screen ───────────────────────────────────────────
add_executable(user_login
    apps/login/main.c
    lib/crt0.S
)
target_include_directories(user_login PRIVATE lib/include)
target_compile_options(user_login PRIVATE ${USER_COMPILE_FLAGS})
target_link_libraries(user_login PRIVATE libwidget libaether)
target_link_options(user_login PRIVATE
    -T ${USER_LINKER_SCRIPT} -nostdlib -static -Wl,--build-id=none
)
set_target_properties(user_login PROPERTIES
    OUTPUT_NAME "login" SUFFIX ""
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/initrd_staging
)

# ── sys_prefs — System Preferences application ───────────────────────────────
add_executable(user_sys_prefs
    apps/sys_prefs/main.c
    lib/config.c
    lib/crt0.S
)
target_include_directories(user_sys_prefs PRIVATE lib/include)
target_compile_options(user_sys_prefs PRIVATE ${USER_COMPILE_FLAGS})
target_link_libraries(user_sys_prefs PRIVATE libwidget libaether)
target_link_options(user_sys_prefs PRIVATE
    -T ${USER_LINKER_SCRIPT} -nostdlib -static -Wl,--build-id=none
)
set_target_properties(user_sys_prefs PROPERTIES
    OUTPUT_NAME "sys_prefs" SUFFIX ""
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

# ── aether_http — HTTP/1.1 client (Phase 5.1) ────────────────────────────────
add_executable(user_aether_http
    apps/aether_http/main.c
    lib/crt0.S
)
target_include_directories(user_aether_http PRIVATE lib/include)
target_compile_options(user_aether_http PRIVATE ${USER_COMPILE_FLAGS})
target_link_libraries(user_aether_http PRIVATE libaether)
target_link_options(user_aether_http PRIVATE
    -T ${USER_LINKER_SCRIPT} -nostdlib -static -Wl,--build-id=none
)
set_target_properties(user_aether_http PROPERTIES
    OUTPUT_NAME "http" SUFFIX ""
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

# ── aether_top — task manager / process monitor ──────────────────────────────
add_executable(user_aether_top
    apps/aether_top/main.c
    lib/crt0.S
)
target_include_directories(user_aether_top PRIVATE lib/include)
target_compile_options(user_aether_top PRIVATE ${USER_COMPILE_FLAGS})
target_link_libraries(user_aether_top PRIVATE libwidget libaether)
target_link_options(user_aether_top PRIVATE
    -T ${USER_LINKER_SCRIPT} -nostdlib -static -Wl,--build-id=none
)
set_target_properties(user_aether_top PROPERTIES
    OUTPUT_NAME "aether_top" SUFFIX ""
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/initrd_staging
)

# ── telnet — RFC-854 Telnet client ───────────────────────────────────────────
add_executable(user_telnet
    apps/telnet/main.c
    lib/crt0.S
)
target_include_directories(user_telnet PRIVATE lib/include)
target_compile_options(user_telnet PRIVATE ${USER_COMPILE_FLAGS})
target_link_libraries(user_telnet PRIVATE libwidget libaether)
target_link_options(user_telnet PRIVATE
    -T ${USER_LINKER_SCRIPT} -nostdlib -static -Wl,--build-id=none
)
set_target_properties(user_telnet PROPERTIES
    OUTPUT_NAME "telnet" SUFFIX ""
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/initrd_staging
)

# ── aether_irc — IRC client (RFC 1459 / RFC 2812) ────────────────────────────

add_executable(user_aether_irc
    apps/aether_irc/main.c
    apps/aether_irc/irc.c
    lib/crt0.S
)
target_include_directories(user_aether_irc PRIVATE
    lib/include
    apps/aether_irc
)
target_compile_options(user_aether_irc PRIVATE ${USER_COMPILE_FLAGS})
target_link_libraries(user_aether_irc PRIVATE libwidget libaether)
target_link_options(user_aether_irc PRIVATE
    -T ${USER_LINKER_SCRIPT} -nostdlib -static -Wl,--build-id=none
)
set_target_properties(user_aether_irc PROPERTIES
    OUTPUT_NAME "aether_irc" SUFFIX ""
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/initrd_staging
)

# ── aether_mail — IMAP4rev1 + SMTP email client ──────────────────────────────
#
# Protocols : IMAP4rev1 (RFC 3501, port 993 IMAPS) + SMTP (RFC 5321, port 465)
# TLS       : wrapped via tls_aether / mbedTLS when vendor/mbedtls/ is present
# Storage   : account config at /email/config on FAT32; message cache planned
# Icons     : ICON_BTN_COMPOSE(12) … ICON_BTN_REFRESH(15) added to gfx.h

add_executable(user_aether_mail
    apps/aether_mail/main.c
    apps/aether_mail/imap.c
    apps/aether_mail/smtp.c
    apps/aether_mail/mail_store.c
    lib/crt0.S
)
target_include_directories(user_aether_mail PRIVATE
    lib/include
    apps/aether_mail
)
target_compile_options(user_aether_mail PRIVATE ${USER_COMPILE_FLAGS})
target_link_libraries(user_aether_mail PRIVATE libwidget libaether)
target_link_options(user_aether_mail PRIVATE
    -T ${USER_LINKER_SCRIPT} -nostdlib -static -Wl,--build-id=none
)
set_target_properties(user_aether_mail PROPERTIES
    OUTPUT_NAME "aether_mail" SUFFIX ""
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/initrd_staging
)

# TLS support: link vendor_mbedtls and expose tls_aether.h when mbedTLS present
if(MBEDTLS_TARGET)
    target_link_libraries(user_aether_mail PRIVATE ${MBEDTLS_TARGET} libaether_posix)
    target_compile_definitions(user_aether_mail PRIVATE HAVE_MBEDTLS=1)
    target_include_directories(user_aether_mail PRIVATE
        ${MBEDTLS_SRC_DIR}/include   # <mbedtls/ssl.h>
        ${MBEDTLS_BRIDGE}            # tls_aether.h, mbedtls_aether_config.h
    )
    message(STATUS "[AetherMail] TLS enabled via mbedTLS ${MBEDTLS_SRC_DIR}")
else()
    message(STATUS "[AetherMail] TLS disabled (vendor/mbedtls/ not found) — plain IMAP/SMTP only")
endif()

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

# ── libaether_posix — POSIX compatibility shim (Phase 7.0) ───────────────────
#
# Provides a full POSIX C API surface for porting external software (NetSurf,
# zlib, libpng, QuickJS, etc.) to AetherOS without modifying their source code.
#
# Compile flags: same USER_COMPILE_FLAGS but with -nostdinc satisfied by our
# own include/ directory (which shadows the partial lib/include/ headers).
# lib/include/ is still listed second so that <sys.h> resolves to the AetherOS
# syscall interface.

set(POSIX_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/vendor/libaether_posix)
set(POSIX_INC_DIR ${POSIX_SRC_DIR}/include)

add_library(libaether_posix STATIC
    ${POSIX_SRC_DIR}/errno.c
    ${POSIX_SRC_DIR}/memory.c
    ${POSIX_SRC_DIR}/string_posix.c
    ${POSIX_SRC_DIR}/stdio_posix.c
    ${POSIX_SRC_DIR}/time_posix.c
    ${POSIX_SRC_DIR}/pthread_stub.c
    ${POSIX_SRC_DIR}/socket_posix.c
    ${POSIX_SRC_DIR}/dirent_posix.c
    ${POSIX_SRC_DIR}/misc_posix.c
    ${POSIX_SRC_DIR}/setjmp_aarch64.S   # AArch64 setjmp/longjmp (required by libpng)
)

# Include order matters:
#   1. POSIX shim headers (complete, shadow lib/include for external apps)
#   2. lib/include (for <sys.h>, <input.h> — AetherOS syscall interface)
target_include_directories(libaether_posix PUBLIC
    ${POSIX_INC_DIR}       # POSIX-complete headers (first → shadows lib/include)
    lib/include            # sys.h, input.h — AetherOS specifics
)

target_compile_options(libaether_posix PRIVATE
    ${USER_COMPILE_FLAGS}
    -Wno-implicit-function-declaration
    -Wno-sign-compare
    -Wno-unused-parameter
    -Wno-unused-function
)

# ── posix_test — Phase 7.0 integration test (Task 7.0.8) ─────────────────────
#
# A mini HTTP wget that exercises all libaether_posix components.
# Launch inside AetherOS: posix_test http://10.0.2.2:8080/test.html

add_executable(user_posix_test
    apps/posix_test/main.c
    lib/crt0.S
)

target_include_directories(user_posix_test PRIVATE
    ${POSIX_INC_DIR}
    lib/include
)

target_compile_options(user_posix_test PRIVATE
    ${USER_COMPILE_FLAGS}
    -Wno-unused-parameter
)

target_link_libraries(user_posix_test PRIVATE libaether_posix)

target_link_options(user_posix_test PRIVATE
    -T ${USER_LINKER_SCRIPT} -nostdlib -static -Wl,--build-id=none
)

set_target_properties(user_posix_test PROPERTIES
    OUTPUT_NAME "posix_test" SUFFIX ""
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/initrd_staging
)

# ── vendor_zlib — zlib 1.3.1 (Phase 7.1) ────────────────────────────────────
#
# Prerequisites: run  scripts/fetch_zlib.sh  once to populate vendor/zlib/.
# gz* files (gzclose/gzlib/gzread/gzwrite) are omitted — libpng only uses the
# in-memory deflate/inflate API, not the gzip file layer.

set(ZLIB_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/vendor/zlib)

if(EXISTS ${ZLIB_SRC_DIR}/zlib.h)

    add_library(vendor_zlib STATIC
        ${ZLIB_SRC_DIR}/adler32.c
        ${ZLIB_SRC_DIR}/compress.c
        ${ZLIB_SRC_DIR}/crc32.c
        ${ZLIB_SRC_DIR}/deflate.c
        ${ZLIB_SRC_DIR}/infback.c
        ${ZLIB_SRC_DIR}/inffast.c
        ${ZLIB_SRC_DIR}/inflate.c
        ${ZLIB_SRC_DIR}/inftrees.c
        ${ZLIB_SRC_DIR}/trees.c
        ${ZLIB_SRC_DIR}/uncompr.c
        ${ZLIB_SRC_DIR}/zutil.c
    )

    target_include_directories(vendor_zlib PUBLIC
        ${ZLIB_SRC_DIR}      # zlib.h, zconf.h
        ${POSIX_INC_DIR}     # stddef.h, stdint.h, stdlib.h, string.h ...
        lib/include          # sys.h (AetherOS syscall interface)
    )

    target_compile_options(vendor_zlib PRIVATE
        ${USER_COMPILE_FLAGS}
        -DHAVE_UNISTD_H
        -Wno-implicit-function-declaration
        -Wno-unused-function
        -Wno-unused-variable
    )

    set(ZLIB_TARGET vendor_zlib)
    message(STATUS "zlib 1.3.x found — vendor_zlib will be built")

else()
    message(STATUS
        "zlib source not found in vendor/zlib/ — "
        "run scripts/fetch_zlib.sh then re-configure.  "
        "vendor_zlib / vendor_libpng / vendor_libjpeg / img_test will NOT be built.")
    set(ZLIB_TARGET "")
endif()

# ── vendor_libpng — libpng 1.6.x (Phase 7.1) ────────────────────────────────
#
# Prerequisites: fetch_zlib.sh + fetch_libpng.sh (which installs pnglibconf.h).
# ARM NEON files (arm/*.c) are excluded; PNG_ARM_NEON_OPT=0 set in pnglibconf.h
# by the fetch script.

set(LIBPNG_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/vendor/libpng)

if(EXISTS ${LIBPNG_SRC_DIR}/png.h AND ZLIB_TARGET)

    add_library(vendor_libpng STATIC
        ${LIBPNG_SRC_DIR}/png.c
        ${LIBPNG_SRC_DIR}/pngerror.c
        ${LIBPNG_SRC_DIR}/pngget.c
        ${LIBPNG_SRC_DIR}/pngmem.c
        ${LIBPNG_SRC_DIR}/pngpread.c
        ${LIBPNG_SRC_DIR}/pngread.c
        ${LIBPNG_SRC_DIR}/pngrio.c
        ${LIBPNG_SRC_DIR}/pngrtran.c
        ${LIBPNG_SRC_DIR}/pngrutil.c
        ${LIBPNG_SRC_DIR}/pngset.c
        ${LIBPNG_SRC_DIR}/pngtrans.c
        ${LIBPNG_SRC_DIR}/pngwio.c
        ${LIBPNG_SRC_DIR}/pngwrite.c
        ${LIBPNG_SRC_DIR}/pngwtran.c
        ${LIBPNG_SRC_DIR}/pngwutil.c
    )

    target_include_directories(vendor_libpng PUBLIC
        ${LIBPNG_SRC_DIR}    # png.h, pngconf.h, pnglibconf.h
        ${ZLIB_SRC_DIR}      # zlib.h (libpng includes it directly)
        ${POSIX_INC_DIR}
        lib/include
    )

    target_compile_options(vendor_libpng PRIVATE
        ${USER_COMPILE_FLAGS}
        -DPNG_ARM_NEON_OPT=0
        -Wno-implicit-function-declaration
        -Wno-unused-function
        -Wno-sign-compare
        -Wno-unused-but-set-variable
    )

    target_link_libraries(vendor_libpng PUBLIC vendor_zlib)

    set(LIBPNG_TARGET vendor_libpng)
    message(STATUS "libpng 1.6.x found — vendor_libpng will be built")

else()
    if(NOT ZLIB_TARGET)
        # Already warned above
    else()
        message(STATUS
            "libpng source not found in vendor/libpng/ — "
            "run scripts/fetch_libpng.sh then re-configure.")
    endif()
    set(LIBPNG_TARGET "")
endif()

# ── vendor_libjpeg — libjpeg 9f (Phase 7.1) ──────────────────────────────────
#
# IJG reference JPEG library, pure C.  jmemnobs.c uses malloc/free for the
# memory manager (no backing store) — matches our libaether_posix heap.
# Prerequisite: run scripts/fetch_libjpeg.sh once.

set(LIBJPEG_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/vendor/libjpeg)

if(EXISTS ${LIBJPEG_SRC_DIR}/jpeglib.h)

    add_library(vendor_libjpeg STATIC
        ${LIBJPEG_SRC_DIR}/jaricom.c
        ${LIBJPEG_SRC_DIR}/jcapimin.c
        ${LIBJPEG_SRC_DIR}/jcapistd.c
        ${LIBJPEG_SRC_DIR}/jcarith.c
        ${LIBJPEG_SRC_DIR}/jccoefct.c
        ${LIBJPEG_SRC_DIR}/jccolor.c
        ${LIBJPEG_SRC_DIR}/jcdctmgr.c
        ${LIBJPEG_SRC_DIR}/jchuff.c
        ${LIBJPEG_SRC_DIR}/jcinit.c
        ${LIBJPEG_SRC_DIR}/jcmainct.c
        ${LIBJPEG_SRC_DIR}/jcmarker.c
        ${LIBJPEG_SRC_DIR}/jcmaster.c
        ${LIBJPEG_SRC_DIR}/jcomapi.c
        ${LIBJPEG_SRC_DIR}/jcparam.c
        ${LIBJPEG_SRC_DIR}/jcprepct.c
        ${LIBJPEG_SRC_DIR}/jcsample.c
        ${LIBJPEG_SRC_DIR}/jctrans.c
        ${LIBJPEG_SRC_DIR}/jdapimin.c
        ${LIBJPEG_SRC_DIR}/jdapistd.c
        ${LIBJPEG_SRC_DIR}/jdarith.c
        ${LIBJPEG_SRC_DIR}/jdatadst.c
        ${LIBJPEG_SRC_DIR}/jdatasrc.c
        ${LIBJPEG_SRC_DIR}/jdcoefct.c
        ${LIBJPEG_SRC_DIR}/jdcolor.c
        ${LIBJPEG_SRC_DIR}/jddctmgr.c
        ${LIBJPEG_SRC_DIR}/jdhuff.c
        ${LIBJPEG_SRC_DIR}/jdinput.c
        ${LIBJPEG_SRC_DIR}/jdmainct.c
        ${LIBJPEG_SRC_DIR}/jdmarker.c
        ${LIBJPEG_SRC_DIR}/jdmaster.c
        ${LIBJPEG_SRC_DIR}/jdmerge.c
        ${LIBJPEG_SRC_DIR}/jdpostct.c
        ${LIBJPEG_SRC_DIR}/jdsample.c
        ${LIBJPEG_SRC_DIR}/jdtrans.c
        ${LIBJPEG_SRC_DIR}/jerror.c
        ${LIBJPEG_SRC_DIR}/jfdctflt.c
        ${LIBJPEG_SRC_DIR}/jfdctfst.c
        ${LIBJPEG_SRC_DIR}/jfdctint.c
        ${LIBJPEG_SRC_DIR}/jidctflt.c
        ${LIBJPEG_SRC_DIR}/jidctfst.c
        ${LIBJPEG_SRC_DIR}/jidctint.c
        ${LIBJPEG_SRC_DIR}/jmemmgr.c
        ${LIBJPEG_SRC_DIR}/jmemnobs.c   # no-backing-store memory manager
        ${LIBJPEG_SRC_DIR}/jquant1.c
        ${LIBJPEG_SRC_DIR}/jquant2.c
        ${LIBJPEG_SRC_DIR}/jutils.c
    )

    target_include_directories(vendor_libjpeg PUBLIC
        ${LIBJPEG_SRC_DIR}   # jpeglib.h, jconfig.h, jmorecfg.h, jerror.h
        ${POSIX_INC_DIR}
        lib/include
    )

    target_compile_options(vendor_libjpeg PRIVATE
        ${USER_COMPILE_FLAGS}
        -Wno-implicit-function-declaration
        -Wno-sign-compare
        -Wno-unused-parameter
        -Wno-unused-variable
        -Wno-shift-negative-value
    )

    set(LIBJPEG_TARGET vendor_libjpeg)
    message(STATUS "libjpeg 9f found — vendor_libjpeg will be built")

else()
    message(STATUS
        "libjpeg source not found in vendor/libjpeg/ — "
        "run scripts/fetch_libjpeg.sh then re-configure.  "
        "img_test will NOT be built.")
    set(LIBJPEG_TARGET "")
endif()

# ── img_test — Phase 7.1 integration test (Task 7.1.4) ───────────────────────
#
# Decodes /images/test.png and /images/test.jpg from the FAT32 disk,
# verifies 4-quadrant pixel colors, prints pass/fail.
# Test images are generated by: python3 scripts/gen_test_images.py

if(LIBPNG_TARGET AND LIBJPEG_TARGET)

    add_executable(user_img_test
        apps/img_test/main.c
        lib/crt0.S
    )

    target_include_directories(user_img_test PRIVATE
        ${LIBPNG_SRC_DIR}
        ${LIBJPEG_SRC_DIR}
        ${ZLIB_SRC_DIR}
        ${POSIX_INC_DIR}
        lib/include
    )

    target_compile_options(user_img_test PRIVATE
        ${USER_COMPILE_FLAGS}
        -Wno-unused-parameter
    )

    target_link_libraries(user_img_test PRIVATE
        vendor_libpng
        vendor_libjpeg
        vendor_zlib
        libaether_posix
    )

    target_link_options(user_img_test PRIVATE
        -T ${USER_LINKER_SCRIPT} -nostdlib -static -Wl,--build-id=none
    )

    set_target_properties(user_img_test PROPERTIES
        OUTPUT_NAME "img_test" SUFFIX ""
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/initrd_staging
    )

    set(IMG_TEST_TARGET user_img_test)

else()
    set(IMG_TEST_TARGET "")
endif()

# ── aether_view — Phase 7.2 image viewer ─────────────────────────────────────
#
# Displays JPEG, PNG, and BMP files with zoom, pan, and Lumina glass UI.
# Requires libjpeg + libpng (same fetch scripts as img_test).
# Launched by the Files app when the user double-clicks an image file.

if(LIBPNG_TARGET AND LIBJPEG_TARGET)

    add_executable(user_aether_view
        apps/aether_view/main.c
        lib/crt0.S
    )

    target_include_directories(user_aether_view PRIVATE
        ${LIBPNG_SRC_DIR}
        ${LIBJPEG_SRC_DIR}
        ${ZLIB_SRC_DIR}
        ${POSIX_INC_DIR}
        lib/include
    )

    target_compile_options(user_aether_view PRIVATE
        ${USER_COMPILE_FLAGS}
        -Wno-unused-parameter
    )

    target_link_libraries(user_aether_view PRIVATE
        vendor_libpng
        vendor_libjpeg
        vendor_zlib
        libwidget
        libaether
    )

    target_link_options(user_aether_view PRIVATE
        -T ${USER_LINKER_SCRIPT} -nostdlib -static -Wl,--build-id=none
    )

    set_target_properties(user_aether_view PROPERTIES
        OUTPUT_NAME "aether_view" SUFFIX ""
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/initrd_staging
    )

else()
    message(STATUS "libjpeg/libpng not found — aether_view will NOT be built.")
endif()

# ── vendor_freetype — FreeType 2.13.x (Phase 7.2) ────────────────────────────
#
# Minimal source set for TrueType rendering: sfnt, truetype, smooth, autofit,
# psnames, raster.  LZW/BZip2/HarfBuzz/Brotli disabled via ftoption.h overrides
# written by fetch_freetype.sh.
#
# FT2_BUILD_LIBRARY unlocks the FT_LOCAL / internal includes.
# Each module dir is added to PRIVATE includes so "module.c" can #include its
# companion source files via quoted paths (e.g. #include "sfobjs.c").

set(FT_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/vendor/freetype)

if(EXISTS ${FT_SRC_DIR}/include/freetype/freetype.h)

    set(FT_BASE_SRCS
        ${FT_SRC_DIR}/src/base/ftbase.c
        ${FT_SRC_DIR}/src/base/ftbbox.c
        ${FT_SRC_DIR}/src/base/ftbitmap.c
        ${FT_SRC_DIR}/src/base/ftdebug.c
        ${FT_SRC_DIR}/src/base/ftglyph.c
        ${FT_SRC_DIR}/src/base/ftinit.c
        ${FT_SRC_DIR}/src/base/ftmm.c
        ${FT_SRC_DIR}/src/base/ftstroke.c
        ${FT_SRC_DIR}/src/base/ftsystem.c
        ${FT_SRC_DIR}/src/base/fttrigon.c
        ${FT_SRC_DIR}/src/base/ftsynth.c
        ${FT_SRC_DIR}/src/base/ftfstype.c
    )

    set(FT_MODULE_SRCS
        ${FT_SRC_DIR}/src/sfnt/sfnt.c
        ${FT_SRC_DIR}/src/truetype/truetype.c
        ${FT_SRC_DIR}/src/smooth/smooth.c
        ${FT_SRC_DIR}/src/autofit/autofit.c
        ${FT_SRC_DIR}/src/psnames/psnames.c
        ${FT_SRC_DIR}/src/raster/raster.c
    )

    # zlib stream support (for gzip-compressed fonts) — uses vendor_zlib
    set(FT_GZIP_SRCS
        ${FT_SRC_DIR}/src/gzip/ftgzip.c
    )

    add_library(vendor_freetype STATIC
        ${FT_BASE_SRCS}
        ${FT_MODULE_SRCS}
        ${FT_GZIP_SRCS}
    )

    target_compile_definitions(vendor_freetype PRIVATE FT2_BUILD_LIBRARY)

    target_include_directories(vendor_freetype PUBLIC
        ${FT_SRC_DIR}/include
    )

    target_include_directories(vendor_freetype PRIVATE
        ${FT_SRC_DIR}/include
        ${FT_SRC_DIR}/src
        ${FT_SRC_DIR}/src/base
        ${FT_SRC_DIR}/src/sfnt
        ${FT_SRC_DIR}/src/truetype
        ${FT_SRC_DIR}/src/smooth
        ${FT_SRC_DIR}/src/autofit
        ${FT_SRC_DIR}/src/psnames
        ${FT_SRC_DIR}/src/raster
        ${FT_SRC_DIR}/src/gzip
        # POSIX shim for stdio.h, stdlib.h, string.h etc.
        ${POSIX_INC_DIR}
        lib/include
    )

    target_compile_options(vendor_freetype PRIVATE
        ${USER_COMPILE_FLAGS}
        -Wno-implicit-function-declaration
        -Wno-unused-parameter
        -Wno-sign-compare
        -Wno-unused-variable
        -Wno-type-limits
        -Wno-missing-field-initializers
        -Wno-strict-aliasing
    )

    # Link vendor_zlib (for gzip font streams) and libaether_posix
    # (for setjmp/longjmp/qsort/getenv used by FreeType internals)
    target_link_libraries(vendor_freetype PUBLIC vendor_zlib libaether_posix)

    set(FREETYPE_TARGET vendor_freetype)

else()
    message(STATUS "[Phase 7.2] FreeType not found — run scripts/fetch_freetype.sh")
    set(FREETYPE_TARGET "")
endif()

# ── libaether_font — thin FreeType wrapper (Phase 7.2) ───────────────────────

if(FREETYPE_TARGET)

    add_library(libaether_font STATIC
        lib/aether_font/aether_font.c
    )

    target_include_directories(libaether_font PUBLIC
        lib/aether_font
        ${FT_SRC_DIR}/include
        ${POSIX_INC_DIR}
    )

    target_compile_options(libaether_font PRIVATE
        ${USER_COMPILE_FLAGS}
        -Wno-unused-parameter
    )

    target_link_libraries(libaether_font PUBLIC vendor_freetype libaether_posix)

    # ── user_font_test — Phase 7.2 integration test (Task 7.2.5) ─────────────

    add_executable(user_font_test
        apps/font_test/main.c
        lib/crt0.S
    )

    target_include_directories(user_font_test PRIVATE
        lib/aether_font
        ${FT_SRC_DIR}/include
        ${POSIX_INC_DIR}
        lib/include
    )

    target_compile_options(user_font_test PRIVATE
        ${USER_COMPILE_FLAGS}
        -Wno-unused-parameter
    )

    target_link_libraries(user_font_test PRIVATE libaether_font libaether_posix)

    target_link_options(user_font_test PRIVATE
        -T ${USER_LINKER_SCRIPT} -nostdlib -static -Wl,--build-id=none
    )

    set_target_properties(user_font_test PROPERTIES
        OUTPUT_NAME "font_test" SUFFIX ""
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/initrd_staging
    )

    set(FONT_TEST_TARGET user_font_test)

else()
    set(FONT_TEST_TARGET "")
endif()

# ── NetSurf support libraries (Phase 7.3) ────────────────────────────────────
#
# Eight pure-C libraries from the NetSurf project, ported to AetherOS
# bare-metal via libaether_posix.  All compiled as static archives.
#
# Dependency order (low → high):
#   libparserutils, libwapcaplet, libnsutils, libnsbmp, libnsgif
#   → libhubbub (needs libparserutils)
#   → libcss    (needs libparserutils + libwapcaplet)
#   → libdom    (needs libhubbub + libwapcaplet + libparserutils)
#
# Prerequisites: run  scripts/fetch_netsurf_libs.sh  once.

set(NETSURF_VENDOR ${CMAKE_CURRENT_SOURCE_DIR}/vendor)
set(NETSURF_GUARD  ${NETSURF_VENDOR}/libparserutils/include/parserutils/parserutils.h)

# Shared warning suppression for all third-party NetSurf code
set(NETSURF_WARN_FLAGS
    -w                              # suppress all warnings in vendor code
    -Wno-undef
)

if(EXISTS ${NETSURF_GUARD})

    # ── vendor_libparserutils ─────────────────────────────────────────────────
    # WITHOUT_ICONV_FILTER: compile the filter.c iconv path out (no iconv on AetherOS)

    set(LPUTILS_DIR ${NETSURF_VENDOR}/libparserutils)
    add_library(vendor_libparserutils STATIC
        ${LPUTILS_DIR}/src/charset/aliases.c
        ${LPUTILS_DIR}/src/charset/codec.c
        ${LPUTILS_DIR}/src/charset/codecs/codec_8859.c
        ${LPUTILS_DIR}/src/charset/codecs/codec_ascii.c
        ${LPUTILS_DIR}/src/charset/codecs/codec_ext8.c
        ${LPUTILS_DIR}/src/charset/codecs/codec_utf16.c
        ${LPUTILS_DIR}/src/charset/codecs/codec_utf8.c
        ${LPUTILS_DIR}/src/charset/encodings/utf16.c
        ${LPUTILS_DIR}/src/charset/encodings/utf8.c
        ${LPUTILS_DIR}/src/input/filter.c
        ${LPUTILS_DIR}/src/input/inputstream.c
        ${LPUTILS_DIR}/src/utils/buffer.c
        ${LPUTILS_DIR}/src/utils/errors.c
        ${LPUTILS_DIR}/src/utils/stack.c
        ${LPUTILS_DIR}/src/utils/vector.c
    )
    target_include_directories(vendor_libparserutils PUBLIC
        ${LPUTILS_DIR}/include
        ${POSIX_INC_DIR}
        lib/include
    )
    target_include_directories(vendor_libparserutils PRIVATE
        ${LPUTILS_DIR}/src
    )
    target_compile_definitions(vendor_libparserutils PRIVATE WITHOUT_ICONV_FILTER=1)
    target_compile_options(vendor_libparserutils PRIVATE
        ${USER_COMPILE_FLAGS} ${NETSURF_WARN_FLAGS}
    )
    target_link_libraries(vendor_libparserutils PUBLIC libaether_posix)

    # ── vendor_libwapcaplet ───────────────────────────────────────────────────
    # Single-file string interning pool; no initialization required.

    set(LWAPCAP_DIR ${NETSURF_VENDOR}/libwapcaplet)
    add_library(vendor_libwapcaplet STATIC
        ${LWAPCAP_DIR}/src/libwapcaplet.c
    )
    target_include_directories(vendor_libwapcaplet PUBLIC
        ${LWAPCAP_DIR}/include
        ${POSIX_INC_DIR}
        lib/include
    )
    target_compile_options(vendor_libwapcaplet PRIVATE
        ${USER_COMPILE_FLAGS} ${NETSURF_WARN_FLAGS}
    )
    target_link_libraries(vendor_libwapcaplet PUBLIC libaether_posix)

    # ── vendor_libnsutils ─────────────────────────────────────────────────────
    # time.c needs _POSIX_TIMERS + _POSIX_MONOTONIC_CLOCK so it uses our
    # clock_gettime(CLOCK_MONOTONIC) implementation from libaether_posix.

    set(LNSUTILS_DIR ${NETSURF_VENDOR}/libnsutils)
    add_library(vendor_libnsutils STATIC
        ${LNSUTILS_DIR}/src/base64.c
        ${LNSUTILS_DIR}/src/time.c
        ${LNSUTILS_DIR}/src/unistd.c
    )
    target_include_directories(vendor_libnsutils PUBLIC
        ${LNSUTILS_DIR}/include
        ${POSIX_INC_DIR}
        lib/include
    )
    target_compile_definitions(vendor_libnsutils PRIVATE
        _POSIX_TIMERS=1
        _POSIX_MONOTONIC_CLOCK=1
    )
    target_compile_options(vendor_libnsutils PRIVATE
        ${USER_COMPILE_FLAGS} ${NETSURF_WARN_FLAGS}
    )
    target_link_libraries(vendor_libnsutils PUBLIC libaether_posix)

    # ── vendor_libnsbmp ───────────────────────────────────────────────────────

    set(LNSBMP_DIR ${NETSURF_VENDOR}/libnsbmp)
    add_library(vendor_libnsbmp STATIC
        ${LNSBMP_DIR}/src/libnsbmp.c
    )
    target_include_directories(vendor_libnsbmp PUBLIC
        ${LNSBMP_DIR}/include
        ${POSIX_INC_DIR}
        lib/include
    )
    target_compile_options(vendor_libnsbmp PRIVATE
        ${USER_COMPILE_FLAGS} ${NETSURF_WARN_FLAGS}
    )
    target_link_libraries(vendor_libnsbmp PUBLIC libaether_posix)

    # ── vendor_libnsgif ───────────────────────────────────────────────────────
    # gif.c #includes lzw.h from the same src/ dir → needs PRIVATE src include.

    set(LNSGIF_DIR ${NETSURF_VENDOR}/libnsgif)
    add_library(vendor_libnsgif STATIC
        ${LNSGIF_DIR}/src/gif.c
        ${LNSGIF_DIR}/src/lzw.c
    )
    target_include_directories(vendor_libnsgif PUBLIC
        ${LNSGIF_DIR}/include
        ${POSIX_INC_DIR}
        lib/include
    )
    target_include_directories(vendor_libnsgif PRIVATE
        ${LNSGIF_DIR}/src
    )
    target_compile_options(vendor_libnsgif PRIVATE
        ${USER_COMPILE_FLAGS} ${NETSURF_WARN_FLAGS}
    )
    target_link_libraries(vendor_libnsgif PUBLIC libaether_posix)

    # ── vendor_libhubbub ──────────────────────────────────────────────────────
    # HTML5 parser; depends on libparserutils for charset detection + streams.

    set(LHUBBUB_DIR ${NETSURF_VENDOR}/libhubbub)
    add_library(vendor_libhubbub STATIC
        ${LHUBBUB_DIR}/src/charset/detect.c
        ${LHUBBUB_DIR}/src/parser.c
        ${LHUBBUB_DIR}/src/tokeniser/entities.c
        ${LHUBBUB_DIR}/src/tokeniser/tokeniser.c
        ${LHUBBUB_DIR}/src/treebuilder/after_after_body.c
        ${LHUBBUB_DIR}/src/treebuilder/after_after_frameset.c
        ${LHUBBUB_DIR}/src/treebuilder/after_body.c
        ${LHUBBUB_DIR}/src/treebuilder/after_frameset.c
        ${LHUBBUB_DIR}/src/treebuilder/after_head.c
        ${LHUBBUB_DIR}/src/treebuilder/before_head.c
        ${LHUBBUB_DIR}/src/treebuilder/before_html.c
        ${LHUBBUB_DIR}/src/treebuilder/element-type.c
        ${LHUBBUB_DIR}/src/treebuilder/generic_rcdata.c
        ${LHUBBUB_DIR}/src/treebuilder/in_body.c
        ${LHUBBUB_DIR}/src/treebuilder/in_caption.c
        ${LHUBBUB_DIR}/src/treebuilder/in_cell.c
        ${LHUBBUB_DIR}/src/treebuilder/in_column_group.c
        ${LHUBBUB_DIR}/src/treebuilder/in_foreign_content.c
        ${LHUBBUB_DIR}/src/treebuilder/in_frameset.c
        ${LHUBBUB_DIR}/src/treebuilder/in_head.c
        ${LHUBBUB_DIR}/src/treebuilder/in_head_noscript.c
        ${LHUBBUB_DIR}/src/treebuilder/in_row.c
        ${LHUBBUB_DIR}/src/treebuilder/in_select.c
        ${LHUBBUB_DIR}/src/treebuilder/in_select_in_table.c
        ${LHUBBUB_DIR}/src/treebuilder/in_table.c
        ${LHUBBUB_DIR}/src/treebuilder/in_table_body.c
        ${LHUBBUB_DIR}/src/treebuilder/initial.c
        ${LHUBBUB_DIR}/src/treebuilder/treebuilder.c
        ${LHUBBUB_DIR}/src/utils/errors.c
        ${LHUBBUB_DIR}/src/utils/string.c
    )
    target_include_directories(vendor_libhubbub PUBLIC
        ${LHUBBUB_DIR}/include
        ${POSIX_INC_DIR}
        lib/include
    )
    target_include_directories(vendor_libhubbub PRIVATE
        ${LHUBBUB_DIR}/src
    )
    target_compile_options(vendor_libhubbub PRIVATE
        ${USER_COMPILE_FLAGS} ${NETSURF_WARN_FLAGS}
    )
    target_link_libraries(vendor_libhubbub PUBLIC vendor_libparserutils libaether_posix)

    # ── vendor_libcss ─────────────────────────────────────────────────────────
    # 183 source files (184 total minus css_property_parser_gen.c tool).
    # Uses GLOB_RECURSE + filter; CONFIGURE_DEPENDS re-globs on any src change.

    set(LCSS_DIR ${NETSURF_VENDOR}/libcss)
    file(GLOB_RECURSE LIBCSS_SRCS CONFIGURE_DEPENDS "${LCSS_DIR}/src/*.c")
    list(FILTER LIBCSS_SRCS EXCLUDE REGEX ".*/css_property_parser_gen\\.c$")

    add_library(vendor_libcss STATIC ${LIBCSS_SRCS})
    target_include_directories(vendor_libcss PUBLIC
        ${LCSS_DIR}/include
        ${POSIX_INC_DIR}
        lib/include
    )
    target_include_directories(vendor_libcss PRIVATE
        ${LCSS_DIR}/src
    )
    target_compile_options(vendor_libcss PRIVATE
        ${USER_COMPILE_FLAGS} ${NETSURF_WARN_FLAGS}
    )
    target_link_libraries(vendor_libcss PUBLIC
        vendor_libparserutils
        vendor_libwapcaplet
        libaether_posix
    )

    # ── vendor_libdom ─────────────────────────────────────────────────────────
    # W3C DOM: 94 files across core/, events/, html/, utils/.

    set(LDOM_DIR ${NETSURF_VENDOR}/libdom)
    file(GLOB_RECURSE LIBDOM_SRCS CONFIGURE_DEPENDS "${LDOM_DIR}/src/*.c")
    # Hubbub binding: dom_hubbub_parser_* — needed by NetSurf html.c content handler
    list(APPEND LIBDOM_SRCS ${LDOM_DIR}/bindings/hubbub/parser.c)

    add_library(vendor_libdom STATIC ${LIBDOM_SRCS})
    target_include_directories(vendor_libdom PUBLIC
        ${LDOM_DIR}/include
        ${POSIX_INC_DIR}
        lib/include
    )
    target_include_directories(vendor_libdom PRIVATE
        ${LDOM_DIR}/src
        ${LDOM_DIR}/bindings/hubbub   # errors.h, utils.h
    )
    target_compile_options(vendor_libdom PRIVATE
        ${USER_COMPILE_FLAGS} ${NETSURF_WARN_FLAGS}
    )
    target_link_libraries(vendor_libdom PUBLIC
        vendor_libhubbub
        vendor_libwapcaplet
        vendor_libparserutils
        libaether_posix
    )

    set(NETSURF_LIBS_TARGETS
        vendor_libparserutils vendor_libwapcaplet vendor_libnsutils
        vendor_libnsbmp vendor_libnsgif vendor_libhubbub
        vendor_libcss vendor_libdom
    )
    message(STATUS "[Phase 7.3] NetSurf support libraries found — building all 8 targets")

    # ── user_html_test — Phase 7.3 integration test (Task 7.3.7) ─────────────
    #
    # Tests libwapcaplet string interning and libhubbub HTML5 parsing.
    # Run in AetherOS:  html_test

    add_executable(user_html_test
        apps/html_test/main.c
        lib/crt0.S
    )
    target_include_directories(user_html_test PRIVATE
        ${LHUBBUB_DIR}/include
        ${LWAPCAP_DIR}/include
        ${LPUTILS_DIR}/include
        ${POSIX_INC_DIR}
        lib/include
    )
    target_compile_options(user_html_test PRIVATE
        ${USER_COMPILE_FLAGS}
        -Wno-unused-parameter
    )
    target_link_libraries(user_html_test PRIVATE
        vendor_libhubbub
        vendor_libwapcaplet
        vendor_libparserutils
        libaether_posix
    )
    target_link_options(user_html_test PRIVATE
        -T ${USER_LINKER_SCRIPT} -nostdlib -static -Wl,--build-id=none
    )
    set_target_properties(user_html_test PROPERTIES
        OUTPUT_NAME "html_test" SUFFIX ""
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/initrd_staging
    )
    set(HTML_TEST_TARGET user_html_test)

else()
    message(STATUS
        "[Phase 7.3] NetSurf libs not found in vendor/ — "
        "run scripts/fetch_netsurf_libs.sh then re-configure.")
    set(NETSURF_LIBS_TARGETS "")
    set(HTML_TEST_TARGET "")
endif()

# ── Phase 7.4 — QuickJS ES2020 engine ────────────────────────────────────────
#
# QuickJS 2021-03-27 (Fabrice Bellard) — small embeddable ES2020 engine.
# Fetch:  scripts/fetch_quickjs.sh
#
# Compiled files:
#   quickjs.c      — main engine (includes dtoa.c internally)
#   libregexp.c    — regexp engine
#   libunicode.c   — Unicode support
#   cutils.c       — basic utilities
#   libbf.c        — bignum (required for BigInt / ES2020)
#
# Excluded:
#   quickjs-libc.c — POSIX OS module (replaced by AetherOS shim)
#   qjs.c / qjsc.c — standalone executables (host tools)

set(QJS_GUARD ${CMAKE_CURRENT_SOURCE_DIR}/vendor/quickjs/quickjs.c)
if(EXISTS ${QJS_GUARD})
    set(QJS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/vendor/quickjs)

    # Resolve the cross-compiler's libgcc.a so we can link it by full path
    # after the other libraries (GNU ld is left-to-right; -lgcc via link_options
    # lands before object files and fails to resolve back-references).
    execute_process(
        COMMAND ${CMAKE_C_COMPILER} -march=armv8-a -print-libgcc-file-name
        OUTPUT_VARIABLE QJS_LIBGCC_PATH
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )

    add_library(vendor_quickjs STATIC
        ${QJS_DIR}/quickjs.c
        ${QJS_DIR}/libregexp.c
        ${QJS_DIR}/libunicode.c
        ${QJS_DIR}/cutils.c
        ${QJS_DIR}/libbf.c
    )
    target_include_directories(vendor_quickjs PUBLIC ${QJS_DIR})
    target_include_directories(vendor_quickjs PRIVATE ${POSIX_INC_DIR})
    target_compile_definitions(vendor_quickjs PRIVATE
        CONFIG_VERSION="2021-03-27"
        CONFIG_BIGNUM        # required for BigInt (ES2020); also enables BigFloat/BigDecimal
                             # which are left unwired (only JS_AddIntrinsicBigInt is called)
        # No CONFIG_ATOMICS → skip pthread/stdatomic (no worker threads)
    )
    target_compile_options(vendor_quickjs PRIVATE
        ${USER_COMPILE_FLAGS}
        -mno-outline-atomics   # inline LL/SC — avoids __aarch64_ldadd*_acq_rel from libgcc
        -Wno-unused-parameter
        -Wno-unused-function
        -Wno-sign-compare
        -Wno-missing-field-initializers
        -Wno-implicit-fallthrough
    )
    target_link_libraries(vendor_quickjs PUBLIC libaether_posix ${QJS_LIBGCC_PATH})
    message(STATUS "[Phase 7.4] QuickJS found — building vendor_quickjs")

    # ── user_js_test — Phase 7.4 integration test (Task 7.4.5) ───────────────
    #
    # Exercises: JS string eval, arithmetic, arrow functions, closures, BigInt.
    # Run in AetherOS:  js_test
    add_executable(user_js_test
        apps/js_test/main.c
        lib/crt0.S
    )
    target_include_directories(user_js_test PRIVATE
        ${QJS_DIR}
        ${POSIX_INC_DIR}
        lib/include
    )
    target_compile_options(user_js_test PRIVATE
        ${USER_COMPILE_FLAGS}
        -Wno-unused-result
    )
    target_link_libraries(user_js_test PRIVATE
        vendor_quickjs
        libaether_posix
    )
    target_link_options(user_js_test PRIVATE
        -T ${USER_LINKER_SCRIPT} -nostdlib -static -Wl,--build-id=none
    )
    set_target_properties(user_js_test PROPERTIES
        OUTPUT_NAME "js_test" SUFFIX ""
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/initrd_staging
    )
    set(JS_TEST_TARGET user_js_test)

else()
    message(STATUS
        "[Phase 7.4] QuickJS not found in vendor/ — "
        "run scripts/fetch_quickjs.sh then re-configure.")
    set(JS_TEST_TARGET "")
endif()

# ── Iteration 3 — mbedTLS 3.6 (HTTPS/TLS) ───────────────────────────────────
#
# Pure-C TLS 1.2 library, ~300 KB, designed for embedded targets.
# Fetch:  scripts/fetch_mbedtls.sh
#   → vendor/mbedtls/   (gitignored — unmodified upstream)
#   → lib/netsurf_aether/ca_bundle_gen.c  (generated + committed)
#
# Bridge files (always tracked):
#   lib/netsurf_aether/mbedtls_aether_config.h  — minimal TLS 1.2 config
#   lib/netsurf_aether/tls_aether.h / .c        — connect/read/write/close API
#   lib/netsurf_aether/ca_bundle_gen.c           — Mozilla CA bundle as C array
#
# Excluded source files (not usable on AetherOS):
#   net_sockets.c   — POSIX socket API (we use custom BIO callbacks)
#   timing.c        — uses setitimer / SIGALRM (not in libaether_posix)
#   threading.c     — pthreads (single-threaded model)
#   entropy_poll.c  — reads /dev/urandom; replaced by mbedtls_hardware_poll()
#                     in tls_aether.c (MBEDTLS_ENTROPY_HARDWARE_ALT)
# PSA crypto files compile to nothing when MBEDTLS_PSA_CRYPTO_C is undefined
# (guarded by #if inside each file) — we don't exclude them explicitly.

set(MBEDTLS_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/vendor/mbedtls)
set(MBEDTLS_BRIDGE  ${CMAKE_CURRENT_SOURCE_DIR}/lib/netsurf_aether)

if(EXISTS ${MBEDTLS_SRC_DIR}/include/mbedtls/ssl.h)

    # Collect all library sources then strip the four AetherOS-incompatible files
    file(GLOB MBEDTLS_SRCS "${MBEDTLS_SRC_DIR}/library/*.c")
    list(FILTER MBEDTLS_SRCS EXCLUDE REGEX
        "/(net_sockets|timing|threading|entropy_poll)\\.c$")

    add_library(vendor_mbedtls STATIC
        ${MBEDTLS_SRCS}
        ${MBEDTLS_BRIDGE}/tls_aether.c          # TLS API wrapper
        ${MBEDTLS_BRIDGE}/ca_bundle_gen.c        # Mozilla CA bundle (generated)
    )

    target_include_directories(vendor_mbedtls PUBLIC
        ${MBEDTLS_SRC_DIR}/include   # <mbedtls/ssl.h>, <mbedtls/entropy.h>, …
        ${MBEDTLS_BRIDGE}            # mbedtls_aether_config.h, tls_aether.h
        ${POSIX_INC_DIR}             # stdlib.h, string.h, stdint.h, …
        lib/include                  # sys.h (sys_rtc_get, sys_get_ticks)
    )

    target_compile_definitions(vendor_mbedtls PRIVATE
        # Tell mbedTLS to use our custom config header instead of the default
        MBEDTLS_CONFIG_FILE="mbedtls_aether_config.h"
    )

    target_compile_options(vendor_mbedtls PRIVATE
        ${USER_COMPILE_FLAGS}
        # Override global -O0: three -O0-specific AArch64 bugs affect mbedTLS at
        # debug build level — wrong bignum from MULADDC asm stack locals, wrong ECP
        # field reduction from 32-bit alias casts, and generic loop/register issues
        # that break ECDSA verify (ecp_muladd) for ECDHE-ECDSA certs (Wikipedia).
        # -O1 fixes all three without enabling NIST_OPTIM or HAVE_ASM.
        -O1
        # mbedTLS is pure integer math (bignum/SHA/ECP).  Restrict compilation to
        # general-purpose registers only so GCC cannot auto-vectorize SHA-256 loops
        # or inline memcpy/memset with NEON instructions.  Without this, a timer IRQ
        # landing inside a NEON-vectorized SHA-256 iteration corrupts the hash state
        # (AetherOS exceptions.S/context_switch.S do not save v0–v31) and causes
        # ECDSA ServerKeyExchange verification to fail with ECP_VERIFY_FAILED.
        -mgeneral-regs-only
        # Also undefine __ARM_NEON to suppress any arm_neon.h include paths that
        # common.h might try even though -mgeneral-regs-only already blocks them.
        -U__ARM_NEON
        -Wno-implicit-function-declaration
        -Wno-unused-function
        -Wno-unused-variable
        -Wno-sign-compare
        -Wno-type-limits
        -Wno-missing-field-initializers
        -Wno-array-bounds
        -Wno-stringop-overflow
        -w   # suppress remaining vendor warnings
    )

    target_link_libraries(vendor_mbedtls PUBLIC libaether_posix)

    set(MBEDTLS_TARGET vendor_mbedtls)
    message(STATUS "[Iter 3] mbedTLS 3.6 found — building vendor_mbedtls (TLS 1.2 client)")

else()
    message(STATUS
        "[Iter 3] mbedTLS not found in vendor/mbedtls/ — "
        "run scripts/fetch_mbedtls.sh then re-configure.  "
        "HTTPS will remain disabled (HTTP-only browsing).")
    set(MBEDTLS_TARGET "")
endif()

# ── Phase 7.5 — NetSurf core port ────────────────────────────────────────────
#
# Two sub-libraries:
#   vendor_libnslog   — NetSurf structured logging framework
#   vendor_netsurf    — NetSurf core (HTML/CSS/layout/fetch) + AetherOS bridge
#
# Prerequisites:
#   scripts/fetch_netsurf.sh   → vendor/libnslog/, vendor/netsurf/
#
# AetherOS bridge files (tracked in git):
#   lib/netsurf_aether/        → gui_misc, gui_fetch, gui_window stubs,
#                                fetch_file, fetch_http stub, js_aether (QuickJS)
#
# The ns_test integration test calls netsurf_init() + netsurf_exit() and
# verifies NSERROR_OK.  It is the deliverable for task 7.5.7.

set(NSLOG_VENDOR ${CMAKE_CURRENT_SOURCE_DIR}/vendor/libnslog)
set(NS_VENDOR    ${CMAKE_CURRENT_SOURCE_DIR}/vendor/netsurf)
set(NS_BRIDGE    ${CMAKE_CURRENT_SOURCE_DIR}/lib/netsurf_aether)

# Guard: only build if the source was fetched
if(EXISTS ${NSLOG_VENDOR}/include/nslog/nslog.h AND
   EXISTS ${NS_VENDOR}/include/netsurf/netsurf.h)

    # ── vendor_libnslog ───────────────────────────────────────────────────────
    # Small structured logging library used by NetSurf core.

    file(GLOB_RECURSE LIBNSLOG_SRCS CONFIGURE_DEPENDS "${NSLOG_VENDOR}/src/*.c")
    # filter.c requires bison-generated headers not shipped in the source tree;
    # replaced by nslog_filter_stub.c in NS_BRIDGE_SRCS (via vendor_netsurf).
    list(FILTER LIBNSLOG_SRCS EXCLUDE REGEX ".*/filter\\.c$")

    add_library(vendor_libnslog STATIC ${LIBNSLOG_SRCS})

    target_include_directories(vendor_libnslog PUBLIC
        ${NSLOG_VENDOR}/include
        ${POSIX_INC_DIR}
        lib/include
    )
    target_compile_options(vendor_libnslog PRIVATE
        ${USER_COMPILE_FLAGS}
        -w   # suppress all warnings in vendor code
    )
    target_link_libraries(vendor_libnslog PUBLIC libaether_posix)

    message(STATUS "[Phase 7.5] libnslog found — building vendor_libnslog")

    # ── vendor_netsurf ────────────────────────────────────────────────────────
    # NetSurf core: all .c files under vendor/netsurf/ EXCEPT:
    #   frontends/          — platform-specific frontends (we have our own bridge)
    #   javascript/duktape/ — Duktape JS engine (replaced by QuickJS bridge)
    #   javascript/none.c   — no-op JS stub (replaced by js_aether.c)
    #   content/fetchers/curl.c — curl HTTP (replaced by fetch_http_aether.c)
    #   test/               — unit test helpers
    #   tools/              — code-generation tools (host-side, not library)

    file(GLOB_RECURSE NETSURF_SRCS CONFIGURE_DEPENDS "${NS_VENDOR}/*.c")

    list(FILTER NETSURF_SRCS EXCLUDE REGEX ".*/frontends/.*\\.c$")
    list(FILTER NETSURF_SRCS EXCLUDE REGEX ".*/javascript/duktape/.*\\.c$")
    list(FILTER NETSURF_SRCS EXCLUDE REGEX ".*/javascript/none\\.c$")
    list(FILTER NETSURF_SRCS EXCLUDE REGEX ".*/content/fetchers/curl\\.c$")
    # Replaced by fetch_file_aether.c (entire file/ dir excluded — uses scandir/dirfd/fstatat)
    list(FILTER NETSURF_SRCS EXCLUDE REGEX ".*/content/fetchers/file/.*\\.c$")
    list(FILTER NETSURF_SRCS EXCLUDE REGEX ".*/test/.*\\.c$")
    list(FILTER NETSURF_SRCS EXCLUDE REGEX ".*/utils/test.*\\.c$")
    list(FILTER NETSURF_SRCS EXCLUDE REGEX ".*/tools/.*\\.c$")
    # Optional image formats not available on AetherOS
    list(FILTER NETSURF_SRCS EXCLUDE REGEX ".*/image/nssprite\\.c$")   # RISC OS sprites (librosprite)
    list(FILTER NETSURF_SRCS EXCLUDE REGEX ".*/image/jpegxl\\.c$")     # JPEG XL (libjxl)
    list(FILTER NETSURF_SRCS EXCLUDE REGEX ".*/image/rsvg\\.c$")       # SVG via librsvg (old)
    list(FILTER NETSURF_SRCS EXCLUDE REGEX ".*/image/rsvg246\\.c$")    # SVG via librsvg 2.46
    list(FILTER NETSURF_SRCS EXCLUDE REGEX ".*/image/svg\\.c$")        # SVG via libsvgtiny
    list(FILTER NETSURF_SRCS EXCLUDE REGEX ".*/image/video\\.c$")      # Video via GStreamer
    list(FILTER NETSURF_SRCS EXCLUDE REGEX ".*/image/webp\\.c$")       # WebP (libwebp)

    # AetherOS bridge files (tracked in git, not part of the upstream clone)
    set(NS_BRIDGE_SRCS
        ${NS_BRIDGE}/nslog_aether.c
        ${NS_BRIDGE}/nslog_filter_stub.c   # replaces libnslog filter.c (bison-generated dep)
        ${NS_BRIDGE}/gui_misc_aether.c
        ${NS_BRIDGE}/gui_fetch_aether.c
        ${NS_BRIDGE}/gui_window_stub.c
        ${NS_BRIDGE}/gui_bitmap_aether.c
        ${NS_BRIDGE}/gui_layout_aether.c
        ${NS_BRIDGE}/plot_aether.c         # Phase 7.7 plotter_table implementation
        ${NS_BRIDGE}/fetch_file_aether.c   # provides fetch_file_register() (replaces built-in)
        ${NS_BRIDGE}/fetch_http_aether.c   # Phase 7.6 real HTTP/1.1 over POSIX sockets
        ${NS_BRIDGE}/cookies_aether.c      # Iteration 5.3 cookie store
        ${NS_BRIDGE}/downloads_aether.c   # Iteration 5.4 download manager
        ${NS_BRIDGE}/js_aether.c
    )

    add_library(vendor_netsurf STATIC ${NETSURF_SRCS} ${NS_BRIDGE_SRCS})

    # Include paths:
    #   NS_VENDOR/     — #include "utils/log.h", "desktop/browser_window.h"
    #   NS_VENDOR/include — #include "netsurf/browser_window.h" (public API)
    #   NS_BRIDGE/     — netsurf_aether.h + options.h
    #   vendor libs    — libdom, libcss, quickjs, …
    #   POSIX shim + sys.h
    target_include_directories(vendor_netsurf PUBLIC
        ${NS_VENDOR}
        ${NS_VENDOR}/include
        ${NETSURF_VENDOR}          # dom/ symlink → libdom/ (needed for dom/bindings/hubbub/parser.h)
        ${NS_VENDOR}/content/handlers   # js.h lives at javascript/js.h here
        ${NS_VENDOR}/frontends          # framebuffer/options.h for nsoption.c
        ${NS_BRIDGE}
        ${NSLOG_VENDOR}/include
        ${NETSURF_VENDOR}/libdom/include
        ${NETSURF_VENDOR}/libcss/include
        ${NETSURF_VENDOR}/libhubbub/include
        ${NETSURF_VENDOR}/libparserutils/include
        ${NETSURF_VENDOR}/libwapcaplet/include
        ${NETSURF_VENDOR}/libnsutils/include
        ${NETSURF_VENDOR}/libnsbmp/include
        ${NETSURF_VENDOR}/libnsgif/include
        ${FT_SRC_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/lib/aether_font   # aether_font.h
        ${QJS_DIR}
        ${LIBPNG_SRC_DIR}
        ${LIBJPEG_SRC_DIR}
        ${ZLIB_SRC_DIR}
        ${POSIX_INC_DIR}
        lib/include
    )

    target_compile_definitions(vendor_netsurf PRIVATE
        NETSURF_AETHER=1
        # nsoption.c selects frontend options via #elif chains on these defines.
        # "nsframebuffer" is the closest existing frontend to AetherOS (same
        # direct framebuffer model); it causes nsoption.c to #include
        # "frontends/framebuffer/options.h" which has sensible bare-metal defaults.
        nsframebuffer=1
        # Enable image formats we have vendor libraries for
        WITH_BMP
        WITH_GIF
        WITH_JPEG
        WITH_PNG
        # Disable optional backends we haven't ported (leave undefined so #ifdef guards work)
        # WITH_RSVG, WITH_WEBP, WITH_HARU, WITH_EXPAT, WITH_NSSPRITE, WITH_JPEGXL, WITH_NS_SVG
        NETSURF_USE_CURL=0
        NETSURF_USE_DUKTAPE=0
        # Log filter defaults (NULL = no filter; our stub always passes through)
        "NETSURF_BUILTIN_LOG_FILTER=NULL"
        "NETSURF_BUILTIN_VERBOSE_FILTER=NULL"
    )

    target_compile_options(vendor_netsurf PRIVATE
        ${USER_COMPILE_FLAGS}
        -w                        # suppress all warnings in vendor code
        -Wno-implicit-function-declaration
        -Wno-int-conversion       # GCC 14: int→pointer now a hard error in vendor code
    )

    target_link_libraries(vendor_netsurf PUBLIC
        vendor_libnslog
        vendor_libdom
        vendor_libcss
        vendor_libhubbub
        vendor_libparserutils
        vendor_libwapcaplet
        vendor_libnsutils
        vendor_libnsbmp
        vendor_libnsgif
        vendor_libpng
        vendor_libjpeg
        vendor_zlib
        vendor_freetype
        vendor_quickjs
        libaether_posix
    )

    # Iteration 3: link mbedTLS when available
    if(MBEDTLS_TARGET)
        target_link_libraries(vendor_netsurf PUBLIC ${MBEDTLS_TARGET})
        target_compile_definitions(vendor_netsurf PRIVATE AETHER_TLS_ENABLED=1)
        target_include_directories(vendor_netsurf PUBLIC
            ${MBEDTLS_SRC_DIR}/include   # <mbedtls/ssl.h> for fetch_http_aether.c
        )
    endif()

    set(NETSURF_TARGET vendor_netsurf)
    message(STATUS "[Phase 7.5] NetSurf core found — building vendor_netsurf")

    # ── user_ns_test — Phase 7.5.7 integration test ───────────────────────────
    #
    # Calls netsurf_init() with the AetherOS table, verifies NSERROR_OK,
    # then calls netsurf_exit().  Exits 0 on success, 1 on failure.
    # Run in AetherOS: ns_test

    add_executable(user_ns_test
        apps/ns_test/main.c
        lib/crt0.S
    )

    target_include_directories(user_ns_test PRIVATE
        ${NS_VENDOR}
        ${NS_VENDOR}/include
        ${NS_BRIDGE}
        ${NSLOG_VENDOR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/lib/aether_font
        ${POSIX_INC_DIR}
        lib/include
    )

    target_compile_options(user_ns_test PRIVATE
        ${USER_COMPILE_FLAGS}
        -Wno-unused-parameter
        -Wno-implicit-function-declaration
    )

    target_link_libraries(user_ns_test PRIVATE
        vendor_netsurf
        libaether_font
        libaether_posix
    )

    target_link_options(user_ns_test PRIVATE
        -T ${USER_LINKER_SCRIPT} -nostdlib -static -Wl,--build-id=none
        -Wl,--allow-multiple-definition
    )

    set_target_properties(user_ns_test PROPERTIES
        OUTPUT_NAME "ns_test" SUFFIX ""
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/initrd_staging
    )

    set(NS_TEST_TARGET user_ns_test)

    # ── user_fetch_test — Phase 7.6.7 HTTP fetch integration test ────────────────
    #
    # Calls fetch_start() with an http:// URL, drains the cooperative scheduler
    # until FETCH_FINISHED arrives, then dumps the HTTP status / body to UART.
    # Usage inside AetherOS: fetch_test [url]
    # Default URL: http://10.0.2.2:8080/test.html  (QEMU host via port-forward)

    add_executable(user_fetch_test
        apps/fetch_test/main.c
        lib/crt0.S
    )

    target_include_directories(user_fetch_test PRIVATE
        ${NS_VENDOR}
        ${NS_VENDOR}/include
        ${NS_BRIDGE}
        ${NSLOG_VENDOR}/include
        ${NETSURF_VENDOR}/libwapcaplet/include
        ${POSIX_INC_DIR}
        lib/include
    )

    target_compile_options(user_fetch_test PRIVATE
        ${USER_COMPILE_FLAGS}
        -Wno-implicit-function-declaration
    )

    target_link_libraries(user_fetch_test PRIVATE
        vendor_netsurf
        libaether_font
        libaether_posix
    )

    target_link_options(user_fetch_test PRIVATE
        -T ${USER_LINKER_SCRIPT} -nostdlib -static -Wl,--build-id=none
        -Wl,--allow-multiple-definition
    )

    set_target_properties(user_fetch_test PROPERTIES
        OUTPUT_NAME "fetch_test" SUFFIX ""
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/initrd_staging
    )

    set(FETCH_TEST_TARGET user_fetch_test)

    # ── user_browser_test — Phase 7.7 render integration test ────────────────
    #
    # Calls browser_window_create() + browser_window_redraw() with
    # aether_plotter_table, blitting the result to the AetherOS framebuffer.
    # Usage inside AetherOS: browser_test [url]
    # Default URL: http://10.0.2.2:8080/index.html

    add_executable(user_browser_test
        apps/browser_test/main.c
        lib/crt0.S
    )

    target_include_directories(user_browser_test PRIVATE
        ${NS_VENDOR}
        ${NS_VENDOR}/include
        ${NS_BRIDGE}
        ${NSLOG_VENDOR}/include
        ${NETSURF_VENDOR}/libwapcaplet/include
        ${CMAKE_CURRENT_SOURCE_DIR}/lib/aether_font
        ${POSIX_INC_DIR}
        lib/include
    )

    target_compile_options(user_browser_test PRIVATE
        ${USER_COMPILE_FLAGS}
        -Wno-implicit-function-declaration
        -Wno-unused-parameter
    )

    target_link_libraries(user_browser_test PRIVATE
        vendor_netsurf
        libaether_font
        libaether_posix
        libaether           # for gfx_raw_blit, gfx_init, gfx_glass_window_frame
    )

    target_link_options(user_browser_test PRIVATE
        -T ${USER_LINKER_SCRIPT} -nostdlib -static -Wl,--build-id=none
        -Wl,--allow-multiple-definition
    )

    set_target_properties(user_browser_test PROPERTIES
        OUTPUT_NAME "browser_test" SUFFIX ""
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/initrd_staging
    )

    set(BROWSER_TEST_TARGET user_browser_test)

    # ── user_aether_browser — Phase 7.8 MVP browser app ──────────────────────
    #
    # Full Lumina-styled browser: toolbar (back/fwd/reload + address bar),
    # NetSurf viewport via aether_plotter_table, status bar, WM event loop.
    # Registered at /aether_browser in initrd; launched from desktop icon.

    add_executable(user_aether_browser
        apps/aether_browser/main.c
        apps/aether_browser/persistence.c
        lib/crt0.S
    )

    target_include_directories(user_aether_browser PRIVATE
        ${NS_VENDOR}
        ${NS_VENDOR}/include
        ${NS_VENDOR}/desktop       # browser_history.h
        ${NS_BRIDGE}
        ${NSLOG_VENDOR}/include
        ${NETSURF_VENDOR}/libwapcaplet/include
        ${CMAKE_CURRENT_SOURCE_DIR}/lib/aether_font
        ${POSIX_INC_DIR}
        lib/include
    )

    target_compile_options(user_aether_browser PRIVATE
        ${USER_COMPILE_FLAGS}
        -Wno-implicit-function-declaration
        -Wno-unused-parameter
    )

    target_link_libraries(user_aether_browser PRIVATE
        vendor_netsurf
        libwidget
        libaether_font
        libaether_posix
        libaether
    )

    target_link_options(user_aether_browser PRIVATE
        -T ${USER_LINKER_SCRIPT} -nostdlib -static -Wl,--build-id=none
        -Wl,--allow-multiple-definition
    )

    set_target_properties(user_aether_browser PROPERTIES
        OUTPUT_NAME "aether_browser" SUFFIX ""
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/initrd_staging
    )

    set(AETHER_BROWSER_TARGET user_aether_browser)

else()
    message(STATUS
        "[Phase 7.5] NetSurf core not found — "
        "run scripts/fetch_netsurf.sh then re-configure.")
    set(NETSURF_TARGET "")
    set(NS_TEST_TARGET "")
    set(FETCH_TEST_TARGET "")
    set(BROWSER_TEST_TARGET "")
    set(AETHER_BROWSER_TARGET "")
endif()

# ── Phase 8.3 — libaetheraudio + AetherSound server ──────────────────────────

add_library(libaetheraudio STATIC
    lib/libaetheraudio/src/aetheraudio.c
)
target_include_directories(libaetheraudio PUBLIC
    lib/libaetheraudio/include
    lib/include
)
target_compile_options(libaetheraudio PRIVATE ${USER_COMPILE_FLAGS}
    -Wno-unused-parameter
)

add_executable(user_aether_sound
    apps/aether_sound/main.c
    lib/crt0.S
)
target_include_directories(user_aether_sound PRIVATE
    lib/libaetheraudio/include
    lib/include
)
target_compile_options(user_aether_sound PRIVATE ${USER_COMPILE_FLAGS}
    -Wno-unused-parameter
)
target_link_libraries(user_aether_sound PRIVATE libaetheraudio libaether)
target_link_options(user_aether_sound PRIVATE
    -T ${USER_LINKER_SCRIPT} -nostdlib -static -Wl,--build-id=none
)
set_target_properties(user_aether_sound PROPERTIES
    OUTPUT_NAME "aether_sound" SUFFIX ""
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/initrd_staging
)

# ── Phase 8.4 — libAetherDSP ─────────────────────────────────────────────────
#
# NEON-accelerated DSP primitives for the AetherOS audio pipeline.
# All processing in float32.  Requires AArch64 NEON (arm_neon.h).
#
# arm_neon.h lives in the GCC compiler-internal include directory.
# Under -nostdinc that directory is NOT searched automatically, so we
# locate it at configure time via -print-libgcc-file-name and add it
# explicitly.  (Same technique used for QJS_LIBGCC_PATH above.)

execute_process(
    COMMAND ${CMAKE_C_COMPILER} -march=armv8-a -print-libgcc-file-name
    OUTPUT_VARIABLE ADSP_LIBGCC_PATH
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
get_filename_component(ADSP_GCC_INCLUDE_DIR "${ADSP_LIBGCC_PATH}" DIRECTORY)
set(ADSP_GCC_INCLUDE_DIR "${ADSP_GCC_INCLUDE_DIR}/include")
message(STATUS "[Phase 8.4] GCC internal include (arm_neon.h): ${ADSP_GCC_INCLUDE_DIR}")

add_library(libAetherDSP STATIC
    lib/libAetherDSP/src/adsp_neon.c
    lib/libAetherDSP/src/adsp_biquad.c
    lib/libAetherDSP/src/adsp_fft.c
    lib/libAetherDSP/src/adsp_conv.c
    lib/libAetherDSP/src/adsp_env.c
    lib/libAetherDSP/src/adsp_waveshaper.c
    lib/libAetherDSP/src/adsp_resampler.c
    lib/libAetherDSP/src/adsp_smooth.c
)

target_include_directories(libAetherDSP PUBLIC
    lib/libAetherDSP/include
)

target_include_directories(libAetherDSP PRIVATE
    ${ADSP_GCC_INCLUDE_DIR}   # arm_neon.h, stdint.h (compiler-internal)
    ${POSIX_INC_DIR}
    lib/include
)

target_compile_options(libAetherDSP PRIVATE
    ${USER_COMPILE_FLAGS}
    "-I${ADSP_GCC_INCLUDE_DIR}"   # arm_neon.h (bypasses -nostdinc filtering)
    -Wno-unused-parameter
    -Wno-unused-function
    -Wno-strict-aliasing   # adsp_neon.c type-puns float↔uint via union
)

target_link_libraries(libAetherDSP PUBLIC libaether_posix)

# ── Phase 8.5 — libAetherPlugin ──────────────────────────────────────────────
#
# CLAP-inspired internal plugin API: descriptor, parameter, instance vtable,
# and static plugin registry.  All plugins are statically linked — no dlopen.

add_library(libAetherPlugin STATIC
    lib/libAetherPlugin/src/aplug_registry.c
    lib/libAetherPlugin/src/aplug_builtin.c
)

target_include_directories(libAetherPlugin PUBLIC
    lib/libAetherPlugin/include
    lib/libAetherDSP/include
)

target_include_directories(libAetherPlugin PRIVATE
    ${POSIX_INC_DIR}
    lib/include
)

target_compile_options(libAetherPlugin PRIVATE
    ${USER_COMPILE_FLAGS}
    -Wno-unused-parameter
)

target_link_libraries(libAetherPlugin PUBLIC libAetherDSP)

# ── Phase 8.6 — libAetherAmp ─────────────────────────────────────────────────
#
# WAV IR loader, cabinet convolution sim, classic tone stacks (Fender/Marshall/Vox),
# and NAM neural amp modeling (LSTM + WaveNet-lite inference).

add_library(libAetherAmp STATIC
    lib/libAetherAmp/src/aamp_wav.c
    lib/libAetherAmp/src/aamp_cab.c
    lib/libAetherAmp/src/aamp_tonestack.c
    lib/libAetherAmp/src/aamp_nam.c
)

target_include_directories(libAetherAmp PUBLIC
    lib/libAetherAmp/include
    lib/libAetherDSP/include
)

target_include_directories(libAetherAmp PRIVATE
    ${POSIX_INC_DIR}
    lib/include
)

target_compile_options(libAetherAmp PRIVATE
    ${USER_COMPILE_FLAGS}
    -Wno-unused-parameter
    -Wno-sign-compare
)

target_link_libraries(libAetherAmp PUBLIC libAetherDSP)

# ── Phase 8.7 — libAetherFX ──────────────────────────────────────────────────
#
# 25 guitar effects: tuner, gate, boost, compressor, limiter, overdrive,
# distortion, fuzz, 4-band EQ, wah, auto-wah, chorus, flanger, phaser,
# tremolo, vibrato, rotary, delay, ping-pong delay, plate, spring, pitch
# shift, octave, ring modulator, looper.

add_library(libAetherFX STATIC
    lib/libAetherFX/src/afx_util.c
    lib/libAetherFX/src/afx_dyn.c
    lib/libAetherFX/src/afx_dist.c
    lib/libAetherFX/src/afx_filter.c
    lib/libAetherFX/src/afx_mod.c
    lib/libAetherFX/src/afx_time.c
    lib/libAetherFX/src/afx_special.c
    lib/libAetherFX/src/afx_register.c
)

target_include_directories(libAetherFX PUBLIC
    lib/libAetherFX/include
    lib/libAetherPlugin/include
    lib/libAetherDSP/include
)

target_include_directories(libAetherFX PRIVATE
    ${POSIX_INC_DIR}
    lib/include
)

target_compile_options(libAetherFX PRIVATE
    ${USER_COMPILE_FLAGS}
    -Wno-unused-parameter
    -Wno-unused-function
    -Wno-sign-compare
)

target_link_libraries(libAetherFX PUBLIC libAetherPlugin libAetherDSP)

# ── Phase 8.8 — libAetherChain ────────────────────────────────────────────────

add_library(libAetherChain STATIC
    lib/libAetherChain/src/achain.c
)
target_include_directories(libAetherChain PUBLIC
    lib/libAetherChain/include
    lib/libAetherPlugin/include
    lib/libAetherDSP/include
)
target_include_directories(libAetherChain PRIVATE
    ${POSIX_INC_DIR}
    lib/include
)
target_compile_options(libAetherChain PRIVATE
    ${USER_COMPILE_FLAGS}
    -Wno-unused-parameter
    -Wno-sign-compare
)
target_link_libraries(libAetherChain PUBLIC libAetherFX libAetherPlugin libAetherDSP)

# ── Phase 8.9 — libAetherAudioWidget ─────────────────────────────────────────

add_library(libAetherAudioWidget STATIC
    lib/libAetherAudioWidget/src/awgt_prims.c
    lib/libAetherAudioWidget/src/awgt_knob.c
    lib/libAetherAudioWidget/src/awgt_led.c
    lib/libAetherAudioWidget/src/awgt_vu.c
    lib/libAetherAudioWidget/src/awgt_stomp.c
    lib/libAetherAudioWidget/src/awgt_fader.c
    lib/libAetherAudioWidget/src/awgt_panel.c
)
target_include_directories(libAetherAudioWidget PUBLIC
    lib/libAetherAudioWidget/include
)
target_include_directories(libAetherAudioWidget PRIVATE
    lib/libAetherAudioWidget/src
    ${POSIX_INC_DIR}
    lib/include
    ${ADSP_GCC_INCLUDE_DIR}
)
target_compile_options(libAetherAudioWidget PRIVATE
    ${USER_COMPILE_FLAGS}
    "-I${ADSP_GCC_INCLUDE_DIR}"
    -Wno-unused-parameter
    -Wno-unused-function
)
target_link_libraries(libAetherAudioWidget PUBLIC libaether_posix)

# ── Phase 8.10 — AetherGuitar ─────────────────────────────────────────────────

add_executable(user_aether_guitar
    apps/aether_guitar/main.c
    apps/aether_guitar/view_board.c
    apps/aether_guitar/view_amp.c
    apps/aether_guitar/view_tuner.c
    apps/aether_guitar/view_presets.c
    apps/aether_guitar/view_settings.c
    lib/crt0.S
)
target_include_directories(user_aether_guitar PRIVATE
    apps/aether_guitar
    lib/libAetherChain/include
    lib/libAetherPlugin/include
    lib/libAetherFX/include
    lib/libAetherAmp/include
    lib/libAetherDSP/include
    lib/libAetherAudioWidget/include
    lib/libAetherAudioWidget/src
    lib/libaetheraudio/include
    lib/include
    ${POSIX_INC_DIR}
)
target_compile_options(user_aether_guitar PRIVATE
    ${USER_COMPILE_FLAGS}
    -Wno-unused-parameter
    -Wno-unused-function
    -Wno-sign-compare
)
target_link_libraries(user_aether_guitar PRIVATE
    libAetherChain
    libAetherFX
    libAetherAmp
    libAetherPlugin
    libAetherDSP
    libAetherAudioWidget
    libaetheraudio
    libwidget
    libaether
)
target_link_options(user_aether_guitar PRIVATE
    -T ${USER_LINKER_SCRIPT} -nostdlib -static -Wl,--build-id=none
)
set_target_properties(user_aether_guitar PROPERTIES
    OUTPUT_NAME "aether_guitar" SUFFIX ""
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/initrd_staging
)

# Copy AetherGuitar BMP assets into initrd_staging/aeguitar/
set(AEGUITAR_ASSETS_SRC ${CMAKE_CURRENT_SOURCE_DIR}/apps/aether_guitar/assets)
set(AEGUITAR_ASSETS_DST ${CMAKE_BINARY_DIR}/initrd_staging/aeguitar)
add_custom_command(
    OUTPUT  ${AEGUITAR_ASSETS_DST}/pedalboard_bg.bmp
    COMMAND ${CMAKE_COMMAND} -E make_directory ${AEGUITAR_ASSETS_DST}
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${AEGUITAR_ASSETS_SRC}
                             ${AEGUITAR_ASSETS_DST}
    DEPENDS ${AEGUITAR_ASSETS_SRC}/pedalboard_bg.bmp
            ${AEGUITAR_ASSETS_SRC}/pedal_blue.bmp
            ${AEGUITAR_ASSETS_SRC}/pedal_cobalt.bmp
            ${AEGUITAR_ASSETS_SRC}/pedal_teal.bmp
            ${AEGUITAR_ASSETS_SRC}/pedal_amber.bmp
            ${AEGUITAR_ASSETS_SRC}/pedal_violet.bmp
            ${AEGUITAR_ASSETS_SRC}/marshall_amp.bmp
            ${AEGUITAR_ASSETS_SRC}/fender_amp.bmp
            ${AEGUITAR_ASSETS_SRC}/vox_amp.bmp
            ${AEGUITAR_ASSETS_SRC}/knob_base.bmp
            ${AEGUITAR_ASSETS_SRC}/stomp_off.bmp
            ${AEGUITAR_ASSETS_SRC}/stomp_on.bmp
            ${AEGUITAR_ASSETS_SRC}/led_green_on.bmp
            ${AEGUITAR_ASSETS_SRC}/led_green_off.bmp
            ${AEGUITAR_ASSETS_SRC}/tuner_bg.bmp
    COMMENT "Copying AetherGuitar BMP assets to initrd"
)
add_custom_target(aeguitar_assets ALL
    DEPENDS ${AEGUITAR_ASSETS_DST}/pedalboard_bg.bmp
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
            ${INITRD_FILES_SRC}/apps/tictactoe.app
            ${INITRD_FILES_SRC}/apps/telnet.app
            ${INITRD_FILES_SRC}/apps/aether_top.app
            ${INITRD_FILES_SRC}/apps/posix_test.app
            ${INITRD_FILES_SRC}/apps/img_test.app
            ${INITRD_FILES_SRC}/apps/font_test.app
            ${INITRD_FILES_SRC}/apps/html_test.app
            ${INITRD_FILES_SRC}/apps/js_test.app
            ${INITRD_FILES_SRC}/apps/compositor.app
            ${INITRD_FILES_SRC}/apps/ns_test.app
            ${INITRD_FILES_SRC}/apps/fetch_test.app
            ${INITRD_FILES_SRC}/apps/aether_mail.app
            ${INITRD_FILES_SRC}/apps/aether_irc.app
    COMMENT "Copying initrd static files"
)

# Step 1: build the CPIO archive from the staging directory
add_custom_command(
    OUTPUT  ${INITRD_CPIO}
    COMMAND ${CMAKE_SOURCE_DIR}/scripts/make_initrd.sh
                ${INITRD_STAGING}
                ${INITRD_CPIO}
    DEPENDS user_init user_login user_dock user_topbar user_notif_center user_desktop user_compositor user_aether_term user_statusbar user_files
            user_textviewer user_calculator user_tictactoe user_widget_demo
            user_aether_editor user_aether_http user_telnet user_aether_top user_aether_mail user_aether_irc
            user_sys_prefs user_aether_sound user_aether_guitar aeguitar_assets
            user_posix_test
            ${INTERP_TARGET}
            ${IMG_TEST_TARGET}
            ${FONT_TEST_TARGET}
            ${HTML_TEST_TARGET}
            ${JS_TEST_TARGET}
            ${NS_TEST_TARGET}
            ${FETCH_TEST_TARGET}
            ${BROWSER_TEST_TARGET}
            ${AETHER_BROWSER_TARGET}
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
