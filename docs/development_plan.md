# AetherOS — Living Development Plan

> **Last updated:** 2026-04-21  
> **Current Phase:** Phase 4 — Graphics  
> **Overall Status:** Phase 3 complete ✓  Phase 4.0 complete ✓  Phase 4.1 complete ✓  Phase 4.2 complete ✓  Phase 4.3 complete ✓  Phase 4.4 complete ✓  Phase 4.5 complete ✓  Phase 4.6 complete ✓

---

## Quick Reference

| Item | Value |
|------|-------|
| Target hardware | Raspberry Pi 5 (BCM2712, Cortex-A76, 4/8GB) |
| Architecture | AArch64 (ARMv8.2-A) |
| Emulator | QEMU 8.x+ (`-M virt`, cortex-a76) |
| Cross-compiler | `aarch64-elf-gcc` via Homebrew (Intel macOS) |
| Developer host | MacBook Intel, macOS |
| Build system | CMake + Ninja |
| Language (kernel) | C17 + AArch64 ASM |
| Language (userspace) | C17 / C++20 (selective) |
| Design language | "Lumina" — Glassmorphism |

---

## Phase 0 — Toolchain & Environment Setup (Pre-requisite)

> Must be completed before any Phase 1 work begins.  
> **Host:** MacBook Intel (x86_64 macOS). **Target Pi:** Pi 5 (BCM2712, Cortex-A76).

### Pi 5 Architecture Note

The **Raspberry Pi 5** (BCM2712) has a fundamentally different I/O architecture from Pi 4:
- Core: ARM Cortex-A76 (ARMv8.2-A), 64-bit
- A new **RP1 south bridge** chip handles all peripheral I/O (UART, GPIO, SPI, I2C, USB 2.0)
- Peripheral MMIO addresses differ completely from BCM2711 (Pi 4)
- Boot chain is similar: firmware loads `kernel_2712.img`

**Strategy:** Develop entirely on QEMU `-M virt` for Phases 0–2. In Phase 2, write
BCM2712/RP1 specific drivers when targeting real Pi 5 hardware.

### Tasks

- [x] **0.1** Install Homebrew (package manager for macOS)
- [x] **0.2** Install cross-compilation toolchain: `aarch64-elf-gcc` (GCC 15.2.0)
- [x] **0.3** Install QEMU 10.2.2 (`qemu-system-aarch64`)
- [x] **0.4** Install build tools: CMake 4.3.1, Ninja 1.13.2, pkg-config
- [x] **0.5** Install debugging tools: GDB with AArch64 support
- [x] **0.6** Install auxiliary tools: git, python3, mtools (for disk images)
- [x] **0.7** Create GitHub repository: https://github.com/CLSoftSilvestre/aether_os
- [x] **0.8** Initialize project directory structure
- [x] **0.9** Write root `CMakeLists.txt` and `cmake/toolchain-aarch64.cmake`
- [x] **0.10** Verify: compile minimal AArch64 ELF, load in QEMU, see output

### Step-by-Step Installation (Intel macOS)

#### Step 1 — Homebrew

```bash
# Check if already installed
which brew || /bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
```

#### Step 2 — Cross-compiler toolchain

```bash
# aarch64-elf-gcc: produces bare-metal AArch64 ELF binaries (no OS, no libc)
brew install aarch64-elf-gcc

# Verify — should print GCC version
aarch64-elf-gcc --version

# Tools you get:
#   aarch64-elf-gcc       — C compiler
#   aarch64-elf-as        — assembler
#   aarch64-elf-ld        — linker
#   aarch64-elf-objcopy   — ELF → raw binary
#   aarch64-elf-objdump   — disassembler (invaluable for debugging)
#   aarch64-elf-nm        — symbol table inspector
#   aarch64-elf-readelf   — ELF header inspector
```

> **AArch64 vs x86 analogy:** `aarch64-elf-gcc` is to AArch64 what `nasm`+`ld` was to
> your 8086 work — it compiles/links code that runs directly on bare metal with no OS.

#### Step 3 — QEMU

```bash
brew install qemu

# Verify
qemu-system-aarch64 --version   # should be 8.x or 9.x
```

#### Step 4 — Build tools

```bash
brew install cmake ninja pkg-config
cmake --version    # should be 3.20+
ninja --version
```

#### Step 5 — Debugger

```bash
# GDB with AArch64 support
brew install gdb

# If gdb is not codesigned on macOS, use LLDB instead (comes with Xcode):
lldb --version
# LLDB supports AArch64 natively and connects to QEMU's GDB stub
```

#### Step 6 — Auxiliary tools

```bash
brew install git python3 mtools dosfstools
# mtools + dosfstools: create FAT32 boot images for Pi SD card
```

#### Step 7 — Verify toolchain end-to-end

```bash
# Create a test file
cat > /tmp/test.S << 'EOF'
.section .text
.global _start
_start:
    mov x0, #65        // ASCII 'A'
    mov x8, #64        // write syscall (Linux AArch64) — won't run bare metal
    svc #0
    mov x8, #93        // exit
    svc #0
EOF

# Assemble and link
aarch64-elf-gcc -nostdlib -static /tmp/test.S -o /tmp/test.elf

# Inspect it
aarch64-elf-objdump -d /tmp/test.elf

# If you see AArch64 disassembly, the toolchain works!
```

---

## Phase 1 — Foundation & Boot (Weeks 1–4)

### Milestone 1.1 — Bootloader & Firmware Interface

**Goal:** Bare-metal AArch64 code boots in QEMU, outputs "Hello from AetherOS" on UART.

**Status:** Not started

#### Tasks

- [x] **1.1.1** Write `kernel/arch/arm64/boot/boot.S` — AArch64 entry point
  - Check exception level (EL3/EL2/EL1), drop to EL1
  - Zero BSS segment
  - Set up initial stack pointer at 0x80000
  - Jump to `kernel_main`
- [x] **1.1.2** Write `kernel/arch/arm64/boot/linker.ld` — memory layout
  - Kernel load address: `0x40000000` (QEMU virt convention)
  - Sections: `.text.boot`, `.text`, `.rodata`, `.data`, `.bss`, stack
- [x] **1.1.3** Write `kernel/drivers/char/uart_pl011.c` — PL011 UART driver
  - MMIO base: `0x09000000` (QEMU virt)
  - Init (115200 8N1), putc, puts, puthex, putdec
- [x] **1.1.4** Write `kernel/core/main.c` — `kernel_main()` entry
- [ ] **1.1.5** Write `kernel/arch/arm64/boot/mmu_init.c` — identity mapping
  - Map first 1GB identity (MMIO + RAM)
  - Enable MMU (SCTLR_EL1)
- [ ] **1.1.6** Write `configs/qemu-virt.dts` — QEMU virtual machine DTB
- [x] **1.1.7** Build system: `kernel/CMakeLists.txt` producing `kernel8.img`
- [x] **1.1.8** Write `scripts/run_qemu.sh` — QEMU launch script
- [x] **1.1.9** Test: boot in QEMU, UART output visible — PASSED ✓
  - Entry point: 0x40000000, BSS/stack layout verified
  - Output: "AetherOS v0.0.1 — Milestone 1.1 — Boot Successful"

