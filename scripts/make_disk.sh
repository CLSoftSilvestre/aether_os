#!/bin/bash
# scripts/make_disk.sh — Create a FAT32 disk image for AetherOS Phase 5.2
#
# Requires: mtools (brew install mtools)
# Output:   build/disk.img  (32 MB FAT32, QEMU virtio-blk compatible)
#
# QEMU attaches it automatically when the image is present:
#   -drive file=build/disk.img,format=raw,if=none,id=hd0
#   -device virtio-blk-pci,drive=hd0
#
# run_qemu.sh does this automatically if build/disk.img exists.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/../build"
DISK="${BUILD_DIR}/disk.img"
SIZE_MB=32

# Ensure build directory exists
mkdir -p "${BUILD_DIR}"

echo "[DISK] Creating ${SIZE_MB}MB FAT32 disk image at ${DISK}"

# Create a blank raw image
dd if=/dev/zero of="${DISK}" bs=1M count="${SIZE_MB}" 2>/dev/null

# Format as FAT32 using mformat (mtools, macOS-compatible — no loop mount needed)
mformat -i "${DISK}" -F -v "AETHERDSK" ::

echo "[DISK] Populating directory structure..."

# Directories
mmd -i "${DISK}" ::docs
mmd -i "${DISK}" ::usr
mmd -i "${DISK}" ::var
mmd -i "${DISK}" ::home

# Root-level files
printf "Welcome to AetherOS!\n\nThis disk is mounted at / by the VFS layer.\nUse 'ls /' or 'cat /readme.txt' from aether_term.\n" \
    | mcopy -i "${DISK}" - ::readme.txt

printf "AetherOS v0.0.6\nPhase 5.2 — Filesystem and Storage\nBuild date: $(date +%Y-%m-%d)\n" \
    | mcopy -i "${DISK}" - ::version.txt

# docs/ subdirectory
printf "AetherOS Disk Filesystem\n========================\n\nThis volume is a standard FAT32 disk readable by any OS.\nFiles placed here persist across reboots (when using a real SD card).\n\nDirectory layout:\n  /             root\n  /docs/        documentation\n  /usr/         user programs (future)\n  /var/         variable data (logs, spool)\n  /home/        user home directories\n" \
    | mcopy -i "${DISK}" - "::docs/readme.txt"

printf "Phase 5.2 adds:\n  - virtio-blk PCI driver (kernel/drivers/block/virtio_blk.c)\n  - FAT32 read-only parser (kernel/fs/fat32.c)\n  - VFS layer (kernel/fs/vfs.c)\n  - SYS_FS_OPEN/READ/CLOSE/READDIR syscalls (800-803)\n  - Disk mounted at /, initrd at /initrd\n" \
    | mcopy -i "${DISK}" - "::docs/changelog.txt"

printf "Line 1: The quick brown fox jumps over the lazy dog.\nLine 2: Pack my box with five dozen liquor jugs.\nLine 3: How vexingly quick daft zebras jump!\nLine 4: AetherOS filesystem test file — reading works!\n" \
    | mcopy -i "${DISK}" - "::docs/testfile.txt"

# /scripts/ — user AetherScript files (Phase 5.5)
mmd -i "${DISK}" ::scripts

printf "-- AetherOS hello world\nprint(\"Hello from AetherOS \" .. os.version)\nfor i = 1, 5 do\n    print(i)\nend\n" \
    | mcopy -i "${DISK}" - "::scripts/hello.as"

# /tmp/ — temporary files (Phase 5.5, for script.as staging)
mmd -i "${DISK}" ::tmp

# /apps/ — app manifests for desktop icon launcher (Phase 5.4)
mmd -i "${DISK}" ::apps

# Wallpaper — lumina_bg.bmp (Phase 6.1)
BMP_SRC="${SCRIPT_DIR}/../assets/lumina_bg.bmp"
if [ -f "${BMP_SRC}" ]; then
    mcopy -i "${DISK}" "${BMP_SRC}" ::lumina_bg.bmp
    echo "[DISK] Copied lumina_bg.bmp ($(du -sh "${BMP_SRC}" | cut -f1))"
else
    echo "[DISK] Warning: assets/lumina_bg.bmp not found — wallpaper will use procedural fallback"
fi

printf "name=Terminal\nicon=icon_term\nexec=/aether_term\ndescription=Terminal emulator\n" \
    | mcopy -i "${DISK}" - ::apps/aether_term.app

printf "name=Files\nicon=icon_files\nexec=/files\ndescription=File browser\n" \
    | mcopy -i "${DISK}" - ::apps/files.app

printf "name=AetherEditor\nicon=icon_editor\nexec=/aether_editor\ndescription=Script editor\n" \
    | mcopy -i "${DISK}" - ::apps/aether_editor.app

echo "[DISK] Contents:"
echo "  /:"
mdir -i "${DISK}" :: 2>/dev/null | grep -v "Volume\|^$" || true
echo "  /docs/:"
mdir -i "${DISK}" ::docs 2>/dev/null | grep -v "Volume\|^$" || true
echo "  /apps/:"
mdir -i "${DISK}" ::apps 2>/dev/null | grep -v "Volume\|^$" || true
echo "  /scripts/:"
mdir -i "${DISK}" ::scripts 2>/dev/null | grep -v "Volume\|^$" || true

DISK_SIZE=$(du -sh "${DISK}" 2>/dev/null | cut -f1)
echo "[DISK] Done — ${DISK} (${DISK_SIZE})"
echo "[DISK] Run scripts/run_qemu.sh to boot with this disk attached."
