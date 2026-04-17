/*
 * AetherOS — Userspace Linker Script
 *
 * User programs load at 0x70000000 (start of the user-accessible RAM region).
 * The kernel maps 0x70000000–0x7FFFFFFF as AP=BOTH_RW so EL0 can execute there.
 *
 * Stack is provided by the kernel at VMM_USER_STACK_TOP (0x7FFFF000),
 * so we do not allocate stack space here.
 */

ENTRY(_start)

SECTIONS
{
    . = 0x70000000;

    .text : {
        KEEP(*(.text.boot))   /* crt0._start must come first */
        *(.text*)
    }

    .rodata : ALIGN(8) {
        *(.rodata*)
    }

    . = ALIGN(4096);

    .data : {
        *(.data*)
    }

    .bss : ALIGN(16) {
        __bss_start = .;
        *(.bss*)
        *(COMMON)
        __bss_end = .;
    }

    . = ALIGN(16);
    __user_end = .;

    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.gnu*)
    }
}