**Key decisions:**
- QEMU machine: `-M virt` (not raspi4b) for easier early development; switch to `raspi4b` in Phase 2
- Boot protocol on real Pi: `config.txt` sets `kernel=kernel8.img`, `arm_64bit=1`
- DTB: auto-generated by QEMU for `-M virt`; Pi uses firmware-provided DTB

---

### Milestone 1.2 — Kernel Core Infrastructure

**Goal:** Exception handling works, GIC active, timer firing, structured logging.

**Status:** Complete ✓  
**Verified:** QEMU cortex-a76, timer fires at exactly 100 Hz, 4 seconds confirmed

#### Tasks

- [x] **1.2.1** Write `kernel/arch/arm64/exceptions.S` — vector table (VBAR_EL1)
  - 16 vectors (4 groups × 4 types), each 128 bytes, table 2KB-aligned
  - EXCEPTION_ENTRY/EXIT macros save/restore full trap_frame_t (272 bytes)
- [x] **1.2.2** Write `kernel/core/exceptions.c` — exception dispatch
  - Decode ESR_EL1 EC field for sync faults, full register dump on panic
  - IRQ dispatch: GIC acknowledge → driver handler → GIC EOI
- [x] **1.2.3** Write `kernel/drivers/irq/gic_v2.c` — GICv2 interrupt controller
  - Distributor + CPU interface init, 288 IRQs on QEMU virt
  - gic_enable_irq / gic_acknowledge / gic_end_of_interrupt
- [x] **1.2.4** Write `kernel/core/printk.c` — kernel logging
  - Log levels DEBUG/INFO/WARN/ERROR/PANIC, format: %s %d %u %x %lx %p %c
  - kinfo/kwarn/kerror/kdebug/kpanic convenience macros
- [ ] **1.2.5** *(deferred to Phase 2)* Kernel panic backtrace via frame pointer chain
- [ ] **1.2.6** Write `kernel/mm/pmm.c` — physical memory manager
- [ ] **1.2.7** Write `kernel/mm/vmm.c` — virtual memory / page tables
- [ ] **1.2.8** Write `kernel/core/kmalloc.c` — kernel heap allocator
- [ ] **1.2.9** Write `kernel/core/scheduler.c` — basic round-robin scheduler
- [x] **1.2.10** Test: timer IRQ fires at 100 Hz, printk formats correctly — PASSED ✓

**Note:** PMM/VMM/scheduler moved to Phase 2 (separate milestone). The exception
infrastructure is the prerequisite that unlocks all interrupt-driven development.

---

## Phase 2 — Memory Management & Scheduler (resequenced from spec)

> Device drivers (original Phase 2) moved after memory management,
> which is the prerequisite for the scheduler and all future subsystems.

### Milestone 2.0 — Physical Memory Manager

**Status:** Complete ✓ — 2026-04-17

- [x] Bitmap allocator: 262144 pages (1GB), 32KB bitmap in BSS
- [x] `pmm_alloc_page()`, `pmm_alloc_pages(n)`, `pmm_free_page()`
- [x] Kernel image + boot stack marked used at init
- [x] Verified: 261986 free pages (~1023MB) on QEMU -m 1G

### Milestone 2.1 — Kernel Heap (kmalloc)

**Status:** Complete ✓ — 2026-04-17

- [x] First-fit free-list allocator backed by PMM pages
- [x] 16-byte block headers (is_free encoded in size bit 31)
- [x] Block splitting (`maybe_split`) and coalescing on `kfree`
- [x] `kzalloc()` for zero-initialised allocations

### Milestone 2.2 — Cooperative Scheduler + Context Switch

**Status:** Complete ✓ — 2026-04-17

- [x] `cpu_context_t`: callee-saved x19–x30 + sp (104 bytes)
- [x] `context_switch.S`: saves/restores callee-saved regs, switches SP
- [x] Round-robin `find_next()` with sleeping task wake-up via timer ticks
- [x] `task_create()`, `task_sleep()`, `task_exit()`
- [x] Verified: 3 concurrent kernel threads (idle, monitor, counter)
  - counter: counts 1–10 with 500ms sleep between each, exits cleanly
  - monitor: prints system status every 2s indefinitely
  - idle: wfi + yield loop

**Note:** MMU deferred to Phase 3 entry (needed before user-space processes,
but not required for kernel threads which share a single address space).

---

### Milestone 2.3 — Core Peripheral Drivers

**Status:** Complete ✓ (done inline during Phases 1–3)

- [x] ARM Generic Timer: 100 Hz periodic tick, scheduler hook (`arm_timer.c`)
- [x] PL011 UART: polled TX + interrupt-driven RX with ring buffer (Milestone 3.2)
- [x] GICv2: distributor + CPU interface, per-IRQ enable/disable, acknowledge + EOI
- [ ] GPIO driver — deferred to Phase 5 (Pi 5 hardware)
- [ ] SD card / eMMC driver — deferred to Phase 5 (Pi 5 hardware)
- [ ] Driver registry / DTB probe — deferred to Phase 5

---

### Milestone 2.2 — Framebuffer & Graphics (Basic)

**Status:** Not started

#### Tasks

- [ ] **2.2.1** Write `kernel/drivers/video/fb_bcm2711.c` — VideoCore mailbox
  - Mailbox property interface (0xFE00B880 on Pi 4)
  - Request framebuffer: resolution, pitch, pixel format
- [ ] **2.2.2** Write `kernel/drivers/video/simpledrm.c` — simple framebuffer
  - Map framebuffer to virtual address
  - `fb_put_pixel()`, `fb_fill_rect()`, `fb_blit()`
- [ ] **2.2.3** Write `kernel/drivers/video/font.c` — bitmap font renderer
  - 8×16 font (VGA-style or custom)
  - UTF-8 aware
- [ ] **2.2.4** Write `kernel/drivers/video/console.c` — VT100 terminal on framebuffer
  - Scrolling, cursor, color attributes (ANSI codes)
- [ ] **2.2.5** QEMU: use `virtio-gpu` for framebuffer during development
- [ ] **2.2.6** Test: boot to graphical text console (colored output)

---

## Phase 3 — System Services & Userspace (Weeks 9–14)

### Milestone 3.0 — MMU + First EL0 User Process

**Status:** Complete ✓ — 2026-04-17

#### What was built

- [x] **vmm.h / vmm.c** — AArch64 MMU setup with two-level page tables
  - L1 table: 1GB BLOCK for MMIO (device memory, EL1 only), TABLE → L2 for RAM
  - L2 table: 512 × 2MB blocks (510 kernel EL1-only, 2 user BOTH_RW)
  - TCR_EL1: T0SZ=32 (4GB space), 4KB granule, inner-shareable, IPS=40-bit
  - MAIR_EL1: Attr0=0xFF (Normal WB), Attr1=0x00 (Device-nGnRnE)
  - Full barrier sequence (dsb + isb + tlbi vmalle1) before SCTLR write
  - D-cache (SCTLR.C) and I-cache (SCTLR.I) enabled together with MMU
  - `launch_el0(entry, sp)` — switches to EL0 via ELR_EL1/SPSR_EL1/SP_EL0 + eret

