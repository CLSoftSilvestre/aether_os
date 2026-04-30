/*
 * AetherOS — Process Manager
 * File: kernel/core/process.c
 */

#include "aether/process.h"
#include "aether/initrd.h"
#include "aether/elf.h"
#include "aether/mm.h"
#include "aether/scheduler.h"
#include "aether/vmm.h"
#include "aether/printk.h"
#include "aether/types.h"

/* ── Helpers ─────────────────────────────────────────────────────── */

static void *memcpy_proc(void *dst, const void *src, u64 n)
{
    u8 *d = (u8 *)dst;
    const u8 *s = (const u8 *)src;
    for (u64 i = 0; i < n; i++) d[i] = s[i];
    return dst;
}

static void memzero_proc(void *dst, u64 n)
{
    u8 *d = (u8 *)dst;
    for (u64 i = 0; i < n; i++) d[i] = 0;
}

/* ── Trampolines ─────────────────────────────────────────────────── */

/*
 * user_task_trampoline — runs in EL1 when a user task is first scheduled.
 * Retrieves entry, sp, and page-table PA from the task_t, switches TTBR0
 * to the task's private page table, then eret-s into EL0.
 */
static void user_task_trampoline(void)
{
    uintptr_t entry, sp, l1_phys, argv_va;
    u32       argc;
    task_get_user_regs(&entry, &sp, &l1_phys, &argc, &argv_va);
    vmm_switch_user_pt(l1_phys);   /* load process page table before eret */
    kinfo("Process: entering EL0 — entry=%p  sp=%p  argc=%u\n",
          (void *)entry, (void *)sp, argc);
    launch_el0(entry, sp, (uintptr_t)argc, argv_va);
}

/* ── process_spawn — global identity-mapped init launch ─────────── */

int process_spawn(const char *path)
{
    kinfo("Process: spawning '%s' (global PT)\n", path);

    u32 elf_size = 0;
    const void *elf_data = initrd_find(path, &elf_size);
    if (!elf_data) {
        kerror("Process: '%s' not found in initrd\n", path);
        return -1;
    }

    uintptr_t entry = elf_load(elf_data, elf_size);
    if (!entry) {
        kerror("Process: ELF load failed for '%s'\n", path);
        return -1;
    }

    int rc = task_create_user(entry, VMM_USER_STACK_TOP,
                              path, user_task_trampoline);
    if (rc != 0) {
        kerror("Process: task_create_user failed\n");
        return -1;
    }

    kinfo("Process: '%s' queued (entry=%p)\n", path, (void *)entry);
    return 0;
}

/* ── process_spawn_child — isolated spawn with per-process PT ────── */

#define SPAWN_STACK_PAGES 64   /* 256 KB user stack */

