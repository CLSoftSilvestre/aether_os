#!/bin/bash
# scripts/run_qemu.sh — Launch AetherOS in QEMU
#
# Machine: "virt" — a generic ARM virtual board. Simple, well-documented,
#          no firmware blobs required. Perfect for early kernel development.
#
# Usage:
#   ./scripts/run_qemu.sh              — VNC display + UART on stdio
#                                        Connect: Finder → Go → Connect to Server…
#                                        URL: vnc://localhost:5900
#                                        Stop: kill the terminal (Ctrl-C)
#                                        QEMU monitor socket: /tmp/aether-qemu-monitor.sock
#   ./scripts/run_qemu.sh --headless   — UART only, no graphical window
#                                        Stop: Ctrl-A X in the terminal
#   ./scripts/run_qemu.sh --direct     — Native QEMU Cocoa window (no VNC)
#                                        Keyboard and mouse work via virtio devices
#                                        Stop: QEMU menu or Ctrl-A X
#   ./scripts/run_qemu.sh --debug      — VNC + GDB stub on port 1234
#
# NOTE: QEMU 11.0.0 on macOS Sequoia (15) does NOT route keyboard/mouse
#       events to virtual devices when using the Cocoa display backend.
#       VNC routes input correctly through QEMU's input mux.
#
# NOTE: In VNC mode the QEMU monitor is on a Unix socket (not stdio) to avoid
#       VS Code's PTY cooked-mode bug that stalls VNC framebuffer refreshes.
#       Access the monitor with: socat - /tmp/aether-qemu-monitor.sock

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/../build"
KERNEL_IMG="${BUILD_DIR}/kernel8.img"
DISK_IMG="${BUILD_DIR}/disk.img"
AFS_IMG="${BUILD_DIR}/afs.img"

if [ ! -f "${KERNEL_IMG}" ]; then
    echo "[ERROR] Kernel image not found: ${KERNEL_IMG}"
    echo "        Run: cmake --build build/ first"
    exit 1
fi

# Parse flags
HEADLESS=0
DIRECT=0
DEBUG=0
for arg in "$@"; do
    case "$arg" in
        --headless) HEADLESS=1 ;;
        --direct)   DIRECT=1   ;;
        --debug)    DEBUG=1    ;;
    esac
done

echo "[QEMU] Starting AetherOS..."
echo "[QEMU] Kernel: ${KERNEL_IMG}"
echo "[QEMU] QEMU version: $(qemu-system-aarch64 --version | head -1)"
if [ "$HEADLESS" = "1" ]; then
    echo "[QEMU] Mode: headless (UART only)"
    echo "[QEMU] Press Ctrl-A X to exit QEMU"
elif [ "$DIRECT" = "1" ]; then
    echo "[QEMU] Mode: direct Cocoa window (keyboard and mouse active)"
    echo "[QEMU] Press Ctrl-A X or close the QEMU window to exit"
else
    echo "[QEMU] Mode: VNC display"
    echo "[QEMU] ┌─────────────────────────────────────────────────────────────┐"
    echo "[QEMU] │  To see the screen and use keyboard/mouse:                  │"
    echo "[QEMU] │                                                              │"
    echo "[QEMU] │  macOS: Finder → Go → Connect to Server…                    │"
    echo "[QEMU] │         Type:  vnc://localhost:5900   → click Connect        │"
    echo "[QEMU] │                                                              │"
    echo "[QEMU] │  The AetherOS framebuffer will appear in the Screen Sharing  │"
    echo "[QEMU] │  window. Click inside it — keyboard and mouse work normally. │"
    echo "[QEMU] └─────────────────────────────────────────────────────────────┘"
    echo "[QEMU] Stop: Ctrl-C in this terminal (Ctrl-A X is not available in VNC mode)"
    echo "[QEMU] Monitor: socat - /tmp/aether-qemu-monitor.sock"
fi
echo ""

