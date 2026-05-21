#!/bin/bash
# scripts/make_usb_disk.sh — Create a 32MB FAT32 USB disk image for QEMU testing
#
# The image is attached to the qemu-xhci controller as a USB mass storage device.
# AetherOS mounts it read-only at /usb via the USB FAT32 driver (Phase 5.2.12).
#
# Usage:
#   bash scripts/make_usb_disk.sh          — creates build/usb_disk.img
#   bash scripts/make_usb_disk.sh clean    — removes the image

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/../build"
IMG="${BUILD_DIR}/usb_disk.img"

if [ "${1}" = "clean" ]; then
    rm -f "${IMG}"
    echo "[USB] Removed ${IMG}"
    exit 0
fi

mkdir -p "${BUILD_DIR}"

SIZE_MB=64    # FAT32 needs ≥ 65525 clusters; 32MB at 512-byte clusters falls short

echo "[USB] Creating ${SIZE_MB}MB FAT32 image at ${IMG}..."

if [[ "$(uname)" == "Darwin" ]]; then
    # macOS: let hdiutil create a proper FAT32 disk image natively (-layout NONE =
    # no MBR wrapper, just a raw VBR — our USB FAT32 driver detects both forms).
    # Then convert the .dmg to a flat raw .img that QEMU can use directly.
    TMP="${IMG%.img}"
    rm -f "${TMP}.dmg" "${TMP}.cdr" "${IMG}"

    hdiutil create -size ${SIZE_MB}m -fs FAT32 -volname AETHUSB "${TMP}" -quiet

    # Populate — mount the .dmg directly
    MNT=$(mktemp -d /tmp/usb_mnt.XXXXXX)
    hdiutil attach "${TMP}.dmg" -mountpoint "${MNT}" -nobrowse -quiet

    mkdir -p "${MNT}/audio" "${MNT}/config"
    echo "AetherOS USB test disk" > "${MNT}/readme.txt"
    echo "# USB audio config placeholder" > "${MNT}/config/audio.conf"

    hdiutil detach "${MNT}" -quiet
    rmdir "${MNT}"

    # Convert UDRW .dmg → raw .img (UDTO = "CD/DVD master" = flat raw bytes)
    hdiutil convert "${TMP}.dmg" -format UDTO -o "${TMP}" -ov -quiet
    mv "${TMP}.cdr" "${IMG}"
    rm -f "${TMP}.dmg"
else
    # Linux: mkfs.fat works directly on raw files
    if ! command -v mkfs.fat &>/dev/null; then
        echo "[ERROR] mkfs.fat not found — install dosfstools"
        exit 1
    fi
    dd if=/dev/zero of="${IMG}" bs=1M count=${SIZE_MB} status=none
    mkfs.fat -F 32 -n AETHUSB "${IMG}"

    MNT=$(mktemp -d /tmp/usb_mnt.XXXXXX)
    sudo mount -o loop,uid=$(id -u),gid=$(id -g) "${IMG}" "${MNT}"

    mkdir -p "${MNT}/audio" "${MNT}/config"
    echo "AetherOS USB test disk" > "${MNT}/readme.txt"
    echo "# USB audio config placeholder" > "${MNT}/config/audio.conf"

    sudo umount "${MNT}"
    rmdir "${MNT}"
fi

echo "[USB] Done: ${IMG}"
echo "[USB] Attach with: -drive file=${IMG},format=raw,if=none,id=usb0 -device usb-storage,bus=xhci.0,drive=usb0"
echo "[USB] Or just run: ./scripts/run_qemu.sh (auto-detected if present in build/)"