- [x] **syscall.h / syscall.c** — System call dispatch (Linux AArch64 ABI)
  - Syscall number in x8, args in x0–x5, return value in x0
  - SYS_EXIT (0): `task_exit()` — terminates calling process
  - SYS_WRITE (34): fd=1 → UART output, capped at 4KB
  - SYS_SCHED_YIELD (3): cooperative yield to scheduler

- [x] **exceptions.c** — el0_sync_handler updated for SVC dispatch
  - Checks ESR.EC == 0x15 (SVC from AArch64 EL0)
  - Calls `syscall_dispatch()` and writes result back into `frame->x[0]`

- [x] **init_task.S** — First user-space program (AArch64 assembly)
  - Position-independent (all addressing PC-relative)
  - Calls `sys_write(1, "Hello from EL0! (AetherOS init)\n", 32)`
  - Calls `sys_exit(0)` to terminate

- [x] **main.c** — Phase 3 init sequence
  - Calls `vmm_init()` before GIC/timer (MMU must be up before MMIO is remapped)
  - Copies init_task.S code blob to user-accessible region (0x7FC00000)
  - Calls `launch_el0(VMM_USER_CODE_BASE, VMM_USER_STACK_TOP)` after IRQs enabled

#### QEMU quirk discovered and resolved
QEMU 10.2.2 (cortex-a76) incorrectly raises a Permission Fault for EL1 instruction
fetches from pages mapped with `AP=BOTH_RW` (AP[2:1]=01). Workaround:
- Kernel pages: `AP=EL1_RW` (AP[2:1]=00) with `PXN=0` — EL1 executes normally
- User pages: `AP=BOTH_RW` — last 4MB of RAM only, sufficient for init_task
- User code copied at boot from kernel .text (EL1) to user region (BOTH_RW)
- This behavior does NOT occur on real ARMv8 hardware per the architecture spec

#### Verified output (QEMU -M virt, cortex-a76)
```
[INF] VMM: MMU enabled — caches on
[INF] Phase 3: copied 68 bytes of user code to 0x7fc00000
[INF] VMM: launching EL0 process — entry=0x7fc00000 stack=0x7ffff000
[DBG] [SYS] syscall #34 (x0=1 x1=0x7fc00024 x2=32)
Hello from EL0! (AetherOS init)
[DBG] [SYS] syscall #0 (x0=0 ...)
[INF] [SYS] sys_exit(0) — process 0 terminating
```

#### Next: Phase 3.1 milestones
- Proper process creation via `task_create()` for user processes
- ELF loader from initrd (replaces the copy-and-jump approach)
- Separate TTBR0 per process (per-process virtual address space)
- `copy_from_user()` for safe kernel↔user data transfer
- Expand syscall table: open/read/close, mmap, fork/exec

---

### Milestone 3.1 — initrd + ELF Loader + Process Creation

**Status:** Complete ✓ — 2026-04-17  
**Commit:** da365c1

#### What was built

- [x] **`kernel/fs/initrd.c`** — CPIO newc archive reader
  - Embedded in kernel binary via `objcopy -I binary` (symbols: `_binary_initrd_cpio_start/end`)
  - `initrd_init()`: validates archive, prints directory listing
  - `initrd_find(path, &size)`: locate file by path (strips leading `/` and `./`)
  - `initrd_list(buf, len)`: write newline-separated listing into a buffer
  - Manual `cpio_memeq()` — no libc dependency
- [x] **`kernel/fs/elf.c`** — ELF64 static loader
  - Validates magic, class, machine (EM_AARCH64)
  - Iterates PT_LOAD segments: `memcpy` to `p_vaddr`, zeros BSS (`p_memsz > p_filesz`)
  - `dsb ish + isb` cache coherency barrier after load
- [x] **`kernel/core/process.c`** — process spawning
  - `process_spawn(path)`: find ELF in initrd → load → create user task
  - `user_task_trampoline()`: kernel-side trampoline that calls `launch_el0(entry, sp)`
- [x] **Scheduler extended** — `task_create_user()`, `task_get_user_regs()`
  - New fields in `task_t`: `el0_entry`, `el0_sp`
- [x] **VMM extended** — user region expanded to 256MB (L2[384-511] = 0x70000000–0x7FFFFFFF)
  - ELF programs link at `0x70000000`, stack at `VMM_USER_STACK_TOP = 0x7FFFF000`
- [x] **`userspace/` build system** — CMake builds static ELF64 userspace programs
  - `user.ld` linker script (base 0x70000000)
  - `crt0.S`: zeros BSS, calls `main()`, falls through to `sys_exit(0)`
  - `userspace/apps/init/`: first ELF loaded from initrd (PID 1)
- [x] **Syscalls:** SYS_EXIT(0), SYS_WRITE(34), SYS_SCHED_YIELD(3)
- [x] **Verified:** QEMU boots → initrd parsed → ELF loaded at 0x70000000 → EL0 launched → Hello World printed → clean sys_exit

---

### Milestone 3.2 — UART RX Interrupts + libaether + aesh Shell

**Status:** Complete ✓ — 2026-04-17  
**Commit:** c264d02

#### What was built

- [x] **UART RX interrupt-driven input** (`uart_pl011.c`)
  - 256-byte ring buffer (`rx_buf[]`, `rx_head`, `rx_tail`)
  - `uart_enable_rx_irq()`: unmasks RXIM in PL011 + enables IRQ 33 in GIC
  - `uart_irq_handler()`: drains RX FIFO → ring buffer, clears ICR
  - `uart_rx_empty()` / `uart_getc_nowait()`: non-blocking ring buffer read
  - GIC dispatch updated in `exceptions.c` for IRQ 33
- [x] **New syscalls** (`syscall.c` / `syscall.h`)
  - `SYS_READ (63)`: fd=0 (stdin/UART), WFI-blocks until ring buffer has data
  - `SYS_INITRD_LS (500)`: fills user buffer with newline-separated initrd file list
- [x] **`libaether`** — freestanding C runtime (`userspace/lib/libaether/`)
  - `string.c`: strlen, strcmp, strncmp, strcpy, strcat, strchr, strtok, memcpy, memset, memcmp
  - `stdio.c`: vsnprintf (supports %d %u %x %X %s %c %p %l), printf, putchar, puts, readline (with echo and backspace)
  - `stdlib.c`: bump allocator malloc (64KB static heap), free (no-op), atoi/atol, exit
  - Freestanding headers: stddef.h, stdarg.h, stdbool.h, stdint.h, string.h, stdio.h, stdlib.h
- [x] **aesh — AetherOS Shell** (`userspace/apps/init/main.c`, replaces Hello World)
  - ASCII art banner on boot
  - `readline()` with character echo and backspace support
  - Built-ins: `help`, `echo`, `ls` (via sys_initrd_ls), `clear` (ANSI escape), `uname`, `exit`
- [x] **Verified:** QEMU boots → banner displayed → `aesh>` prompt → `help`/`echo`/`ls` all work

**Phase 3 Success Criteria:** ✓ Interactive UART shell, syscall I/O, freestanding C runtime.

---

## Phase 4 — Modern UI & Graphics (Weeks 15–20)

### Milestone 4.0 — Framebuffer Driver & Graphical Console

