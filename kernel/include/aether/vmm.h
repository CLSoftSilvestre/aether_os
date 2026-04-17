#ifndef AETHER_VMM_H
#define AETHER_VMM_H

/*
 * AetherOS — Virtual Memory Manager (VMM) — Phase 3 MVP
 *
 * This module sets up AArch64 page tables and enables the MMU.
 *
 * ═══════════════════════════════════════════════════════════
 * Why do we need the MMU?
 * ═══════════════════════════════════════════════════════════
 * Without the MMU:
 *   - No memory protection: a bug in user code can overwrite kernel data
 *   - Caches are disabled: every memory access hits DRAM (~100 cycles)
 *   - Can't give each process its own virtual address space
 *
 * With the MMU enabled:
 *   - The CPU translates virtual addresses → physical addresses via page tables
 *   - We control who can read/write/execute each page (AP bits)
 *   - Data and instruction caches become active (10–100× faster)
 *
 * ═══════════════════════════════════════════════════════════
 * Phase 3 MVP: Identity Map
 * ═══════════════════════════════════════════════════════════
 * Identity map = virtual address == physical address.
 * This is the simplest possible mapping: nothing appears to change
 * from the kernel's perspective — existing pointers still work.
 *
 * Memory layout (QEMU virt, -m 1G):
 *
 *   VA range              PA range        Type
 *   0x00000000–0x3FFFFFFF same            Device (MMIO) — EL1 only
 *   0x40000000–0x7FFFFFFF same            Normal RAM — EL0+EL1 RW
 *
 * We use 1GB block descriptors (level-1 table, T0SZ=32) so the entire
 * 4GB address space is covered with just 4 page table entries.
 *
 * ═══════════════════════════════════════════════════════════
 * Phase 3.1 (later): separate kernel/user address spaces
 * ═══════════════════════════════════════════════════════════
 * TTBR0 (VA[63:48]==0x0000) → user space page tables per process
 * TTBR1 (VA[63:48]==0xFFFF) → shared kernel page tables
 * This gives each process its own 256TB virtual space.
 * Memory protection: AP=00 for kernel pages (EL0 no access).
 */

#include "aether/types.h"

/* ── Page table descriptor attribute bits ────────────────────────────── */

/* Descriptor type (bits [1:0]) */
#define PTE_TYPE_BLOCK   0x1UL    /* L1/L2 block (1GB or 2MB) */
#define PTE_TYPE_TABLE   0x3UL    /* L0/L1/L2 next-level table pointer */
#define PTE_TYPE_PAGE    0x3UL    /* L3 page descriptor */

/* Access Flag — MUST be 1 or the CPU raises an Access Flag Fault */
#define PTE_AF           (1UL << 10)

/* Shareability (bits [9:8]) */
#define PTE_SH_NONE      (0UL << 8)   /* non-shareable */
#define PTE_SH_OUTER     (2UL << 8)   /* outer shareable */
#define PTE_SH_INNER     (3UL << 8)   /* inner shareable */

/* Access permissions (bits [7:6]) */
#define PTE_AP_EL1_RW    (0UL << 6)   /* EL1 read/write, EL0 no access */
#define PTE_AP_BOTH_RW   (1UL << 6)   /* EL1 + EL0 read/write */
#define PTE_AP_EL1_RO    (2UL << 6)   /* EL1 read-only, EL0 no access */
#define PTE_AP_BOTH_RO   (3UL << 6)   /* EL1 + EL0 read-only */

/* Execute-never bits */
#define PTE_UXN          (1UL << 54)  /* EL0 cannot execute */
#define PTE_PXN          (1UL << 53)  /* EL1 cannot execute */

/* Memory Attribute Index (bits [4:2]) — indexes into MAIR_EL1 */
#define PTE_ATTR_NORMAL  (0UL << 2)   /* Attr0: Normal WB cacheable */
#define PTE_ATTR_DEVICE  (1UL << 2)   /* Attr1: Device-nGnRnE */

/* ── MAIR_EL1 encoding ───────────────────────────────────────────────── */
/*
 * MAIR_EL1 holds up to 8 memory attribute descriptors (1 byte each).
 * We use only two:
 *   Attr0 = 0xFF = Normal memory, write-back, read/write allocate
 *   Attr1 = 0x00 = Device-nGnRnE (no gathering, no reorder, no early write)
 */
