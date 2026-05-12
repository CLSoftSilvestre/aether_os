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
    lib/libaether/icon_cache.c
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
    lib/libwidget/progress.c
    lib/libwidget/anim.c
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

    add_library(vendor_libdom STATIC ${LIBDOM_SRCS})
    target_include_directories(vendor_libdom PUBLIC
        ${LDOM_DIR}/include
        ${POSIX_INC_DIR}
        lib/include
    )
    target_include_directories(vendor_libdom PRIVATE
        ${LDOM_DIR}/src
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
            user_aether_http user_telnet user_aether_top user_posix_test
            ${INTERP_TARGET}
            ${IMG_TEST_TARGET}
            ${FONT_TEST_TARGET}
            ${HTML_TEST_TARGET}
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