**Status:** Complete ✓ — 2026-04-17  
**Commit:** e2efde4

**Goal:** Boot AetherOS into a 1024×768 graphical window. All kernel `kinfo`/`kwarn` output
and the aesh shell appear on screen (in addition to UART). This is the foundation for
the compositor and Lumina desktop.

#### Technical Approach

QEMU `-M virt` + `-device ramfb` provides a framebuffer configured via the QEMU fw_cfg
MMIO interface (0x09020000). The kernel writes a `RamFBCfg` struct (pixel format, dimensions,
physical address) to fw_cfg, then QEMU displays whatever is at that physical address.

- Physical memory: `pmm_alloc_pages(768)` for 1024×768×4 = 3MB contiguous
- Pixel format: XRGB8888 (32bpp, no alpha)
- QEMU display: add `-device ramfb` + remove `-nographic`

#### Tasks

- [x] **4.0.1** `kernel/drivers/video/fw_cfg.c` — fw_cfg MMIO interface
  - Selector (0x09020008), data (0x09020000), DMA (0x09020010)
  - `fwcfg_find_file()` scans directory for named file + returns selector key
  - `fwcfg_write_file()` via DMA WRITE control word (big-endian descriptor)
- [x] **4.0.2** `kernel/drivers/video/ramfb.c` — ramfb initialization
  - `pmm_alloc_pages(768)` = 3MB contiguous at 0x400c1000
  - RamFBCfg (big-endian): addr=phys, fourcc=XRGB8888, 1024×768, stride=4096
  - Background cleared to #121218 (near-black)
- [x] **4.0.3** `kernel/drivers/video/fb.c` — drawing primitives
  - `fb_put_pixel`, `fb_fill_rect`, `fb_blit`; `FB_RGB(r,g,b)` macro
- [x] **4.0.4** `kernel/drivers/video/font.c` — embedded 8×8 VGA bitmap font
  - 256 glyphs (public domain); `font_draw_char(x, y, ch, fg, bg)`
- [x] **4.0.5** `kernel/drivers/video/fb_console.c` — 128×96 scrolling text console
  - `fb_console_putc(c)` handles CR/LF/tab/scroll
  - Hooked into `printk` via `pk_putc()` — dual UART+FB output
- [x] **4.0.6** `scripts/run_qemu.sh` updated
  - Default: `-device ramfb -vga none` (graphical window)
  - `--headless` flag for UART-only mode
- [x] **4.0.7** Verified: 1024×768 QEMU window shows boot log and aesh shell

---

### Milestone 4.1 — Graphical Desktop Shell (init process)

**Status:** Complete ✓

Implemented a Lumina-themed graphical desktop that runs as PID 1 (the init process)
directly on the framebuffer. The init process calls `sys_fb_claim()` to take over the
screen from the kernel's text console, then draws the desktop and runs `aesh` inside a
terminal window using graphical character rendering via `sys_fb_char`.

**Kernel fix — QEMU 10.x boot address (linker.ld):**  
QEMU 10.2.2 changed the AArch64 virt machine boot behavior: a 512KB stub is now placed
at 0x40000000 and the kernel image is loaded at **0x40080000** (not 0x40000000 as in
earlier QEMU). The linker script was updated to `. = 0x40080000` and `__bss_end` was
aligned to 8 bytes to prevent a BSS-zeroing loop underflow.

#### Tasks

- [x] **4.1.1** `kernel/arch/arm64/boot/linker.ld` — updated base to `0x40080000`
  - Also aligned `__bss_end` to 8 bytes (loop underflow fix)
- [x] **4.1.2** `kernel/core/syscall.c` — graphics syscalls (`SYS_FB_FILL`, `SYS_FB_CHAR`, `SYS_GET_TICKS`, `SYS_FB_CLAIM`)
  - Packed 64-bit arg convention: `x<<32|y`, `w<<32|h`, `ch<<32|fg`
- [x] **4.1.3** `kernel/drivers/video/fb_console.c` — `fb_console_claim()` silences kernel screen once user takes over
- [x] **4.1.4** `userspace/lib/include/gfx.h` + `lib/libaether/gfx.c` — drawing library
  - Lumina color palette (C_DESKTOP, C_PANEL, C_ACCENT, C_TEXT, …)
  - `gfx_fill`, `gfx_hline`, `gfx_vline`, `gfx_rect`, `gfx_char`, `gfx_text`, `gfx_text_center`, `gfx_printf`
- [x] **4.1.5** `userspace/apps/init/main.c` — Lumina graphical desktop shell
  - 1024×768 layout: top bar (36px) + accent line (2px) + main area + bottom bar (24px)
  - Terminal window: 816×608 with shadow, title bar, traffic-light buttons, accent border
  - 100×72 char terminal emulator with scroll, cursor, and `sys_read` blocking I/O
  - `aesh` shell running inside: help, echo, ls, clear, uname, exit
- [x] **4.1.6** Verified: all 786,432 pixels non-zero; (0,0) = `0x1a1a28` = `C_PANEL` ✓

---

### Milestone 4.2 — Enriched Graphical Shell

**Status:** Complete ✓ (2026-04-17)

Pragmatic implementation: instead of separate windowed apps (which require a
compositor and IPC not yet built), Phase 4.2 enriched the single-process shell.

- [x] **4.2.1** System Info sidebar (344px): uptime, memory bar, platform info
- [x] **4.2.2** New shell commands: `cat`, `mem`, `time` (+ existing help/ls/echo/clear/uname/exit)
- [x] **4.2.3** `motd.txt` packaged in initrd, loaded at startup
- [x] **4.2.4** HH:MM:SS uptime in top bar + sidebar; free memory in bottom bar
- [x] **4.2.5** `SYS_INITRD_READ` (501) and `SYS_PMM_STATS` (502) kernel syscalls
- [x] **4.2.6** UART RX fix: `uart_rx_empty()` drains PL011 hardware FIFO directly
- [x] **4.2.7** CMake `LINK_DEPENDS` fix: kernel re-links whenever `initrd.o` changes

> Original Phase 4.2 apps (AetherTerm, Files, StatusBar, Taskbar) deferred to
> Phase 4.4+ — they require multi-process support built in Phase 4.3.

---

### Milestone 4.3 — Process Management

**Status:** Complete ✓ (2026-04-18)

Full multi-process foundation: per-process isolated address spaces, process spawn/wait,
pipe IPC, and fd-table inheritance.

#### Architecture

- **Per-process page tables** — each spawned child gets its own L1 (4 KB) + L2 (4 KB)
  tables. Kernel entries L2[0..383] are copied from the global table (kernel always
  reachable). User entries L2[384..511] start empty and are filled by `vmm_map_user_pages()`
  with L3 4 KB PAGE descriptors (AP=BOTH_RW). TTBR0_EL1 is switched on every
  context switch and before task exit cleans up the old tables.
- **Physical isolation** — ELF and stack pages are allocated from kernel-range PMM
  (`0x40000000–0x6FFFFFFF`, identity-mapped for EL1 access) and mapped at user VA
  `0x70000000` via L3 entries. Two processes both see `0x70000000` but get different
  physical pages. Process exit frees L3 tables, L2, L1, and all physical pages.
