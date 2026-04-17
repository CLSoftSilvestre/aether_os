#ifndef AETHER_ELF_H
#define AETHER_ELF_H

/*
 * AetherOS — ELF64 Loader
 *
 * Parses a static AArch64 ELF64 executable and copies its PT_LOAD segments
 * into memory at their virtual addresses (identity map: VA == PA).
 *
 * Only static executables are supported (no dynamic linker, no GOT/PLT fixups).
 * User programs must be compiled with -static -nostdlib.
 *
 * ELF64 format (AArch64, little-endian):
 *   e_ident[0..3] = 0x7f 'E' 'L' 'F'
 *   e_ident[4]    = ELFCLASS64 (2)
 *   e_ident[5]    = ELFDATA2LSB (1, little-endian)
 *   e_type        = ET_EXEC (2, executable)
 *   e_machine     = EM_AARCH64 (183)
 *   e_entry       = virtual address of first instruction
 *   e_phoff       = offset of first program header
 *   e_phnum       = number of program headers
 *
 * For each PT_LOAD program header:
 *   p_vaddr  = destination virtual address
 *   p_offset = source offset in ELF file
 *   p_filesz = bytes to copy from file
 *   p_memsz  = total size (p_memsz - p_filesz bytes of BSS to zero)
 */

#include "aether/types.h"

/* ── ELF64 structures ────────────────────────────────────────────── */

#define EI_NIDENT    16

#define ELFMAG0      0x7fU
#define ELFMAG1      'E'
#define ELFMAG2      'L'
#define ELFMAG3      'F'
#define ELFCLASS64   2
#define ELFDATA2LSB  1

#define ET_EXEC      2
#define EM_AARCH64   183
#define EV_CURRENT   1

#define PT_NULL      0
#define PT_LOAD      1

typedef struct {
    u8  e_ident[EI_NIDENT];
    u16 e_type;
    u16 e_machine;
    u32 e_version;
    u64 e_entry;
    u64 e_phoff;
    u64 e_shoff;
    u32 e_flags;
    u16 e_ehsize;
    u16 e_phentsize;
    u16 e_phnum;
    u16 e_shentsize;
    u16 e_shnum;
    u16 e_shstrndx;
} __attribute__((packed)) Elf64_Ehdr;

typedef struct {
    u32 p_type;
    u32 p_flags;
    u64 p_offset;
    u64 p_vaddr;
    u64 p_paddr;
    u64 p_filesz;
    u64 p_memsz;
    u64 p_align;
} __attribute__((packed)) Elf64_Phdr;

/* ── Public API ──────────────────────────────────────────────────── */

/*
 * elf_load — parse and load a static ELF64 executable.
 *
 * elf_data: pointer to the raw ELF file bytes (in kernel memory)
 * elf_size: size of the ELF file in bytes
 *
 * Loads all PT_LOAD segments to their virtual addresses (identity map).
 * BSS (p_memsz > p_filesz) is zeroed.
 *
 * Returns the entry point virtual address on success, 0 on error.
 */
uintptr_t elf_load(const void *elf_data, u32 elf_size);

#endif /* AETHER_ELF_H */
