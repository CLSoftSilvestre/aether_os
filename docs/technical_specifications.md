# Project AetherOS: Technical Specification & Development Roadmap

## Executive Summary

AetherOS is a modern, visually striking operating system designed specifically for Raspberry Pi hardware, with QEMU-based development and testing workflows. Built from the ground up with a custom kernel, it prioritizes real hardware performance while maintaining developer ergonomics through emulation-first development.

---

## Architecture Overview

### Target Hardware Specifications

| Component | Specification |
|-----------|--------------|
| **Primary Target** | Raspberry Pi 4/5 (ARM Cortex-A72/A76) |
| **Secondary Target** | Raspberry Pi 3 (ARM Cortex-A53) |
| **Memory Requirements** | Minimum 512MB, Recommended 2GB+ |
| **Boot Medium** | SD Card / USB / Network Boot |
| **Graphics** | VideoCore VI/VII (V3D), HDMI output |
| **Peripherals** | USB 3.0, Ethernet, WiFi, Bluetooth, GPIO |

### Development Environment

- **Primary Emulator**: QEMU 7.0+ with ARM virt machine
- **Secondary Emulator**: Raspberry Pi Emulator (rpiboot for firmware testing)
- **Cross-Compiler**: AArch64 ELF GCC toolchain
- **Build System**: CMake + Ninja
- **Version Control**: Git with conventional commits

---

## Phase 1: Foundation & Boot (Weeks 1-4)

### Milestone 1.1: Bootloader & Firmware Interface

**Technical Requirements:**
- Implement AArch64 boot sequence following ARM Trusted Firmware specification
- Support both EL3 (Secure) and EL2 (Hypervisor) entry points
- Parse Device Tree Blob (DTB) passed by firmware
- Initialize UART0 (PL011) for early debugging output
- Set up MMU with identity mapping for initial boot

**Deliverables:**

/boot/
├── boot.S              # Assembly entry point
├── linker.ld           # Memory layout script
├── dtb_parser.c        # Device tree parsing
├── uart_pl011.c        # Serial output driver
└── mmu_init.c          # Initial page tables


**Key Technical Decisions:**

- **Boot Protocol**: Follow Raspberry Pi's start4.elf chain-loading
- **Memory Layout**: 
  - 0x80000: Kernel load address (Raspberry Pi convention)
  - 0x0: Device MMIO region (identity mapped)
  - Stack top: 0x80000 (growing downward)

**Testing Strategy:**

```bash
# QEMU test command
qemu-system-aarch64 \
  -M virt,highmem=off \
  -cpu cortex-a72 \
  -m 1G \
  -kernel build/kernel8.img \
  -dtb qemu-rpi.dtb \
  -serial stdio \
  -nographic
```

Milestone 1.2: Kernel Core Infrastructure
Technical Requirements:
Implement interrupt vector table (VBAR_EL1)
Configure GICv2 interrupt controller
Set up exception levels and stack switching
Implement basic printk logging system
Create physical memory manager (bitmap allocator)

Architecture:

/kernel/core/
├── exceptions.S        # Vector table and handlers
├── gic.c              # Generic Interrupt Controller
├── scheduler.c        # Basic round-robin scheduler
├── kmalloc.c          # Kernel heap allocator
└── panic.c            # Kernel panic handling

Memory Management Strategy:
Physical Allocator: Simple bitmap for 4KB pages
Virtual Memory: Two-level page tables (ARMv8-A)
Kernel Heap: Slab allocator for small objects, buddy system for large

Phase 2: Device Drivers & HAL (Weeks 5-8)
Milestone 2.1: Core Peripheral Drivers
Priority Drivers:

| Driver   | Hardware          | Interface        | Complexity |
| -------- | ----------------- | ---------------- | ---------- |
| UART     | PL011             | MMIO             | Low        |
| Timer    | ARM Generic Timer | System registers | Low        |
| GPIO     | BCM2711/2712      | MMIO             | Medium     |
| EMMC2    | SD Card           | MMIO + DMA       | High       |
| USB XHCI | USB 3.0           | MMIO + DMA       | Very High  |

