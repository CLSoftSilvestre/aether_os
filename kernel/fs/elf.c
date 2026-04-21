/*
 * AetherOS — ELF64 Loader
 * File: kernel/fs/elf.c
 *
 * Loads a static AArch64 ELF64 executable into memory.
 *
 * Identity mapping (VA == PA) means we can directly use the virtual
 * addresses from the ELF program headers as destination pointers.
 * The user region (0x70000000–0x7FFFFFFF) is mapped AP=BOTH_RW so
 * the kernel (EL1) can freely write there.
 *
 * Supports: static executables, PT_LOAD segments, BSS zeroing.
 * Does not support: shared libraries, dynamic linking, compressed ELFs.
 */

#include "aether/elf.h"
#include "aether/printk.h"
#include "aether/types.h"

/* ── Helpers ────────────────────────────────────────────────────── */

static void *memcpy_kernel(void *dst, const void *src, u64 n)
{
    u8       *d = (u8 *)dst;
    const u8 *s = (const u8 *)src;
    for (u64 i = 0; i < n; i++) d[i] = s[i];
    return dst;
}

static void *memzero(void *dst, u64 n)
{
    u8 *d = (u8 *)dst;
    for (u64 i = 0; i < n; i++) d[i] = 0;
    return dst;
}

/* ── ELF loader ─────────────────────────────────────────────────── */

uintptr_t elf_load(const void *elf_data, u32 elf_size)
{
    if (!elf_data || elf_size < sizeof(Elf64_Ehdr)) {
        kerror("ELF: data too small (%lu bytes)\n", (unsigned long)elf_size);
        return 0;
    }

    const Elf64_Ehdr *ehdr = (const Elf64_Ehdr *)elf_data;

    /* ── 1. Validate ELF header ─────────────────────────────────── */
    if (ehdr->e_ident[0] != ELFMAG0 || ehdr->e_ident[1] != ELFMAG1 ||
        ehdr->e_ident[2] != ELFMAG2 || ehdr->e_ident[3] != ELFMAG3) {
        kerror("ELF: bad magic (not an ELF file)\n");
        return 0;
    }
    if (ehdr->e_ident[4] != ELFCLASS64) {
        kerror("ELF: not a 64-bit ELF\n");
        return 0;
    }
    if (ehdr->e_ident[5] != ELFDATA2LSB) {
        kerror("ELF: not little-endian\n");
        return 0;
    }
    if (ehdr->e_type != ET_EXEC) {
        kerror("ELF: not an executable (type=%u)\n", ehdr->e_type);
        return 0;
    }
    if (ehdr->e_machine != EM_AARCH64) {
        kerror("ELF: wrong architecture (machine=%u, want %u)\n",
               ehdr->e_machine, EM_AARCH64);
        return 0;
    }
    if (ehdr->e_phnum == 0 || ehdr->e_phoff == 0) {
        kerror("ELF: no program headers\n");
        return 0;
    }

    kinfo("ELF: entry=%p  phnum=%u\n",
          (void *)(uintptr_t)ehdr->e_entry, ehdr->e_phnum);

    /* ── 2. Load PT_LOAD segments ───────────────────────────────── */
    const u8 *base = (const u8 *)elf_data;

    for (u16 i = 0; i < ehdr->e_phnum; i++) {
        uintptr_t ph_off = (uintptr_t)ehdr->e_phoff +
                           (uintptr_t)i * ehdr->e_phentsize;

        if (ph_off + sizeof(Elf64_Phdr) > elf_size) {
            kerror("ELF: program header %u beyond file bounds\n", i);
            return 0;
        }

        const Elf64_Phdr *phdr = (const Elf64_Phdr *)(base + ph_off);

        if (phdr->p_type != PT_LOAD) continue;
        if (phdr->p_memsz == 0)      continue;

        if (phdr->p_offset + phdr->p_filesz > elf_size) {
            kerror("ELF: segment %u data beyond file bounds\n", i);
            return 0;
        }

        void    *dst  = (void *)(uintptr_t)phdr->p_vaddr;
        const u8 *src = base + phdr->p_offset;

        /* Copy file data */
        if (phdr->p_filesz > 0)
            memcpy_kernel(dst, src, phdr->p_filesz);

        /* Zero BSS portion (memsz > filesz) */
        if (phdr->p_memsz > phdr->p_filesz)
            memzero((u8 *)dst + phdr->p_filesz,
                    phdr->p_memsz - phdr->p_filesz);

        kinfo("ELF:   LOAD  va=%p  filesz=%lu  memsz=%lu\n",
              (void *)(uintptr_t)phdr->p_vaddr,
              (unsigned long)phdr->p_filesz,
              (unsigned long)phdr->p_memsz);
    }

    /* ── 3. Instruction cache coherency ────────────────────────── */
    /*
     * For each cache line of written code: clean D-cache (dc cvau) then
     * invalidate I-cache (ic ivau), followed by barriers.
     * A bare "dsb ish; isb" is insufficient on real hardware because it
     * does not push dirty D-cache lines to the PoU.
     */
    {
        /* Flush entire loaded image — rescan segments for VA range */
        uintptr_t flush_lo = (uintptr_t)-1, flush_hi = 0;
        for (u16 i = 0; i < ehdr->e_phnum; i++) {
            uintptr_t ph_off2 = (uintptr_t)ehdr->e_phoff +
                                (uintptr_t)i * ehdr->e_phentsize;
            const Elf64_Phdr *ph2 = (const Elf64_Phdr *)(base + ph_off2);
            if (ph2->p_type != PT_LOAD || ph2->p_memsz == 0) continue;
            uintptr_t lo = (uintptr_t)ph2->p_vaddr;
            uintptr_t hi = lo + (uintptr_t)ph2->p_memsz;
            if (lo < flush_lo) flush_lo = lo;
            if (hi > flush_hi) flush_hi = hi;
        }
        for (uintptr_t a = flush_lo & ~63UL; a < flush_hi; a += 64) {
            __asm__ volatile("dc cvau, %0" :: "r"(a) : "memory");
        }
        __asm__ volatile("dsb ish" ::: "memory");
        for (uintptr_t a = flush_lo & ~63UL; a < flush_hi; a += 64) {
            __asm__ volatile("ic ivau, %0" :: "r"(a) : "memory");
        }
        __asm__ volatile("dsb ish\n isb\n" ::: "memory");
    }

    kinfo("ELF: loaded OK — entry %p\n", (void *)(uintptr_t)ehdr->e_entry);
    return (uintptr_t)ehdr->e_entry;
}
