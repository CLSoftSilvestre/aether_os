#!/usr/bin/env bash
# Phase 7.5 — Fetch NetSurf core and libnslog
#
# Uses the same download.netsurf-browser.org tarball server as
# scripts/fetch_netsurf_libs.sh (Phase 7.3).
#
# Run once from the repo root:
#   scripts/fetch_netsurf.sh
#
# Populates:
#   userspace/vendor/netsurf/   — NetSurf core 3.11
#   userspace/vendor/libnslog/  — structured logging library 0.1.3

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
VENDOR_DIR="${SCRIPT_DIR}/../userspace/vendor"
CACHE_DIR="/tmp/netsurf-src"

LIB_BASE="https://download.netsurf-browser.org/libs/releases"
NS_BASE="https://download.netsurf-browser.org/netsurf/releases/source"

mkdir -p "${CACHE_DIR}" "${VENDOR_DIR}"

# ── fetch helper (same pattern as fetch_netsurf_libs.sh) ─────────────────────

fetch_tarball() {
    local url="$1"
    local tarball="${url##*/}"
    local cache="${CACHE_DIR}/${tarball}"

    if [ ! -f "${cache}" ]; then
        echo "  Downloading ${tarball}..." >&2
        curl -fL -o "${cache}" "${url}" >&2
    else
        echo "  (cached) ${tarball}" >&2
    fi
    echo "${cache}"   # only the path goes to stdout for command substitution
}

# ── libnslog 0.1.3 ───────────────────────────────────────────────────────────

NSLOG_DIR="${VENDOR_DIR}/libnslog"
if [ -d "${NSLOG_DIR}/include" ]; then
    echo "[fetch_netsurf] vendor/libnslog/ already present — skipping."
else
    echo "[fetch_netsurf] Fetching libnslog 0.1.3..."
    cache=$(fetch_tarball "${LIB_BASE}/libnslog-0.1.3-src.tar.gz")
    rm -rf "${NSLOG_DIR}"
    tar -xzf "${cache}" -C "${VENDOR_DIR}"
    mv "${VENDOR_DIR}/libnslog-0.1.3" "${NSLOG_DIR}"
    echo "[fetch_netsurf]   → ${NSLOG_DIR}"
fi

# ── NetSurf core 3.11 ────────────────────────────────────────────────────────

NS_DIR="${VENDOR_DIR}/netsurf"
if [ -d "${NS_DIR}/desktop" ]; then
    echo "[fetch_netsurf] vendor/netsurf/ already present — skipping."
else
    echo "[fetch_netsurf] Fetching NetSurf 3.11..."
    cache=$(fetch_tarball "${NS_BASE}/netsurf-3.11-src.tar.gz")
    rm -rf "${NS_DIR}"
    tar -xzf "${cache}" -C "${VENDOR_DIR}"
    mv "${VENDOR_DIR}/netsurf-3.11" "${NS_DIR}"
    echo "[fetch_netsurf]   → ${NS_DIR}"
fi

echo ""
echo "[fetch_netsurf] Done."
echo "  vendor/libnslog/  — libnslog 0.1.3"
echo "  vendor/netsurf/   — NetSurf 3.11"
echo ""
echo "Next: ninja -C build"
