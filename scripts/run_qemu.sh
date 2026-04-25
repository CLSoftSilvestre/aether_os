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
#   ./scripts/run_qemu.sh --headless   — UART only, no graphical window
#   ./scripts/run_qemu.sh --debug      — VNC + GDB stub on port 1234
#
# NOTE: QEMU 11.0.0 on macOS Sequoia (15) does NOT route keyboard/mouse
#       events to virtual devices when using the Cocoa display backend.
#       VNC routes input correctly through QEMU's input mux.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/../build"
KERNEL_IMG="${BUILD_DIR}/kernel8.img"

if [ ! -f "${KERNEL_IMG}" ]; then
    echo "[ERROR] Kernel image not found: ${KERNEL_IMG}"
    echo "        Run: cmake --build build/ first"
    exit 1
fi

# Parse flags
HEADLESS=0
DEBUG=0
for arg in "$@"; do
    case "$arg" in
        --headless) HEADLESS=1 ;;
        --debug)    DEBUG=1    ;;
    esac
done

echo "[QEMU] Starting AetherOS..."
echo "[QEMU] Kernel: ${KERNEL_IMG}"
echo "[QEMU] QEMU version: $(qemu-system-aarch64 --version | head -1)"
if [ "$HEADLESS" = "1" ]; then
    echo "[QEMU] Mode: headless (UART only)"
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
fi
echo "[QEMU] Press Ctrl-A X to exit QEMU"
echo ""

# Base QEMU arguments
QEMU_ARGS=(
    -M virt,highmem=off     # Virtual ARM board, no >4GB memory regions
    -cpu cortex-a76         # Pi 5 CPU core
    -smp 1                  # Single core for now
    -m 1G                   # 1GB RAM
    -kernel "${KERNEL_IMG}" # Load kernel image
    -serial mon:stdio       # UART0 + QEMU monitor → your terminal
    -no-reboot              # Don't restart on crash
)

if [ "$HEADLESS" = "1" ]; then
    QEMU_ARGS+=(-nographic)
else
    # VNC display: routes keyboard/mouse through QEMU's input mux correctly.
    # QEMU 11.0.0 Cocoa on macOS Sequoia does NOT forward events to virtual
    # input devices; VNC does.  The kernel virtio_input driver receives all
    # keyboard and mouse events from the VNC client via virtio-keyboard-pci
    # and virtio-tablet-pci.
    QEMU_ARGS+=(
        -device ramfb
        -vga none
        -display vnc=127.0.0.1:0
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