int process_spawn_child(const char *path, u32 ppid, u32 *child_pid_out)
{
    kinfo("Process: spawning child '%s' ppid=%lu\n", path, (unsigned long)ppid);

    /* 1. Locate ELF in initrd */
    u32 elf_size = 0;
    const void *elf_data = initrd_find(path, &elf_size);
    if (!elf_data) {
        kerror("Process: '%s' not found\n", path);
        return -1;
    }

    const Elf64_Ehdr *ehdr = (const Elf64_Ehdr *)elf_data;
    if (elf_size < sizeof(Elf64_Ehdr)             ||
        ehdr->e_ident[0] != ELFMAG0               ||
        ehdr->e_ident[4] != ELFCLASS64            ||
        ehdr->e_type     != ET_EXEC               ||
        ehdr->e_machine  != EM_AARCH64) {
        kerror("Process: '%s' invalid ELF\n", path);
        return -1;
    }

    /* 2. Determine VA footprint of all PT_LOAD segments */
    uintptr_t va_min = (uintptr_t)-1, va_max = 0;
    const u8 *base = (const u8 *)elf_data;
    for (u16 i = 0; i < ehdr->e_phnum; i++) {
        const Elf64_Phdr *ph = (const Elf64_Phdr *)
            (base + ehdr->e_phoff + (uintptr_t)i * ehdr->e_phentsize);
        if (ph->p_type != PT_LOAD || ph->p_memsz == 0) continue;
        if (ph->p_vaddr < va_min) va_min = (uintptr_t)ph->p_vaddr;
        uintptr_t end = (uintptr_t)ph->p_vaddr + (uintptr_t)ph->p_memsz;
        if (end > va_max) va_max = end;
    }
    if (va_min == (uintptr_t)-1) {
        kerror("Process: '%s' has no loadable segments\n", path);
        return -1;
    }

    /* Round up code allocation to page boundary */
    u32 code_pages = (u32)((va_max - va_min + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE);

    /* 3. Allocate physical pages */
    uintptr_t code_phys  = pmm_alloc_pages(code_pages);
    if (!code_phys) { kerror("Process: OOM (code)\n"); return -1; }

    uintptr_t stack_phys = pmm_alloc_pages(SPAWN_STACK_PAGES);
    if (!stack_phys) {
        for (u32 i = 0; i < code_pages; i++)
            pmm_free_page(code_phys + (uintptr_t)i * PMM_PAGE_SIZE);
        kerror("Process: OOM (stack)\n");
        return -1;
    }

    /* 4. Create per-process page tables */
    uintptr_t proc_l1 = vmm_create_process_pt();
    if (!proc_l1) {
        kerror("Process: failed to create page tables\n");
        goto fail_pt;
    }

    /* 5. Map code pages: VA va_min → PA code_phys */
    if (vmm_map_user_pages(proc_l1, va_min, code_phys, code_pages) != 0) {
        kerror("Process: vmm_map_user_pages(code) failed\n");
        goto fail_map;
    }

    /* 6. Map stack pages: VA (STACK_TOP - stack_size) → PA stack_phys */
    uintptr_t stack_va = VMM_USER_STACK_TOP -
                         (uintptr_t)SPAWN_STACK_PAGES * PMM_PAGE_SIZE;
    if (vmm_map_user_pages(proc_l1, stack_va, stack_phys, SPAWN_STACK_PAGES) != 0) {
        kerror("Process: vmm_map_user_pages(stack) failed\n");
        goto fail_map;
    }

    /* 7. Copy ELF segments to physical memory (kernel can write via identity map) */
    for (u16 i = 0; i < ehdr->e_phnum; i++) {
        const Elf64_Phdr *ph = (const Elf64_Phdr *)
            (base + ehdr->e_phoff + (uintptr_t)i * ehdr->e_phentsize);
        if (ph->p_type != PT_LOAD || ph->p_memsz == 0) continue;

        u8 *dst = (u8 *)(code_phys + ((uintptr_t)ph->p_vaddr - va_min));
        const u8 *src = base + ph->p_offset;

        if (ph->p_filesz > 0)
            memcpy_proc(dst, src, (u64)ph->p_filesz);
        if (ph->p_memsz > ph->p_filesz)
            memzero_proc(dst + ph->p_filesz,
                         (u64)(ph->p_memsz - ph->p_filesz));
    }

    /* Instruction-cache coherency: clean D$ then invalidate I$ per cache line */
    {
        uintptr_t lo = code_phys;
        uintptr_t hi = code_phys + (uintptr_t)code_pages * PMM_PAGE_SIZE;
        for (uintptr_t a = lo; a < hi; a += 64)
            __asm__ volatile("dc cvau, %0" :: "r"(a) : "memory");
        __asm__ volatile("dsb ish" ::: "memory");
        for (uintptr_t a = lo; a < hi; a += 64)
            __asm__ volatile("ic ivau, %0" :: "r"(a) : "memory");
        __asm__ volatile("dsb ish\n isb\n" ::: "memory");
    }

    /* 8. Create the isolated task */
    int rc = task_create_isolated(
        (uintptr_t)ehdr->e_entry, VMM_USER_STACK_TOP,
        path, user_task_trampoline,
        proc_l1, ppid,
        code_phys,  code_pages,
        stack_phys, SPAWN_STACK_PAGES,
        0, 0,
        child_pid_out);

    if (rc != 0) {
        kerror("Process: task_create_isolated failed\n");
        goto fail_map;
    }

    kinfo("Process: child spawned pid=%lu entry=%p\n",
          child_pid_out ? (unsigned long)*child_pid_out : 0,
          (void *)(uintptr_t)ehdr->e_entry);
    return 0;

fail_map:
    vmm_free_process_pt(proc_l1);
fail_pt:
    for (u32 i = 0; i < SPAWN_STACK_PAGES; i++)
        pmm_free_page(stack_phys + (uintptr_t)i * PMM_PAGE_SIZE);
    for (u32 i = 0; i < code_pages; i++)
        pmm_free_page(code_phys  + (uintptr_t)i * PMM_PAGE_SIZE);
    return -1;
}

/* ── process_spawn_child_args — spawn with argc/argv on user stack ── */

#define ARGV_MAX      16
#define ARGV_STRLEN   256

int process_spawn_child_args(const char *path, u32 ppid, u32 *child_pid_out,
                              const char *const *argv, u32 argc)
{
    if (argc > ARGV_MAX) argc = ARGV_MAX;

    /* 1–7: same ELF load + page table setup as process_spawn_child */
    kinfo("Process: spawning child (args) '%s' ppid=%lu argc=%u\n",
          path, (unsigned long)ppid, argc);

    u32 elf_size = 0;
    const void *elf_data = initrd_find(path, &elf_size);
    if (!elf_data) { kerror("Process: '%s' not found\n", path); return -1; }

    const Elf64_Ehdr *ehdr = (const Elf64_Ehdr *)elf_data;
    if (elf_size < sizeof(Elf64_Ehdr)          ||
        ehdr->e_ident[0] != ELFMAG0            ||
        ehdr->e_ident[4] != ELFCLASS64         ||
        ehdr->e_type     != ET_EXEC            ||
        ehdr->e_machine  != EM_AARCH64) {
        kerror("Process: '%s' invalid ELF\n", path);
        return -1;
    }

    uintptr_t va_min = (uintptr_t)-1, va_max = 0;
    const u8 *base = (const u8 *)elf_data;
    for (u16 i = 0; i < ehdr->e_phnum; i++) {
        const Elf64_Phdr *ph = (const Elf64_Phdr *)
            (base + ehdr->e_phoff + (uintptr_t)i * ehdr->e_phentsize);
        if (ph->p_type != PT_LOAD || ph->p_memsz == 0) continue;
        if ((uintptr_t)ph->p_vaddr < va_min) va_min = (uintptr_t)ph->p_vaddr;
        uintptr_t end = (uintptr_t)ph->p_vaddr + (uintptr_t)ph->p_memsz;
        if (end > va_max) va_max = end;
    }
    if (va_min == (uintptr_t)-1) {
        kerror("Process: '%s' no loadable segments\n", path);
        return -1;
    }

    u32 code_pages = (u32)((va_max - va_min + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE);

    uintptr_t code_phys  = pmm_alloc_pages(code_pages);
    if (!code_phys) { kerror("Process: OOM (code)\n"); return -1; }

    uintptr_t stack_phys = pmm_alloc_pages(SPAWN_STACK_PAGES);
    if (!stack_phys) {
        for (u32 i = 0; i < code_pages; i++)
            pmm_free_page(code_phys + (uintptr_t)i * PMM_PAGE_SIZE);
        kerror("Process: OOM (stack)\n");
        return -1;
    }

    uintptr_t proc_l1 = vmm_create_process_pt();
    if (!proc_l1) { kerror("Process: page table alloc failed\n"); goto args_fail_pt; }

    if (vmm_map_user_pages(proc_l1, va_min, code_phys, code_pages) != 0)
        goto args_fail_map;

    uintptr_t stack_va = VMM_USER_STACK_TOP -
                         (uintptr_t)SPAWN_STACK_PAGES * PMM_PAGE_SIZE;
    if (vmm_map_user_pages(proc_l1, stack_va, stack_phys, SPAWN_STACK_PAGES) != 0)
        goto args_fail_map;

    /* Copy ELF segments */
    for (u16 i = 0; i < ehdr->e_phnum; i++) {
        const Elf64_Phdr *ph = (const Elf64_Phdr *)
            (base + ehdr->e_phoff + (uintptr_t)i * ehdr->e_phentsize);
        if (ph->p_type != PT_LOAD || ph->p_memsz == 0) continue;
        u8 *dst = (u8 *)(code_phys + ((uintptr_t)ph->p_vaddr - va_min));
        const u8 *src = base + ph->p_offset;
        if (ph->p_filesz > 0)  memcpy_proc(dst, src, (u64)ph->p_filesz);
        if (ph->p_memsz > ph->p_filesz)
            memzero_proc(dst + ph->p_filesz, (u64)(ph->p_memsz - ph->p_filesz));
    }
    {
        uintptr_t lo = code_phys;
        uintptr_t hi = code_phys + (uintptr_t)code_pages * PMM_PAGE_SIZE;
        for (uintptr_t a = lo; a < hi; a += 64)
            __asm__ volatile("dc cvau, %0" :: "r"(a) : "memory");
        __asm__ volatile("dsb ish" ::: "memory");
        for (uintptr_t a = lo; a < hi; a += 64)
            __asm__ volatile("ic ivau, %0" :: "r"(a) : "memory");
        __asm__ volatile("dsb ish\n isb\n" ::: "memory");
    }

    /* ── Write argv onto the top of the user stack ──────────────────── */
    /*
     * Layout (growing down from VMM_USER_STACK_TOP):
     *   [argv[0] string \0][argv[1] string \0]...[argv[argc-1] string \0]
     *   [u64 argv_ptrs[0]] [u64 argv_ptrs[1]] ... [u64 argv_ptrs[argc]]
     *   ← new SP (16-byte aligned)
     *
     * The kernel writes to the physical pages via identity mapping.
     * argv_ptrs contains user virtual addresses.
     */
    uintptr_t sp_va   = VMM_USER_STACK_TOP;
    uintptr_t sp_phys = stack_phys + (uintptr_t)SPAWN_STACK_PAGES * PMM_PAGE_SIZE;
    /* delta: phys = va + delta  (since stack_phys = stack_va + delta) */
    uintptr_t delta   = stack_phys - stack_va;

    uintptr_t str_ptrs[ARGV_MAX + 1];

    /* Phase 1: copy strings onto stack growing downward */
    for (int i = (int)argc - 1; i >= 0; i--) {
        u32 slen = 0;
        while (argv[i][slen]) slen++;
        slen++; /* include null terminator */
        if (slen > ARGV_STRLEN) slen = ARGV_STRLEN;
        sp_va   -= slen;
        sp_phys -= slen;
        memcpy_proc((u8 *)sp_phys, argv[i], slen);
        ((u8 *)sp_phys)[slen - 1] = '\0';  /* ensure null termination */
        str_ptrs[i] = sp_va;
    }
    str_ptrs[argc] = 0;  /* NULL sentinel */

    /* Phase 2: push argv pointer array */
    u32 ptrs_bytes = (argc + 1) * (u32)sizeof(uintptr_t);
    sp_va   -= ptrs_bytes;
    sp_phys -= ptrs_bytes;
    memcpy_proc((u8 *)sp_phys, str_ptrs, ptrs_bytes);
    uintptr_t argv_user_va = sp_va;   /* this is x1 in crt0 */

    /* Phase 3: 16-byte align SP */
    sp_va   &= ~15UL;
    sp_phys  = sp_va + delta;

    kinfo("Process: argv_user_va=%p  adjusted_sp=%p\n",
          (void *)argv_user_va, (void *)sp_va);

    int rc = task_create_isolated(
        (uintptr_t)ehdr->e_entry, sp_va,
        path, user_task_trampoline,
        proc_l1, ppid,
        code_phys,  code_pages,
        stack_phys, SPAWN_STACK_PAGES,
        argc, argv_user_va,
        child_pid_out);

    if (rc != 0) {
        kerror("Process: task_create_isolated(args) failed\n");
        goto args_fail_map;
    }

    kinfo("Process: child(args) spawned pid=%lu\n",
          child_pid_out ? (unsigned long)*child_pid_out : 0);
    return 0;

args_fail_map:
    vmm_free_process_pt(proc_l1);
args_fail_pt:
    for (u32 i = 0; i < SPAWN_STACK_PAGES; i++)
        pmm_free_page(stack_phys + (uintptr_t)i * PMM_PAGE_SIZE);
    for (u32 i = 0; i < code_pages; i++)
        pmm_free_page(code_phys  + (uintptr_t)i * PMM_PAGE_SIZE);
    return -1;
}
