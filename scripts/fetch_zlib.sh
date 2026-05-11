#!/bin/bash
# scripts/fetch_zlib.sh — Download and unpack zlib 1.3.1 into userspace/vendor/zlib/
#
# Run once before building:  scripts/fetch_zlib.sh
# After this, ninja -C build will compile vendor_zlib.
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
VENDOR_DIR="${SCRIPT_DIR}/../userspace/vendor"
ZLIB_VER="1.3.1"
ZLIB_DIR="${VENDOR_DIR}/zlib"
TARBALL="/tmp/zlib-${ZLIB_VER}.tar.gz"

echo "[fetch_zlib] Fetching zlib ${ZLIB_VER}..."
# Remove cached file if it's too small to be a valid tarball (failed download)
if [ -f "${TARBALL}" ] && [ "$(wc -c < "${TARBALL}")" -lt 100000 ]; then
    echo "[fetch_zlib] Removing invalid cached tarball (too small)"
    rm -f "${TARBALL}"
fi
if [ ! -f "${TARBALL}" ]; then
    curl -fL -o "${TARBALL}" \
        "https://github.com/madler/zlib/releases/download/v${ZLIB_VER}/zlib-${ZLIB_VER}.tar.gz"
fi

echo "[fetch_zlib] Extracting to ${ZLIB_DIR}..."
rm -rf "${ZLIB_DIR}"
mkdir -p "${VENDOR_DIR}"
tar -xzf "${TARBALL}" -C "${VENDOR_DIR}"
mv "${VENDOR_DIR}/zlib-${ZLIB_VER}" "${ZLIB_DIR}"

echo "[fetch_zlib] zlib ${ZLIB_VER} ready — run: ninja -C build"