# Base QEMU arguments
QEMU_ARGS=(
    -M virt,highmem=off     # Virtual ARM board, no >4GB memory regions
    -cpu cortex-a76         # Pi 5 CPU core
    -smp 1                  # Single core for now
    -m 1G                   # 1GB RAM
    -kernel "${KERNEL_IMG}" # Load kernel image
    -no-reboot              # Don't restart on crash
    -rtc base=localtime     # PL031 RTC tracks host local time

    # Network (Phase 5.1): user-mode NAT networking via virtio-net-pci
    # QEMU provides: DHCP lease 10.0.2.15/24, gateway 10.0.2.2, DNS 10.0.2.3
    # disable-legacy=on forces pure modern VirtIO 1.x (devid=0x1041, no transitional quirks)
    -netdev user,id=n0
    -object filter-dump,id=pcap0,netdev=n0,file=/tmp/aether.pcap
    -device virtio-net-pci,netdev=n0,disable-legacy=on
)

# Block storage (Phase 5.2): hd0 = FAT32 disk.img, hd1 = AetherFS afs.img
# Create with: bash scripts/make_disk.sh  and  bash scripts/make_afs.sh
if [ -f "${DISK_IMG}" ]; then
    echo "[QEMU] hd0: ${DISK_IMG} (FAT32  → /)"
    QEMU_ARGS+=(
        -drive file="${DISK_IMG}",format=raw,if=none,id=hd0
        -device virtio-blk-pci,drive=hd0
    )
else
    echo "[QEMU] hd0: not found — run scripts/make_disk.sh to create FAT32 image"
fi

if [ -f "${AFS_IMG}" ]; then
    echo "[QEMU] hd1: ${AFS_IMG} (AetherFS → /afs)"
    QEMU_ARGS+=(
        -drive file="${AFS_IMG}",format=raw,if=none,id=hd1
        -device virtio-blk-pci,drive=hd1
    )
else
    echo "[QEMU] hd1: not found — run scripts/make_afs.sh to create AetherFS image"
fi

if [ "$HEADLESS" = "1" ]; then
    # In headless mode: mon:stdio is fine — the external terminal handles raw
    # mode correctly and Ctrl-A X works as expected.
    QEMU_ARGS+=(
        -serial mon:stdio
        -nographic
    )
elif [ "$DIRECT" = "1" ]; then
    # Direct Cocoa window: opens a native QEMU window without VNC.
    # Keyboard and mouse route correctly to virtio-keyboard-pci / virtio-tablet-pci.
    QEMU_ARGS+=(
        -serial mon:stdio
        -device ramfb
        -vga none
        -display cocoa
        -device virtio-tablet-pci
        -device virtio-keyboard-pci
    )
else
    # VNC display: routes keyboard/mouse through QEMU's input mux correctly.
    # QEMU 11.0.0 Cocoa on macOS Sequoia does NOT forward events to virtual
    # input devices; VNC does.  The kernel virtio_input driver receives all
    # keyboard and mouse events from the VNC client via virtio-keyboard-pci
    # and virtio-tablet-pci.
    #
    # IMPORTANT: do NOT use -serial mon:stdio here.  VS Code's integrated
    # terminal PTY ignores tcsetattr(TCSANOW) raw-mode requests, so stdin
    # stays in cooked (line-buffered) mode.  QEMU's GLib main loop then only
    # wakes up for stdin after Enter is pressed, which stalls VNC framebuffer
    # refreshes until that happens.  Splitting the monitor onto its own socket
    # keeps stdio line-buffered for UART output while the event loop can wake
    # freely on VNC socket activity.
    #
    # -k pt: tells QEMU's VNC server to use the Portuguese keyboard layout
    # when translating X11 keysyms (sent by the VNC client) into evdev
    # keycodes for virtio-keyboard-pci.  Without this, keys specific to the
    # PT-PT layout (ç, º, «, etc.) are not mapped to the correct positions.
    QEMU_ARGS+=(
        -serial stdio
        -monitor unix:/tmp/aether-qemu-monitor.sock,server,nowait
        -device ramfb
        -vga none
        -display vnc=127.0.0.1:0
        -k pt
        -device virtio-tablet-pci
        -device virtio-keyboard-pci
    )
fi

if [ "$DEBUG" = "1" ]; then
    echo "[QEMU] Debug mode: connect GDB with:"
    echo "       aarch64-elf-gdb build/kernel/aether_kernel.elf"
    echo "       (gdb) target remote localhost:1234"
    echo "       (gdb) break kernel_main"
    echo "       (gdb) continue"
    echo ""
    QEMU_ARGS+=(-s -S)
fi

qemu-system-aarch64 "${QEMU_ARGS[@]}"