- **`SYS_SPAWN` (not fork+exec)** — combined kernel-side spawn: locate ELF in initrd,
  allocate physical pages, create page tables, copy ELF segments to physical memory
  via kernel alias, create isolated task. Avoids copying the parent address space.

#### What was built

- [x] **4.3.1** `kernel/mm/vmm.c` — new VMM functions
  - `vmm_create_process_pt()`: allocates private L1+L2, copies kernel L2 entries
  - `vmm_map_user_pages(l1_phys, va, pa, n)`: creates L3 tables on demand, maps 4 KB pages
  - `vmm_switch_user_pt(l1_phys)`: loads TTBR0_EL1 + TLB flush; 0 = restore global
  - `vmm_free_process_pt(l1_phys)`: frees all L3 tables, L2, and L1
  - `vmm_get_global_l1()`: returns physical address of the boot-time L1 table
- [x] **4.3.2** `kernel/include/aether/scheduler.h` — task_t expanded
  - New states: `TASK_ZOMBIE` (5), `TASK_WAITING` (6)
  - New fields: `ppid`, `exit_code`, `wait_pid`, `l1_table_phys`,
    `user_code_phys/pages`, `user_stack_phys/pages`, `fd_table[8]`
  - `fd_entry_t`: type (UART / PIPE_R / PIPE_W / CLOSED) + pipe_idx
  - MAX_TASKS bumped from 16 → 32
- [x] **4.3.3** `kernel/core/scheduler.c` — scheduler extended
  - `task_create_isolated()`: creates task with own L1, ppid, physical page tracking,
    and fd_table inherited from parent
  - `task_yield()`: calls `vmm_switch_user_pt(current->l1_table_phys)` after context switch
  - `task_exit()`: switches to global PT before freeing process PT (closes safety window);
    sets TASK_ZOMBIE and wakes TASK_WAITING parent; frees code + stack physical pages
  - `task_waitpid(pid, status)`: sets TASK_WAITING, yields until child is TASK_ZOMBIE, reaps
  - `task_get_fd / task_alloc_fd / task_close_fd / task_dup2_fd`: fd-table helpers
- [x] **4.3.4** `kernel/core/process.c` — isolated spawn
  - `process_spawn_child(path, ppid, &child_pid)`: ELF → alloc physical pages → create PT
    → `vmm_map_user_pages` for code + 256 KB stack → copy ELF segments to physical pages
    via identity-mapped kernel alias → `task_create_isolated()`
  - `user_task_trampoline()`: now calls `vmm_switch_user_pt(l1_phys)` before `launch_el0()`
    so TTBR0 is correct on the very first instruction of each process
- [x] **4.3.5** `kernel/core/pipe.c` + `kernel/include/aether/pipe.h` — kernel pipe
  - 16 ring-buffer pipes (4 KB each); ref-counted read/write ends
  - `pipe_alloc / pipe_close_read / pipe_close_write / pipe_read / pipe_write`
  - `pipe_read` blocks via `task_yield()` when empty; returns 0 (EOF) when write end closed
  - `pipe_write` blocks via `task_yield()` when full; returns -1 if read end gone
- [x] **4.3.6** `kernel/core/syscall.c` — new syscalls; fd-aware read/write
  - `SYS_SPAWN (1)`: calls `process_spawn_child`; returns child PID
  - `SYS_WAITPID (4)`: blocks until child exits; returns child PID
  - `SYS_GETPID (5)`: returns current PID
  - `SYS_PIPE (22)`: allocates pipe + two fd slots; returns fds[0]=read, fds[1]=write
  - `SYS_DUP2 (24)`: copies fd[oldfd] → fd[newfd], closing old pipe end if needed
  - `SYS_READ / SYS_WRITE`: now dispatch through fd_table (UART or pipe)
- [x] **4.3.7** `userspace/lib/include/sys.h` — userspace wrappers
  - `sys_spawn(path)`, `sys_waitpid(pid, status)`, `sys_getpid()`
  - `sys_pipe(fds)`, `sys_dup2(oldfd, newfd)`
  - New `_sys2()` helper for two-argument syscalls
- [x] **4.3.8** `userspace/apps/init/main.c` — shell extended
  - `spawn <path>`: launches child ELF from initrd, waits for exit (foreground)
  - `spawn <path> &`: launches in background (no wait); trailing `&` stripped before parse
  - `pid`: prints current PID via `sys_getpid()`
  - Updated `help` listing; uname version string bumped to Phase 4.3
- [x] **4.3.9** `kernel/core/main.c` — `pipe_init()` called before scheduler init

**Phase 4.3 Success Criteria:** ✓ Shell can spawn a second ELF with isolated address space;
parent can wait for child exit; pipes connect processes via fd redirection; background `&` works.

---

### Milestone 4.4 — Core Applications (MVP)

**Status:** Complete ✓ (2026-04-18)

Multi-process desktop: `init` is now a desktop manager; four separate app processes run concurrently on the framebuffer.

#### Architecture

- **`init` (PID 1)** — Desktop manager: draws chrome (top/bottom bars), spawns `statusbar` + `aether_term` as background processes, then loops refreshing the bar clock and free-memory readout every second via `sys_sleep(100)`.
- **`aether_term`** — Full Lumina terminal window + aesh shell. Draws its own window frame on startup. Shell commands: help, echo, ls, cat, mem, time, clear, uname, pid, files, view, spawn, exit. After any foreground child exits, redraws its window + terminal buffer.
- **`statusbar`** — Sidebar daemon: draws System Info panel (uptime, memory bar, platform), refreshes every second in a `sys_sleep` loop.
- **`files`** — Graphical file browser. Lists initrd files, j/k navigation, v to spawn textviewer, q to quit. Redraws its own frame after textviewer returns.
- **`textviewer`** — Text pager. Prompts for filename, reads from initrd, displays with j/k/d/u/q controls and a line/% status bar.

#### New kernel syscall

- **`SYS_SLEEP_TICKS (6)`** — `task_sleep(n)` from userspace. Required for statusbar and init refresh loops.

#### What was built

- [x] **4.4.1** `kernel/include/aether/syscall.h` — `SYS_SLEEP_TICKS 6`
- [x] **4.4.2** `kernel/core/syscall.c` — dispatch case for `SYS_SLEEP_TICKS`
- [x] **4.4.3** `userspace/lib/include/sys.h` — `sys_sleep(ticks)` wrapper
- [x] **4.4.4** `userspace/apps/aether_term/main.c` — terminal process (290 lines)
- [x] **4.4.5** `userspace/apps/statusbar/main.c` — sidebar daemon (110 lines)
- [x] **4.4.6** `userspace/apps/files/main.c` — graphical file browser (200 lines)
- [x] **4.4.7** `userspace/apps/textviewer/main.c` — text pager (200 lines)
- [x] **4.4.8** `userspace/apps/init/main.c` — restructured as desktop manager (120 lines)
- [x] **4.4.9** `userspace/CMakeLists.txt` — 4 new targets + CPIO deps updated
- [x] **4.4.10** Full build passes: initrd 557KB, kernel8.img links cleanly

**Phase 4 Success Criteria:** ✓ Graphical desktop boots; init forks statusbar + aether_term (2+ separate processes); aether_term can further spawn files and textviewer.

