#!/bin/bash
# scripts/fetch_libjpeg.sh — Download and unpack libjpeg 9f into userspace/vendor/libjpeg/
#
# Uses the IJG reference implementation (pure C, no SIMD).
# Run once before building:  scripts/fetch_libjpeg.sh
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
VENDOR_DIR="${SCRIPT_DIR}/../userspace/vendor"
JPEG_VER="9f"
JPEG_DIR="${VENDOR_DIR}/libjpeg"
TARBALL="/tmp/jpegsrc-${JPEG_VER}.tar.gz"

echo "[fetch_libjpeg] Fetching libjpeg ${JPEG_VER}..."
if [ -f "${TARBALL}" ] && [ "$(wc -c < "${TARBALL}")" -lt 100000 ]; then
    echo "[fetch_libjpeg] Removing invalid cached tarball (too small)"
    rm -f "${TARBALL}"
fi
if [ ! -f "${TARBALL}" ]; then
    curl -fL -o "${TARBALL}" \
        "http://www.ijg.org/files/jpegsrc.v${JPEG_VER}.tar.gz"
fi

echo "[fetch_libjpeg] Extracting to ${JPEG_DIR}..."
rm -rf "${JPEG_DIR}"
mkdir -p "${VENDOR_DIR}"
tar -xzf "${TARBALL}" -C "${VENDOR_DIR}"
mv "${VENDOR_DIR}/jpeg-${JPEG_VER}" "${JPEG_DIR}"

# Create jconfig.h for AArch64 bare-metal (normally produced by ./configure).
cat > "${JPEG_DIR}/jconfig.h" << 'EOF'
/* jconfig.h — AetherOS AArch64 bare-metal configuration for libjpeg 9f.
 * Hand-crafted replacement for the ./configure-generated file. */

#define HAVE_PROTOTYPES      1
#define HAVE_UNSIGNED_CHAR   1
#define HAVE_UNSIGNED_SHORT  1
#define HAVE_STDDEF_H        1
#define HAVE_STDLIB_H        1
#undef  NEED_BSD_STRINGS
#undef  NEED_SYS_TYPES_H
#undef  NEED_FAR_POINTERS
#undef  NEED_SHORT_EXTERNAL_NAMES
#undef  INCOMPLETE_TYPES_BROKEN

#ifdef  JPEG_INTERNALS
#undef  RIGHT_SHIFT_IS_UNSIGNED
#endif

#ifdef  JPEG_CJPEG_DJPEG
#define BMP_SUPPORTED
#define GIF_SUPPORTED
#define PPM_SUPPORTED
#undef  RLE_SUPPORTED
#define TARGA_SUPPORTED
#define TWO_FILE_COMMANDLINE
#undef  NEED_SIGNAL_CATCHER
#undef  DONT_USE_B_MODE
#undef  PROGRESS_REPORT
#endif
EOF

echo "[fetch_libjpeg] libjpeg ${JPEG_VER} ready — run: ninja -C build"
