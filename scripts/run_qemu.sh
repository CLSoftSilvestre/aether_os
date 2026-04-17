#!/bin/bash
# scripts/run_qemu.sh — Launch AetherOS in QEMU
#
# Machine: "virt" — a generic ARM virtual board. Simple, well-documented,
#          no firmware blobs required. Perfect for early kernel development.
#
# Usage:
#   ./scripts/run_qemu.sh          — normal run
#   ./scripts/run_qemu.sh --debug  — pause at boot, wait for GDB on port 1234

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/../build"
KERNEL_IMG="${BUILD_DIR}/kernel8.img"

if [ ! -f "${KERNEL_IMG}" ]; then
    echo "[ERROR] Kernel image not found: ${KERNEL_IMG}"
    echo "        Run: cmake --build build/ first"
    exit 1
fi

echo "[QEMU] Starting AetherOS..."
echo "[QEMU] Kernel: ${KERNEL_IMG}"
echo "[QEMU] Press Ctrl-A X to exit QEMU"
echo ""

# Base QEMU arguments
QEMU_ARGS=(
    -M virt,highmem=off     # Virtual ARM board, no >4GB memory regions
    -cpu cortex-a76         # Pi 5 CPU core
    -smp 1                  # Single core for now (Phase 2 adds SMP)
    -m 1G                   # 1GB RAM (matches Pi 5 minimum config)
    -kernel "${KERNEL_IMG}" # Load kernel image at RAM start (0x40000000)
    -serial mon:stdio       # UART0 + QEMU monitor → your terminal
    -nographic              # No graphical window (framebuffer comes in Phase 2)
    -no-reboot              # Don't restart on crash — make the failure visible
)

# Debug mode: pause at first instruction, expose GDB stub on port 1234
if [ "${1}" = "--debug" ]; then
    echo "[QEMU] Debug mode: connect GDB with:"
    echo "       aarch64-elf-gdb build/kernel/aether_kernel.elf"
    echo "       (gdb) target remote localhost:1234"
    echo "       (gdb) break kernel_main"
    echo "       (gdb) continue"
    echo ""
    QEMU_ARGS+=(-s -S)      # -s = GDB server on :1234, -S = pause at start
fi

qemu-system-aarch64 "${QEMU_ARGS[@]}"