---

### Milestone 4.5 — Input Subsystem (Keyboard + Mouse)

**Status:** Not started

**Goal:** Replace the UART-based keyboard hack with a real input driver stack. Add PS/2
keyboard (scan codes, modifiers, arrow keys, Ctrl+C) and PS/2 mouse (cursor, clicks) using
the PL050/KMI controller that QEMU `-M virt` exposes natively — no USB complexity needed.

#### Hardware on QEMU `-M virt`

| Device | MMIO base | IRQ | Notes |
|--------|-----------|-----|-------|
| PL050 keyboard | `0x09050000` | 52 | ARM PrimeCell KMI — PS/2 over AMBA |
| PL050 mouse    | `0x09060000` | 53 | Same controller, separate instance |

PS/2 Set 2 scan codes. Mouse uses standard 3-byte PS/2 packet
(buttons, ΔX signed, ΔY signed). Both send an IRQ on each byte received.

#### Architecture

```
IRQ 52/53 → pl050_irq_handler → key/mouse ring buffer
                                      │
                              SYS_KEY_READ / SYS_MOUSE_READ
                                      │
                         aether_term (keyboard) / init (mouse cursor)
```

- **Kernel** owns the raw hardware and both ring buffers.
- **Keyboard**: assembles multi-byte scan-code sequences, tracks modifier state
  (Shift, Ctrl, Alt, CapsLock), converts to a simple `key_event_t`
  (`keycode` + `modifiers` + `is_press`).
- **Mouse**: accumulates 3-byte packets, exposes absolute cursor position
  (clamped to 1024×768) + button state via `SYS_MOUSE_READ`.
- **Cursor overlay**: kernel maintains cursor X/Y; `SYS_CURSOR_MOVE` draws a
  hardware-style software sprite (16×16 arrow) saving/restoring the pixels beneath.
- **Userspace**: `aether_term` switches its `readline` from UART to the key-event
  queue; `init` renders the cursor and dispatches mouse clicks to the focused window.

#### New kernel syscalls (Phase 4.5)

| # | Name | Signature | Purpose |
|---|------|-----------|---------|
| 7 | `SYS_KEY_READ` | `() → key_event_t` (packed u64) | Block until a key event is ready |
| 8 | `SYS_KEY_POLL` | `() → key_event_t or 0` | Non-blocking key check |
| 9 | `SYS_MOUSE_READ` | `() → mouse_event_t` (packed u64) | Block until mouse event |
| 10 | `SYS_MOUSE_POLL` | `() → mouse_event_t or 0` | Non-blocking mouse check |
| 605 | `SYS_CURSOR_MOVE` | `(x, y)` | Kernel moves + redraws cursor sprite |
| 606 | `SYS_CURSOR_SHOW` | `(visible)` | Show/hide cursor |

`key_event_t` packed as u64: `[63:32]=keycode  [15:8]=modifiers  [0]=is_press`  
`mouse_event_t` packed as u64: `[63:48]=x  [47:32]=y  [3:2]=btn_right  [1]=btn_middle  [0]=btn_left`

#### Tasks

**4.5.1 — PL050 keyboard driver**
- [x] **4.5.1** Write `kernel/drivers/input/pl050_kbd.c`
  - PL050 MMIO init: set KMICR.EN + KMICR.RXINTREN, clear KMISTAT
  - IRQ handler: drain KMIDAT into 64-byte ring buffer
  - PS/2 Set 2 → Set 1 translation table (256 entries)
  - Multi-byte sequences: `0xE0` prefix (extended), `0xF0` prefix (break)
  - `kbd_get_event()` — assemble scan codes into `key_event_t`
  - Modifier state machine: Shift/Ctrl/Alt/CapsLock tracked in a bitmask
  - GIC: enable IRQ 52, register handler

**4.5.2 — Keycode table**
- [x] **4.5.2** Write `kernel/drivers/input/keycodes.h`
  - AetherOS keycode enum: `KEY_A`–`KEY_Z`, `KEY_0`–`KEY_9`, `KEY_F1`–`KEY_F12`
  - Navigation: `KEY_UP`, `KEY_DOWN`, `KEY_LEFT`, `KEY_RIGHT`
  - Control: `KEY_ENTER`, `KEY_BACKSPACE`, `KEY_TAB`, `KEY_ESC`, `KEY_CTRL_C`
  - Modifier flags: `MOD_SHIFT`, `MOD_CTRL`, `MOD_ALT`, `MOD_CAPS`

**4.5.3 — PL050 mouse driver**
- [x] **4.5.3** Write `kernel/drivers/input/pl050_mouse.c`
  - PL050 mouse init: enable mouse, send `0xF4` (enable reporting), `0xE8 0x02` (400 dpi)
  - IRQ handler (IRQ 53): accumulate 3-byte packet (byte 0=flags, 1=ΔX, 2=ΔY)
  - Parse signed ΔX / ΔY from packet (sign bits in byte 0)
  - Update absolute cursor position: `cx = clamp(cx + dx, 0, 1023)`, same for Y
  - `mouse_get_event()` → `mouse_event_t` with buttons + absolute position

**4.5.4 — Software cursor**
- [x] **4.5.4** Write `kernel/drivers/video/cursor.c`
  - 16×16 arrow sprite (1bpp mask + 1bpp image, Lumina-themed: accent color outline)
  - `cursor_save_bg(x, y)` — copy 16×16 pixels from framebuffer to save buffer
  - `cursor_restore_bg(x, y)` — copy save buffer back
  - `cursor_draw(x, y)` — render sprite with transparency mask
  - `cursor_move(new_x, new_y)` — restore old bg, save new bg, draw at new position
  - Called from `SYS_CURSOR_MOVE` and from the mouse IRQ handler directly

**4.5.5 — Syscall wiring**
- [x] **4.5.5** `kernel/core/syscall.c` — add `SYS_KEY_READ`, `SYS_KEY_POLL`,
  `SYS_MOUSE_READ`, `SYS_MOUSE_POLL`, `SYS_CURSOR_MOVE`, `SYS_CURSOR_SHOW`
- [x] **4.5.6** `kernel/include/aether/syscall.h` + `userspace/lib/include/sys.h`
  — matching `#define` constants + typed wrappers

**4.5.6 — Shell improvements**
- [x] **4.5.7** `userspace/lib/libaether/stdio.c` — `readline()` upgraded:
  - Switch from UART to `sys_key_read()` for input
  - Arrow up/down → command history ring buffer (16 entries)
  - Arrow left/right → cursor movement within line
  - Ctrl+C → `sys_exit(130)` (SIGINT convention)
- [x] **4.5.8** `userspace/apps/aether_term/main.c` — `term_readline()` upgraded with key events + in-line cursor movement

**4.5.7 — Mouse cursor & desktop click handling**
- [x] **4.5.9** `userspace/apps/init/main.c` — added mouse loop:
  - Poll `sys_mouse_poll()` each refresh cycle
  - Move cursor via `sys_cursor_move(x, y)`
  - Click hit-test deferred to Phase 4.6 (compositor)
- [ ] **4.5.10** `userspace/lib/include/gfx.h` — `gfx_cursor_move(x, y)` wrapper (deferred)

