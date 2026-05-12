#!/bin/bash
# scripts/fetch_freetype.sh — Download and set up FreeType 2.13.3
#
# Creates a minimal bare-metal build config (ftconfig.h) and patches
# ftoption.h to disable LZW, BZip2, HarfBuzz, and Brotli.
#
# Run once before building:  scripts/fetch_freetype.sh
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
VENDOR_DIR="${SCRIPT_DIR}/../userspace/vendor"
FT_VER="2.13.3"
FT_DIR="${VENDOR_DIR}/freetype"
TARBALL="/tmp/freetype-${FT_VER}.tar.gz"

echo "[fetch_freetype] Fetching FreeType ${FT_VER}..."
if [ -f "${TARBALL}" ] && [ "$(wc -c < "${TARBALL}")" -lt 100000 ]; then
    echo "[fetch_freetype] Removing invalid cached tarball"
    rm -f "${TARBALL}"
fi
if [ ! -f "${TARBALL}" ]; then
    curl -fL -o "${TARBALL}" \
        "https://github.com/freetype/freetype/archive/refs/tags/VER-2-13-3.tar.gz"
fi

echo "[fetch_freetype] Extracting to ${FT_DIR}..."
rm -rf "${FT_DIR}"
mkdir -p "${VENDOR_DIR}"
tar -xzf "${TARBALL}" -C "${VENDOR_DIR}"
mv "${VENDOR_DIR}/freetype-VER-2-13-3" "${FT_DIR}"

# ── ftconfig.h — replaces the ./configure-generated platform config ───────────
#
# AArch64 LP64: int=4, long=8, pointer=8. 64-bit arithmetic available.
# No Unix-specific features (no mmap, no fork, no pthreads).

cat > "${FT_DIR}/include/freetype/config/ftconfig.h" << 'EOF'
/* ftconfig.h — AetherOS AArch64 bare-metal config for FreeType 2.13.x.
 * Hand-crafted replacement for the ./configure-generated version.
 *
 * Mirrors what the standard generated file does: include ftstdlib.h,
 * define FT_SIZEOF_*, then pull in integer-types.h, public-macros.h,
 * mac-support.h from the FreeType distribution itself. */

#ifndef FTCONFIG_H_
#define FTCONFIG_H_

/* Standard freestanding headers */
#include <stddef.h>   /* size_t, ptrdiff_t                         */
#include <stdint.h>   /* uint8_t, uint32_t …                       */
#include <limits.h>   /* CHAR_BIT, UINT_MAX (read by integer-types.h) */

/* FreeType stdlib mapping: ft_memset → memset, ft_sprintf → sprintf … */
#include <freetype/config/ftstdlib.h>

/* Feature flags (FT_MAX_MODULES, T1_MAX_SUBRS_CALLS, …) */
#include <freetype/config/ftoption.h>

/* AArch64 LP64 sizes — defined before integer-types.h to skip auto-detect */
#define FT_SIZEOF_INT       4
#define FT_SIZEOF_LONG      8
#define FT_SIZEOF_LONG_LONG 8
#define FT_SIZEOF_VOID_P    8

/* FreeType integer typedefs and public API macros */
#include <freetype/config/integer-types.h>
#include <freetype/config/public-macros.h>
#include <freetype/config/mac-support.h>

/* No GCC visibility attributes — static bare-metal link */
#define FT_VISIBILITY_PRAGMA_PUSH
#define FT_VISIBILITY_PRAGMA_POP

/* Use portable C only */
#define FT_CONFIG_OPTION_NO_ASSEMBLER

#endif /* FTCONFIG_H_ */
EOF

# ── ftmodule.h — minimal module list (TrueType only) ─────────────────────────
#
# ftinit.c builds ft_default_modules[] from this file.  Only include modules
# whose source .c files are listed in the CMakeLists vendor_freetype target.

cat > "${FT_DIR}/include/freetype/config/ftmodule.h" << 'EOF'
/* ftmodule.h — AetherOS minimal FreeType module list. */
FT_USE_MODULE( FT_Driver_ClassRec,  tt_driver_class          )
FT_USE_MODULE( FT_Module_Class,     sfnt_module_class        )
FT_USE_MODULE( FT_Renderer_Class,   ft_smooth_renderer_class )
FT_USE_MODULE( FT_Renderer_Class,   ft_raster1_renderer_class )
FT_USE_MODULE( FT_Module_Class,     autofit_module_class     )
FT_USE_MODULE( FT_Module_Class,     psnames_module_class     )
EOF

# ── ftoption.h — append bare-metal overrides at the end ───────────────────────
#
# The distributed ftoption.h enables LZW (src/lzw/) by default.  We disable it
# (and other features that require extra source files or system libraries) so the
# minimal CMake source list is sufficient.

FT_OPTION="${FT_DIR}/include/freetype/config/ftoption.h"

cat >> "${FT_OPTION}" << 'EOF'

/* ── AetherOS bare-metal overrides (appended by fetch_freetype.sh) ───────── */
#undef  FT_CONFIG_OPTION_USE_LZW      /* no src/lzw/ in build */
#undef  FT_CONFIG_OPTION_USE_BZIP2    /* no libbz2             */
#undef  FT_CONFIG_OPTION_USE_PNG      /* libpng linked separately */
#undef  FT_CONFIG_OPTION_USE_HARFBUZZ /* HarfBuzz deferred      */
#undef  FT_CONFIG_OPTION_USE_BROTLI   /* no libbrotli           */
/* Use our vendor zlib for compressed font streams */
#define FT_CONFIG_OPTION_SYSTEM_ZLIB
EOF

echo "[fetch_freetype] FreeType ${FT_VER} ready — run: ninja -C build"
