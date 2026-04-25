#!/bin/bash
# AetherOS QEMU launcher — delegates to scripts/run_qemu.sh
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
exec "$SCRIPT_DIR/scripts/run_qemu.sh" "$@"
