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

# /images/ — test images for Phase 7.1 img_test (PNG, JPEG)
mmd -i "${DISK}" ::images

IMAGES_ASSET_DIR="${SCRIPT_DIR}/../assets/images"
echo "[DISK] Generating Phase 7.1 test images..."
if python3 "${SCRIPT_DIR}/gen_test_images.py" "${IMAGES_ASSET_DIR}" 2>&1; then
    img_count=0
    for img in "${IMAGES_ASSET_DIR}"/*; do
        [ -f "$img" ] || continue
        fname="$(basename "${img}")"
        mcopy -i "${DISK}" "${img}" "::images/${fname}"
        img_count=$((img_count + 1))
    done
    echo "[DISK] Copied ${img_count} test image(s) to /images/"
else
    echo "[DISK] Note: gen_test_images.py failed — /images/ will be empty (img_test will skip JPEG)"
fi

# /fonts/ — TrueType fonts for Phase 7.2 FreeType rendering
mmd -i "${DISK}" ::fonts

FONTS_ASSET_DIR="${SCRIPT_DIR}/../assets/fonts"
if [ -d "${FONTS_ASSET_DIR}" ]; then
    font_count=0
    for font in "${FONTS_ASSET_DIR}"/*.ttf "${FONTS_ASSET_DIR}"/*.otf; do
        [ -f "$font" ] || continue
        fname="$(basename "${font}")"
        mcopy -i "${DISK}" "${font}" "::fonts/${fname}"
        font_count=$((font_count + 1))
    done
    if [ "${font_count}" -gt 0 ]; then
        echo "[DISK] Copied ${font_count} font(s) to /fonts/"
    else
        echo "[DISK] Note: no fonts in assets/fonts/ — run scripts/fetch_fonts.sh"
    fi
else
    echo "[DISK] Note: assets/fonts/ not found — run scripts/fetch_fonts.sh"
fi

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

# /icons/ — BMP icons (Phase 6.2)
# Expected files: 48×48 px, 24-bpp or 32-bpp uncompressed BMP.
# Background / transparent areas must be filled with RGB(255,0,255) — pure magenta.
#
# App icons:    icon_term.bmp  icon_files.bmp  icon_editor.bmp
#               icon_calc.bmp  icon_tictactoe.bmp  icon_widget.bmp
#               icon_text.bmp  icon_telnet.bmp
# File types:   file_folder.bmp  file_folder_open.bmp
#               file_txt.bmp  file_as.bmp  file_exec.bmp  file_generic.bmp
# Drives:       drive_fat32.bmp  drive_initrd.bmp  drive_afs.bmp
#
# If a file is missing the system falls back to the procedural vector icon.
mmd -i "${DISK}" ::icons

ICON_DIR="${SCRIPT_DIR}/../assets/icons"
if [ -d "${ICON_DIR}" ]; then
    icon_count=0
    for bmp in "${ICON_DIR}"/*.bmp; do
        [ -f "${bmp}" ] || continue
        fname="$(basename "${bmp}")"
        mcopy -i "${DISK}" "${bmp}" "::icons/${fname}"
        icon_count=$((icon_count + 1))
    done
    echo "[DISK] Copied ${icon_count} icon(s) from assets/icons/"
else
    echo "[DISK] Note: assets/icons/ not found — system will use procedural vector icons"
    echo "[DISK]       Create 48×48 BMP files there to override individual icons."
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