#define MAIR_ATTR_NORMAL_WB  0xFFUL   /* Attr0 */
#define MAIR_ATTR_DEVICE     0x00UL   /* Attr1 */
#define MAIR_EL1_VAL  ((MAIR_ATTR_DEVICE << 8) | MAIR_ATTR_NORMAL_WB)

/* ── TCR_EL1 value ───────────────────────────────────────────────────── */
/*
 * TCR_EL1 — Translation Control Register
 *
 *  T0SZ [5:0]   = 32 → TTBR0 covers 2^(64-32) = 4GB (32-bit VA space)
 *  IRGN0 [9:8]  = 01 → Inner cache: write-back, write-allocate
 *  ORGN0 [11:10]= 01 → Outer cache: write-back, write-allocate
 *  SH0 [13:12]  = 11 → Inner shareable
 *  TG0 [15:14]  = 00 → 4KB granule for TTBR0
 *  T1SZ [21:16] = 32 → TTBR1 covers 4GB
 *  IRGN1 [25:24]= 01, ORGN1 [27:26]= 01, SH1 [29:28]= 11 (same as TTBR0)
 *  TG1 [31:30]  = 10 → 4KB granule for TTBR1
 *  IPS [34:32]  = 010 → 40-bit physical address space
 */
#define TCR_T0SZ    (32UL  <<  0)
#define TCR_IRGN0   (1UL   <<  8)
#define TCR_ORGN0   (1UL   << 10)
#define TCR_SH0     (3UL   << 12)
#define TCR_TG0_4K  (0UL   << 14)
#define TCR_T1SZ    (32UL  << 16)
#define TCR_IRGN1   (1UL   << 24)
#define TCR_ORGN1   (1UL   << 26)
#define TCR_SH1     (3UL   << 28)
#define TCR_TG1_4K  (2UL   << 30)   /* note: TG1 value 10 = 4KB */
#define TCR_IPS_40  (2UL   << 32)   /* 40-bit PA */

#define TCR_EL1_VAL (TCR_T0SZ | TCR_IRGN0 | TCR_ORGN0 | TCR_SH0 | TCR_TG0_4K | \
                     TCR_T1SZ | TCR_IRGN1 | TCR_ORGN1 | TCR_SH1 | TCR_TG1_4K | \
                     TCR_IPS_40)

/* ── SCTLR_EL1 bits ──────────────────────────────────────────────────── */
#define SCTLR_M      (1UL <<  0)   /* MMU enable */
#define SCTLR_C      (1UL <<  2)   /* Data cache enable */
#define SCTLR_I      (1UL << 12)   /* Instruction cache enable */

/* ── User-space memory region ────────────────────────────────────────── */
/*
 * Phase 3.1: the top 256MB of RAM (L2[384-511] = 0x70000000–0x7FFFFFFF)
 * is mapped AP=BOTH_RW so EL0 can read, write, and execute there.
 *
 * User ELF programs are linked at VMM_USER_BASE (0x70000000).
 * The user stack grows down from VMM_USER_STACK_TOP (0x7FFFF000).
 *
 *   0x70000000 – 0x7FEFFFFF  (L2[384-510])  user code/data  (AP=BOTH_RW)
 *   0x7FF00000 – 0x7FFFFFFF  (L2[511])      user stack      (AP=BOTH_RW)
 *
 * VMM_USER_L2_START is the L2 index where the user region begins.
 * L2[i] covers PA = 0x40000000 + i * 0x200000.
 * L2[384]: 0x40000000 + 384 * 0x200000 = 0x70000000.
 */
#define VMM_USER_L2_START   384UL          /* L2 index where user region begins */
#define VMM_USER_BASE       0x70000000UL   /* start of user virtual address space */
#define VMM_USER_STACK_TOP  0x7FFFF000UL   /* EL0 SP_EL0 initial value           */

/* ── Public API ──────────────────────────────────────────────────────── */

/*
 * vmm_init — set up page tables and enable the MMU.
 *
 * After this returns, all memory accesses go through the page tables.
 * Identity mapping means existing pointers remain valid.
 * Data and instruction caches are enabled.
 */
void vmm_init(void);

/*
 * launch_el0 — transfer execution to user space.
 *
 * Sets up ELR_EL1, SPSR_EL1, SP_EL0 and executes `eret`.
 * Does not return — the process communicates back via syscalls.
 *
 * entry:       virtual address of user entry function
 * user_stack:  initial SP_EL0 value (top of user stack)
 */
__attribute__((noreturn))
void launch_el0(uintptr_t entry, uintptr_t user_stack);

#endif /* AETHER_VMM_H */