Driver Architecture Pattern:
```c
// HAL abstraction layer
typedef struct {
    const char *name;
    int (*init)(void *device_tree_node);
    int (*probe)(void);
    void (*remove)(void);
    // Power management hooks
    int (*suspend)(void);
    int (*resume)(void);
} driver_t;

// Device tree matching
static const dt_match_t pl011_matches[] = {
    { .compatible = "arm,pl011" },
    { .compatible = "arm,primecell" },
    { NULL }
};
```
Milestone 2.2: Framebuffer & Graphics
Technical Specifications:
Resolution: 1920x1080 @ 60Hz (primary), scalable down to 640x480
Color Depth: 32-bit ARGB8888
API: Custom compositor with DRM/KMS-inspired interface
Acceleration: V3D support (Phase 4)
Implementation:

/drivers/video/
├── fb_bcm2711.c       # VideoCore mailbox interface
├── simpledrm.c        # Simple framebuffer driver
├── compositor.c       # Window compositor
└── font.c             # Built-in bitmap font (8x16)

Phase 3: System Services & Userspace (Weeks 9-14)
Milestone 3.1: System Call Interface
Design Principles:
Follow POSIX where practical, but don't be constrained by legacy
Modern capabilities-based security model
Async I/O by default
Syscall Categories:

System Calls (128 total reserved)
├── Process Management (0-15)
│   ├── sys_spawn       # Create new process
│   ├── sys_exit        # Terminate
│   ├── sys_wait        # Wait for child
│   └── sys_sched_yield # Yield CPU
├── Memory (16-31)
│   ├── sys_mmap        # Map memory regions
│   ├── sys_munmap      # Unmap regions
│   └── sys_mprotect    # Change protection
├── Filesystem (32-63)
│   ├── sys_open        # Open file
│   ├── sys_read        # Read data
│   ├── sys_write       # Write data
│   └── sys_close       # Close descriptor
├── IPC (64-79)
│   ├── sys_channel_create # Message channel
│   ├── sys_send        # Send message
│   └── sys_recv        # Receive message
└── Device (80-95)
    ├── sys_ioctl       # Device control
    └── sys_map_device  # MMIO mapping (privileged)

Milestone 3.2: Userspace Environment
C Library (libaether):
Custom libc optimized for size and speed
musl-inspired but with modern extensions
Mandatory stack canaries and ASLR
Init System:
Simple service manager (not systemd complexity)
Declarative service configuration
Dependency-based startup
Shell (aesh - Aether Shell):
Modern scripting language
JSON-native data types
Structured piping (not just text)

Phase 4: Modern UI & Graphics (Weeks 15-20)
Milestone 4.1: Display Server & Compositor
Architecture:

┌─────────────────────────────────────┐
│         Applications                │
│  (Wayland-inspired protocol)        │
└─────────────┬───────────────────────┘
              │ aether_protocol
┌─────────────▼───────────────────────┐
│      Aether Compositor              │
│  - Scene graph management           │
│  - Damage tracking                  │
│  - Hardware cursor                  │
└─────────────┬───────────────────────┘
              │ DRM/KMS ioctls
┌─────────────▼───────────────────────┐
│      Kernel DRM Subsystem           │
│  - Mode setting                     │
│  - Buffer management (GEM)            │
│  - VSync handling                   │
└─────────────────────────────────────┘

Visual Design System:
Design Language: "Lumina" - Glassmorphism with depth
Color Palette: Dynamic theming with HSLuv color space
Typography: Inter font family, system-ui stack
Animations: 60fps minimum, spring physics-based
Compositor Features:
Hardware-accelerated transformations
Damage-based rendering (only redraw changed regions)
Multi-monitor support (hot-pluggable)
HDR support (Phase 5)
Milestone 4.2: Widget Toolkit & Applications
UI Framework (AetherUI):