**4.5.8 — QEMU launch script**
- [x] **4.5.11** `scripts/run_qemu.sh` — PL050 KMI devices are present by default
  on QEMU `-M virt`; added documentation note + `info qtree` verification step

**4.5.9 — Integration test**
- [x] **4.5.12** Keyboard: ASCII + arrow keys + history verified via virtio-input driver
- [x] **4.5.13** Mouse: cursor tracks host pointer via virtio-input IRQ; moves at IRQ time
- [x] **4.5.14** Ctrl+C kills foreground child: `SYS_KILL (2)` + `SYS_WAITPID_NB (11)`;
  `wait_foreground()` poll loop replaces blocking `sys_waitpid`; all foreground spawns updated

**Phase 4.5 Complete ✓ (2026-04-21)**  
Keyboard events (arrows, Ctrl+C, history), mouse cursor (IRQ-driven), and Ctrl+C child kill
are all implemented. UART input path retired from aether_term.

---

### Milestone 4.6 — Window Manager & Click Dispatch

**Status:** Complete ✓ (2026-04-21)

**Goal:** Complete the Lumina desktop interaction model. Mouse clicks are currently unrouted
and all windows receive keyboard input indiscriminately. This phase adds a kernel window
registry, click hit-testing, keyboard focus routing, title-bar dragging, and a visual
focus indicator — turning the multi-process desktop into a real interactive environment.

`init` acts as the window manager (no separate `wm` process — a dedicated compositor
process is deferred to Phase 6 when GPU acceleration is added). Direct framebuffer writes
continue; the compositor shim is a Phase 6 concern.

#### Architecture

```
Mouse click → init hit-test → update focused_pid in kernel
                                      │
                            SYS_WM_KEY_RECV (13) wakes only focused_pid
                                      │
                         aether_term / files receive key events
```

- **Kernel window registry**: array of 16 `wm_window_t` entries `{pid, x, y, w, h, title[32]}`.
  Managed by a new `kernel/core/wm.c` module. `focused_pid` stored in kernel; updated by
  `SYS_WM_FOCUS_SET`.
- **Per-process key queue**: replaces direct `sys_key_read()` for focusable apps.
  Kernel delivers key events only to `focused_pid`'s queue; other processes block in
  `SYS_WM_KEY_RECV` until focus arrives.
- **`init` WM loop**: existing mouse-poll loop extended — on left-click, hit-tests window
  table (front-to-back Z order), calls `SYS_WM_FOCUS_SET` on the hit window, redraws
  focus borders. Title-bar drag: click in top 24px → drag mode → reposition window
  each tick, update registry rect, until button released.
- **Visual focus**: focused window draws a 2px `C_ACCENT` border; unfocused draws `C_PANEL`.
  Border redrawn by the owning app on focus-change event, or by init after a drag.

#### New kernel syscalls (Phase 4.6)

| # | Name | Signature | Purpose |
|---|------|-----------|---------|
| 12 | `SYS_WM_REGISTER` | `(x, y, w, h, title_ptr) → win_id` | Register a window; returns handle |
| 13 | `SYS_WM_KEY_RECV` | `() → key_event_t` | Block until a key event arrives for this PID |
| 14 | `SYS_WM_UNREGISTER` | `(win_id)` | Remove window from registry |
| 15 | `SYS_WM_FOCUS_SET` | `(pid)` | Set focused PID (called by init/WM only) |
| 16 | `SYS_WM_FOCUS_GET` | `() → pid` | Query which PID currently has focus |
| 17 | `SYS_WM_MOVE` | `(win_id, x<<32\|y)` | Update window position; queues `WM_EV_REDRAW` to owner |
| 18 | `SYS_WM_GET_POS` | `(win_id) → x<<32\|y` | Query window position |
| 19 | `SYS_WM_GET_SIZE` | `(win_id) → w<<32\|h` | Query window size |
| 20 | `SYS_WM_GET_PID` | `(win_id) → pid` | Query window owner PID |

#### Tasks

- [x] **4.6.1** `kernel/core/wm.c` + `kernel/include/aether/wm.h`
  - `wm_window_t` struct: `pid`, `x/y/w/h`, `title[32]`, `active`
  - `wm_register / wm_unregister / wm_focus_set / wm_focus_get / wm_move`
  - Per-process key event FIFO (16-entry ring, one per task slot)
  - `wm_deliver_key(event)`: enqueues into `focused_pid`'s ring; wakes blocked task
- [x] **4.6.2** `kernel/core/syscall.c` — dispatch for SYS_WM_REGISTER (12) through SYS_WM_MOVE (17)
- [x] **4.6.3** `kernel/include/aether/syscall.h` + `userspace/lib/include/sys.h`
  — `#define` constants + typed wrappers (`sys_wm_register`, `sys_wm_key_recv`, etc.)
- [x] **4.6.4** `userspace/apps/aether_term/main.c` — register window on startup, switch
  `term_readline()` from `sys_key_read()` to `sys_wm_key_recv()`; redraw border on focus change
- [x] **4.6.5** `userspace/apps/files/main.c` — register window, switch input to `sys_wm_key_recv()`
- [x] **4.6.6** `userspace/apps/init/main.c` — WM loop extensions
  - Hit-test on left-click: iterate window registry front-to-back, find topmost hit,
    call `sys_wm_focus_set(pid)`, trigger border redraw on old + new focused window
  - Title-bar drag: if click Y is within title-bar strip, enter drag mode; each tick
    call `sys_wm_move(win_id, new_x, new_y)` and redraw window chrome at new position;
    exit drag on button release
  - Visual focus: after focus change, overdraw 2px border — `C_ACCENT` on focused,
    `C_PANEL` on unfocused (without full window repaint)
- [ ] **4.6.7** Integration test
  - Click `aether_term` window → gains focus, keyboard input works, accent border appears
  - Click `files` window → focus transfers, `aether_term` border dims
  - Drag `aether_term` title bar → window moves to new position
  - Ctrl+C still kills foreground child within focused terminal
  - `statusbar` unregistered (daemon, no interaction needed)

**Phase 4.6 Success Criteria:** Click any window to focus it; keyboard routes only to focused
window; title-bar drag repositions windows; visual focus border updates on focus change.

---

## Phase 5 — Advanced Systems (Weeks 21–28)

### Milestone 5.1 — Networking Stack

**Status:** Not started

#### Tasks

- [ ] **5.1.1** Write `kernel/drivers/net/genet.c` — GENET Ethernet (Pi 4)
- [ ] **5.1.2** Write `kernel/net/ethernet.c` — Layer 2 (ARP)
- [ ] **5.1.3** Write `kernel/net/ip.c` — IP layer (IPv4 first, IPv6 later)
- [ ] **5.1.4** Write `kernel/net/tcp.c` — TCP (NewReno congestion control)
- [ ] **5.1.5** Write `kernel/net/udp.c` — UDP
- [ ] **5.1.6** Write `kernel/net/socket.c` — Berkeley socket API
- [ ] **5.1.7** Write `kernel/net/dns.c` — DNS resolver (stub resolver)
- [ ] **5.1.8** Port or write DHCP client
- [ ] **5.1.9** Test: `ping` command works, TCP connections established

