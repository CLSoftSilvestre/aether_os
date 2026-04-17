/*
 * AetherOS — AArch64 Linker Script (QEMU virt machine)
 *
 * Memory map for QEMU "-M virt":
 *   0x00000000 – 0x3FFFFFFF   Peripheral MMIO (UART at 0x09000000)
 *   0x40000000 – 0x7FFFFFFF   RAM (1GB when launched with -m 1G)
 *
 * We load the kernel at 0x40000000 (RAM start).
 * Stack lives just after the kernel image, growing downward from __stack_top.
 *
 * Pi 5 note: when targeting real hardware the load address changes to
 * 0x80000 (Pi convention). We will add a separate linker script for that
 * in Phase 2. For now, QEMU virt is our development target.
 */

ENTRY(_start)

SECTIONS
{
    /*
     * Kernel load address: start of RAM on QEMU virt machine.
     * ". = <address>" sets the current location counter.
     * Think of it like ORG in 8086 assembly.
     */
    . = 0x40000000;

    /*
     * .text.boot — boot code MUST be at the very beginning of the image
     * so that _start is the first instruction executed.
     * The QEMU -kernel flag jumps to the first byte of the loaded image.
     */
    .text.boot : {
        KEEP(*(.text.boot))
    }

    /* All other code */
    .text : {
        *(.text*)
    }

    /* Read-only data (string literals, const globals) */
    .rodata : ALIGN(8) {
        *(.rodata*)
    }

    /* Initialised global/static variables */
    .data : ALIGN(8) {
        *(.data*)
    }

    /*
     * BSS — uninitialised globals (zeroed by boot.S before kernel_main).
     * The C standard guarantees global variables are zero-initialised;
     * we have to do that manually since there is no OS to do it for us.
     * Equivalent to the .bss segment you may have seen in x86 NASM listings.
     */
    .bss : ALIGN(16) {
        __bss_start = .;
        *(.bss*)
        *(COMMON)
        __bss_end = .;
    }

    /* Align to 16-byte boundary after BSS */
    . = ALIGN(16);

    /* Kernel end marker — used for dynamic memory allocation later */
    __kernel_end = .;

    /*
     * Stack — 64 KB, placed immediately after the kernel image.
     * Stack grows DOWNWARD in AArch64 (same as x86).
     * SP starts at __stack_top and moves toward __stack_bottom as you push.
     */
    __stack_bottom = .;
    . += 0x10000;   /* 64 KB */
    __stack_top = .;
}