```c
// Declarative UI definition
widget_t *window = window_create("Hello Aether", 800, 600);
widget_t *button = button_create("Click Me");
button_set_style(button, &(button_style_t){
    .background = color_from_hsluv(220, 90, 50),
    .corner_radius = 8,
    .shadow = shadow_create(0, 4, 12, 0.15)
});
widget_add_child(window, button);
```

Core Applications:

| Application      | Purpose              | Tech Stack                      |
| ---------------- | -------------------- | ------------------------------- |
| AetherTerm       | Terminal emulator    | GPU-accelerated text rendering  |
| Files            | File manager         | Async I/O, thumbnails           |
| Settings         | System configuration | Reactive UI, instant apply      |
| Browser (WebKit) | Web browsing         | WebKitGTK port (Phase 6)        |
| Code             | Text editor          | Tree-sitter syntax highlighting |


Phase 5: Advanced Systems (Weeks 21-28)
Milestone 5.1: Networking Stack
Implementation:
Layer 2: Ethernet driver (GENET on Pi 4, custom MAC on Pi 5)
Layer 3: IPv4/IPv6 dual-stack, custom implementation
Layer 4: TCP (NewReno congestion control), UDP
Socket API: Berkeley sockets with async extensions
Network Stack Architecture:

User Space
┌─────────────────────────────────────┐
│  Applications (Browser, SSH, etc)   │
└─────────────┬───────────────────────┘
              │ Socket API
┌─────────────▼───────────────────────┐
│      Protocol Stack                 │
│  TCP  │  UDP  │  ICMP  │  IGMP       │
└─────────────┬───────────────────────┘
              │
┌─────────────▼───────────────────────┐
│      IP Layer (v4/v6)               │
│  Routing │  Fragmentation │  NAT     │
└─────────────┬───────────────────────┘
              │
┌─────────────▼───────────────────────┐
│      Network Interface Layer        │
│  Ethernet │  ARP  │  NDP             │
└─────────────┬───────────────────────┘
              │
┌─────────────▼───────────────────────┐
│      Device Drivers                 │
│  GENET │  WiFi (brcmfmac) │  USB    │
└─────────────────────────────────────┘

Milestone 5.2: Filesystem & Storage
Supported Filesystems:
Native: AetherFS (modern copy-on-write, ZFS-inspired)
Compatibility: ext4, FAT32/exFAT, Btrfs (read-only initially)
AetherFS Features:
Copy-on-write snapshots
Transparent compression (Zstd)
Checksums for data integrity
RAID-Z style redundancy (future)

Phase 6: Hardware Acceleration & Optimization (Weeks 29-36)
Milestone 6.1: GPU Integration
VideoCore VI/V7 Integration:
Firmware Communication: VCHIQ driver for mailbox interface
3D Acceleration: V3D kernel driver, Mesa Gallium driver
Video Decode: H.264/H.265 hardware decode
Camera: CSI interface support
Graphics Stack:

┌─────────────────────────────────────┐
│      OpenGL ES 3.1 / Vulkan         │
│      (Mesa with V3D driver)         │
└─────────────┬───────────────────────┘
              │
┌─────────────▼───────────────────────┐
│      Kernel DRM (V3D driver)        │
│  - GPU scheduler                    │
│  - Memory management (BOs)            │
│  - Command stream validation          │
└─────────────┬───────────────────────┘
              │
┌─────────────▼───────────────────────┐
│      V3D Hardware                   │
│  - Tile-based deferred rendering    │
│  - 4 QPUs (Quad Processor Units)    │
└─────────────────────────────────────┘

Milestone 6.2: Power Management
CPU frequency scaling (ondemand governor)
USB autosuspend
Display power management (DPMS)
Thermal throttling protection

Development Workflow & Tooling
Build System Architecture

