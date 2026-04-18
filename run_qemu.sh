#!/bin/bash
# AetherOS QEMU launcher
# - Graphics window shows the Lumina desktop
# - A new Terminal window opens for keyboard input

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

pkill -f "qemu-system-aarch64.*kernel8" 2>/dev/null
sleep 0.5

qemu-system-aarch64 \
  -M virt,highmem=off \
  -cpu cortex-a76 \
  -m 1G \
  -kernel "$SCRIPT_DIR/build/kernel8.img" \
  -device ramfb \
  -vga none \
  -display cocoa \
  -serial tcp:127.0.0.1:4444,server,nowait \
  -no-reboot &

sleep 2

# Open a new Terminal window connected to the serial port
osascript <<'EOF'
tell application "Terminal"
    activate
    do script "echo 'AetherOS serial console — type commands here, output appears in the QEMU window' && sleep 1 && nc 127.0.0.1 4444"
end tell
EOF
