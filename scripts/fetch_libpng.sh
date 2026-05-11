#!/bin/bash
# scripts/fetch_libpng.sh — Download and unpack libpng 1.6.43 into userspace/vendor/libpng/
#
# Run once before building:  scripts/fetch_libpng.sh
# Prerequisite: fetch_zlib.sh must already have been run.
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
VENDOR_DIR="${SCRIPT_DIR}/../userspace/vendor"
PNG_VER="1.6.43"
PNG_DIR="${VENDOR_DIR}/libpng"
TARBALL="/tmp/libpng-${PNG_VER}.tar.gz"

echo "[fetch_libpng] Fetching libpng ${PNG_VER}..."
if [ -f "${TARBALL}" ] && [ "$(wc -c < "${TARBALL}")" -lt 100000 ]; then
    echo "[fetch_libpng] Removing invalid cached tarball (too small)"
    rm -f "${TARBALL}"
fi
if [ ! -f "${TARBALL}" ]; then
    curl -fL -o "${TARBALL}" \
        "https://github.com/pnggroup/libpng/archive/refs/tags/v${PNG_VER}.tar.gz"
fi

echo "[fetch_libpng] Extracting to ${PNG_DIR}..."
rm -rf "${PNG_DIR}"
mkdir -p "${VENDOR_DIR}"
tar -xzf "${TARBALL}" -C "${VENDOR_DIR}"
mv "${VENDOR_DIR}/libpng-${PNG_VER}" "${PNG_DIR}"

# Use the prebuilt configuration (avoids running autoconf/configure).
# Then force PNG_ARM_NEON_OPT=0 — the prebuilt sets it to 2 (auto-detect), which
# pulls in arm/ NEON files we aren't compiling.
echo "[fetch_libpng] Installing pnglibconf.h from prebuilt..."
cp "${PNG_DIR}/scripts/pnglibconf.h.prebuilt" "${PNG_DIR}/pnglibconf.h"
# Overwrite the NEON opt flag (sed -i works on both Linux and macOS with .bak)
sed -i.bak 's/#define PNG_ARM_NEON_OPT .*/#define PNG_ARM_NEON_OPT 0/' \
    "${PNG_DIR}/pnglibconf.h" && rm -f "${PNG_DIR}/pnglibconf.h.bak"

echo "[fetch_libpng] libpng ${PNG_VER} ready — run: ninja -C build"