```cmake
# Top-level CMakeLists.txt structure
project(aether_os C ASM)

# Kernel build
add_subdirectory(kernel)

# Userspace libraries
add_subdirectory(libs/libaether)      # libc
add_subdirectory(libs/libui)          # UI toolkit
add_subdirectory(libs/libgfx)         # Graphics

# Applications
add_subdirectory(apps/terminal)
add_subdirectory(apps/files)
add_subdirectory(apps/settings)

# Generate bootable image
add_custom_target(image
    COMMAND ${CMAKE_SOURCE_DIR}/scripts/mkimage.sh
    DEPENDS kernel apps
)
```

QEMU Development Environment
Full System Emulation:

```bash
#!/bin/bash
# scripts/run_qemu.sh

qemu-system-aarch64 \
    -M virt,highmem=off \
    -cpu cortex-a72 \
    -smp 4 \
    -m 2G \
    -kernel build/kernel8.img \
    -dtb build/qemu-rpi4.dtb \
    -drive file=build/sdcard.img,format=raw,if=sd \
    -netdev user,id=net0,hostfwd=tcp::2222-:22 \
    -device virtio-net-device,netdev=net0 \
    -device virtio-gpu-pci \
    -serial stdio \
    -monitor telnet::45454,server,nowait \
    -s -S  # GDB server, wait for connection
```

Debugging Setup:

```gdb
# .gdbinit
set architecture aarch64
target remote localhost:1234
symbol-file build/kernel8.elf
break kernel_main
continue
```

Testing Strategy

| Test Level  | Tool           | Coverage                           |
| ----------- | -------------- | ---------------------------------- |
| Unit        | CMocka         | Kernel algorithms, data structures |
| Integration | Custom harness | Driver initialization, syscalls    |
| System      | QEMU + scripts | Full boot, application launch      |
| Hardware    | Real Pi        | Performance, timing, edge cases    |


Project Structure

aether_os/
├── docs/                      # Architecture documentation
├── kernel/
│   ├── arch/arm64/           # Architecture-specific
│   │   ├── boot/
│   │   ├── mm/
│   │   └── include/
│   ├── core/                  # Core kernel (portable)
│   ├── drivers/
│   │   ├── char/
│   │   ├── block/
│   │   ├── net/
│   │   └── video/
│   ├── fs/                    # Filesystems
│   ├── net/                   # Network stack
│   └── include/
├── libs/
│   ├── libaether/            # Standard C library
│   ├── libui/                # UI toolkit
│   └── libgfx/               # Graphics utilities
├── apps/
│   ├── terminal/
│   ├── files/
│   ├── settings/
│   └── shell/
├── scripts/                   # Build and test scripts
├── configs/                   # Device tree, kernel configs
└── tools/                     # Host utilities

Technical Standards & Best Practices
Code Standards
Language: C17 for kernel, C++20 for userspace (selected components)
Formatting: clang-format with custom kernel style
Static Analysis: Clang Static Analyzer, Coverity
Documentation: Doxygen for APIs, Markdown for design docs

Git Workflow

```plain
main (stable, tagged releases)
├── develop (integration branch)
│   ├── feature/memory-management
│   ├── feature/usb-drivers
│   └── feature/ui-compositor
├── release/v0.1.0
└── hotfix/critical-bug
```

Risk Mitigation & Contingencies
| Risk                        | Mitigation Strategy                              |
| --------------------------- | ------------------------------------------------ |
| Hardware complexity         | Extensive QEMU testing before Pi deployment      |
| Driver development time     | Prioritize virtio drivers for QEMU, adapt for Pi |
| Performance issues          | Early benchmarking, profiling infrastructure     |
| Scope creep                 | Strict milestone definitions, MVP-first approach |
| Single developer bottleneck | Modular design, comprehensive documentation      |

Success Metrics

| Phase | Completion Criteria                             |
| ----- | ----------------------------------------------- |
| 1     | Successful boot to shell on both QEMU and Pi    |
| 2     | All core peripherals functional, serial console |
| 3     | Multi-process userspace, basic shell            |
| 4     | Graphical desktop, window manager, 3 apps       |
| 5     | Network connectivity, web browser functional    |
| 6     | 3D acceleration, video playback, <2s boot time  |


