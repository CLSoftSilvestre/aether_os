#!/bin/bash
# scripts/run_qemu.sh — Launch AetherOS in QEMU
#
# Machine: "virt" — a generic ARM virtual board. Simple, well-documented,
#          no firmware blobs required. Perfect for early kernel development.
#
# Usage:
#   ./scripts/run_qemu.sh              — graphical window + UART on stdio
#   ./scripts/run_qemu.sh --headless   — UART only, no graphical window
#   ./scripts/run_qemu.sh --debug      — graphical + GDB stub on port 1234

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
if [ "$HEADLESS" = "1" ]; then
    echo "[QEMU] Mode: headless (UART only)"
else
    echo "[QEMU] Mode: graphical (1024×768 ramfb + UART on stdio)"
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
    # Phase 4.0: ramfb provides a 1024×768 display window.
    # -vga none: disable default VGA device (conflicts with ramfb).
    # -device ramfb: QEMU renders fb_base[] to a graphical window.
    #
    # Phase 4.5: PL050/KMI keyboard and mouse devices.
    # On QEMU -M virt, these are present by default at:
    #   keyboard: 0x09050000, IRQ 52
    #   mouse:    0x09060000, IRQ 53
    # The graphical window routes host keyboard/mouse events to the KMI
    # controllers, which fire IRQs into our PL050 drivers.
    #
    # Verify device presence: in QEMU monitor (Ctrl-A C) run:
    #   info qtree | grep pl050
    QEMU_ARGS+=(
        -device ramfb
        -vga none
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