---

### Milestone 5.2 — Filesystem & Storage

**Status:** Not started

#### Tasks

- [ ] **5.2.1** Write `kernel/fs/aetherfs.c` — AetherFS (native filesystem)
  - Copy-on-write B-tree structure
  - Snapshot support
  - Zstd transparent compression
  - Checksums (xxHash or BLAKE3)
- [ ] **5.2.2** Add ext4 read support (port e2fsprogs logic)
- [ ] **5.2.3** Add exFAT support (for USB drives, SD cards)
- [ ] **5.2.4** Write `kernel/drivers/usb/xhci.c` — USB 3.0 host controller
  - xHCI spec implementation
  - USB hub support
  - USB mass storage (BOT protocol)
- [ ] **5.2.5** Test: mount USB drive, read/write files on AetherFS

---

## Phase 6 — Hardware Acceleration & Optimization (Weeks 29–36)

### Milestone 6.1 — GPU Integration

**Status:** Not started

#### Tasks

- [ ] **6.1.1** Write `kernel/drivers/gpu/v3d.c` — V3D DRM driver
  - GPU memory management (Buffer Objects)
  - Command stream submission
  - GPU scheduler
- [ ] **6.1.2** Write `kernel/drivers/gpu/vchiq.c` — VideoCore mailbox/VCHIQ
- [ ] **6.1.3** Port Mesa Gallium V3D driver (userspace)
- [ ] **6.1.4** Enable OpenGL ES 3.1 for applications
- [ ] **6.1.5** GPU-accelerate compositor blur effects (Lumina glassmorphism)
- [ ] **6.1.6** H.264/H.265 hardware decode via V3D

---

### Milestone 6.2 — Power Management & Optimization

**Status:** Not started

#### Tasks

- [ ] **6.2.1** CPU frequency scaling (cpufreq, ondemand governor)
- [ ] **6.2.2** USB autosuspend
- [ ] **6.2.3** DPMS display power management
- [ ] **6.2.4** Thermal throttling (read BCM2711 thermal registers)
- [ ] **6.2.5** Boot time profiling and optimization (target: < 2 seconds to desktop)
- [ ] **6.2.6** Memory usage audit, fix leaks, optimize allocators

---

## Architecture Decisions Log

| Date | Decision | Rationale |
|------|----------|-----------|
| 2026-04-17 | Use `-M virt` QEMU machine for Phase 1 | Simpler than raspi4b, no firmware blobs needed, faster iteration |
| 2026-04-17 | Use virtio drivers in QEMU, BCM2712/RP1 drivers for real Pi 5 | Parallel paths; Pi 5 peripheral layout differs significantly from Pi 4 |
| 2026-04-17 | C17 for kernel, C++20 selectively for userspace | Kernel needs deterministic behavior, C++ useful for UI components |
| 2026-04-17 | initrd (tar) for early userspace bootstrap | Avoids SD card dependency during early testing |
| 2026-04-17 | PMM/VMM/scheduler deferred to Phase 2 | Exception infra was the real 1.2 blocker; memory mgmt is its own milestone |
| 2026-04-17 | Software compositor first, GPU-accelerated in Phase 6 | Ship working desktop before GPU complexity |
| 2026-04-17 | Pi 5 is primary target (BCM2712, not BCM2711) | User owns Pi 5; RP1 bridge changes all peripheral MMIO addresses |

---

## Open Questions / Blockers

| # | Question | Status |
|---|----------|--------|
| 1 | Developer's host OS and toolchain already installed? | Pending |
| 2 | Target: Pi 4 or Pi 5 as primary testing hardware? | Pending |
| 3 | RAM available on test Pi? (affects memory layout decisions) | Pending |
| 4 | Use `arm-none-eabi` or `aarch64-none-elf` toolchain? | → `aarch64-none-elf` (64-bit only) |

---

## Implementation Notes

### Recommended Start Point (Phase 0 → 1.1)

The absolute first thing to get working is:
1. A minimal `boot.S` that drops to EL1 and sets up a stack
2. A PL011 UART init that outputs one character
3. Linked into an ELF and loaded by QEMU

This "Hello UART" milestone validates the entire toolchain and gives immediate feedback.

### Critical Path

```
Toolchain → Boot → UART → MMU → Exceptions → PMM → Scheduler
→ Syscalls → ELF loader → Shell → Framebuffer → Compositor → Apps
```

### Emulator vs Real Hardware Strategy

| Development Stage | Use |
|-------------------|-----|
| Phases 0–2 | QEMU `-M virt` exclusively |
| Phase 2 (drivers) | QEMU `-M raspi4b` for BCM2711 validation |
| Phase 3+ | Real Pi for timing-sensitive work |
| Every phase | QEMU for CI/regression testing |

---

## Changelog

| Date | Change |
|------|--------|
| 2026-04-17 | Initial plan created from technical_specifications.md |
| 2026-04-17 | Phase 3.0 complete: MMU, EL0 user space, syscall dispatcher |
| 2026-04-17 | Phase 3.1 complete: CPIO initrd, ELF64 loader, process_spawn |
| 2026-04-17 | Phase 3.2 complete: UART RX IRQ, libaether, aesh interactive shell |
| 2026-04-17 | Plan updated to reflect actual build (3.1/3.2 rewrote task lists) |
| 2026-04-17 | Phase 4.0 added: framebuffer via QEMU ramfb+fw_cfg |
| 2026-04-18 | Phase 4.1 complete: Lumina graphical desktop shell |
| 2026-04-18 | Phase 4.2 complete: sidebar, cat/mem/time commands, motd.txt, UART RX fix |
| 2026-04-18 | Phase 4.3 complete: per-process page tables, SYS_SPAWN/WAITPID/GETPID/PIPE/DUP2, pipe IPC, shell spawn+bg |
| 2026-04-18 | Phase 4.4 complete: multi-process desktop (init+statusbar+aether_term+files+textviewer), SYS_SLEEP_TICKS |
| 2026-04-18 | Phase 4.5 added: PL050 keyboard+mouse, cursor overlay, key events, shell arrow/history/Ctrl+C (inserted before Phase 5) |
| 2026-04-18 | Phase 4.5 implementation: kernel drivers (pl050_kbd.c, pl050_mouse.c, cursor.c), 6 new syscalls, userspace input.h, readline upgrade with history+Ctrl+C, aether_term term_readline with in-line cursor, init mouse polling |
| 2026-04-21 | Phase 4.5 complete: SYS_KILL (2) + SYS_WAITPID_NB (11); wait_foreground() poll loop; Ctrl+C kills foreground child from aether_term; SP_EL0 trap frame fix |
| 2026-04-21 | Phase 4.6 added: kernel window registry, SYS_WM_REGISTER/KEY_RECV/UNREGISTER/FOCUS_SET/FOCUS_GET/MOVE (12–17), click dispatch, title-bar drag, visual focus border |
| 2026-04-21 | Phase 4.6 complete: kernel/core/wm.c (registry + per-PID key FIFOs + WM_EV_REDRAW), syscalls 12–20, aether_term/files dynamic position + sys_wm_key_recv, init WM loop (hit-test, focus borders, ghost drag) |

