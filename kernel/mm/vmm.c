/*
 * AetherOS — Virtual Memory Manager / MMU Setup
 * File: kernel/mm/vmm.c
 *
 * Sets up AArch64 page tables and enables the MMU with data and
 * instruction caches. After vmm_init() returns:
 *   - All memory accesses go through page tables
 *   - Identity map: VA == PA (existing pointers stay valid)
 *   - D-cache and I-cache are on (significant speedup)
 *   - EL0 (user mode) can access RAM; EL0 cannot access MMIO
 *
 * ═══════════════════════════════════════════════════════════
 * Two-level page table layout (T0SZ=32, 4KB granule):
 * ═══════════════════════════════════════════════════════════
 *
 *   Virtual address (32-bit):
 *   ┌─────────────┬──────────────┬─────────────────────────┐
 *   │ bits[31:30] │ bits[29:21]  │ bits[20:0]              │
 *   │  L1 index   │  L2 index    │  block offset (2MB)     │
 *   └─────────────┴──────────────┴─────────────────────────┘
 *
 *   L1 table (4 entries, one per GB):
 *     [0] → 1GB BLOCK at 0x00000000 (MMIO, device memory, EL1 only)
 *     [1] → TABLE → l2_table_ram (RAM mapped as 2MB blocks)
 *     [2] → invalid
 *     [3] → invalid
 *
 *   l2_table_ram (512 entries, each covers 2MB):
 *     [0]   → 2MB block at 0x40000000 (AP=BOTH_RW, Normal WB)
 *     [1]   → 2MB block at 0x40200000 (AP=BOTH_RW, Normal WB)
 *     ...
 *     [511] → 2MB block at 0x7FE00000 (AP=BOTH_RW, Normal WB)
 *
 * Using 2MB blocks (rather than 1GB) at L2 avoids a QEMU 10.x quirk
 * where AP=BOTH_RW on a 1GB L1 block incorrectly triggers a Permission
 * Fault for EL1 instruction fetches. L2 2MB blocks with the same AP
 * value work correctly on both QEMU and real hardware.
 *
 * TTBR0_EL1 holds the physical address of the L1 table.
 */

#include "aether/vmm.h"
#include "aether/printk.h"
#include "aether/types.h"

/* L1 table: 4 used entries (one per 1GB), must be 4KB-aligned */
static u64 l1_table[512] __attribute__((aligned(4096)));

/*
 * L2 table for the RAM region (0x40000000–0x7FFFFFFF).
 * 512 entries × 2MB = 1GB.  Must be 4KB-aligned.
 */
static u64 l2_table_ram[512] __attribute__((aligned(4096)));

/* ── System register accessors ──────────────────────────────────────────── */

static inline void write_mair_el1(u64 v)
{
    __asm__ volatile("msr MAIR_EL1, %0" :: "r"(v) : "memory");
}

static inline void write_tcr_el1(u64 v)
{
    __asm__ volatile("msr TCR_EL1, %0" :: "r"(v) : "memory");
}

static inline void write_ttbr0_el1(u64 v)
{
    __asm__ volatile("msr TTBR0_EL1, %0" :: "r"(v) : "memory");
}

static inline u64 read_sctlr_el1(void)
{
    u64 v;
    __asm__ volatile("mrs %0, SCTLR_EL1" : "=r"(v));
    return v;
}

static inline void write_sctlr_el1(u64 v)
{
    __asm__ volatile("msr SCTLR_EL1, %0" :: "r"(v) : "memory");
}

/* ── MMU init ─────────────────────────────────────────────────────────── */

void vmm_init(void)
{
    /*
     * Step 1a: Fill the L2 table for RAM (0x40000000–0x7FFFFFFF).
     *
     * 512 × 2MB blocks with identity mapping (VA == PA).
     * All blocks: Normal WB, inner-shareable, EL1+EL0 RW, AF=1.
     *
     * 2MB block descriptor bit layout:
     *   [1:0]   = 01 → block descriptor
     *   [4:2]   = AttrIdx (0 = Normal WB)
     *   [7:6]   = AP[2:1] (01 = EL1+EL0 RW)
     *   [9:8]   = SH (11 = inner shareable)
     *   [10]    = AF (access flag, must be 1)
     *   [47:21] = OA (output address, 2MB-aligned)
     *   [53]    = PXN (0 = EL1 may execute)
     *   [54]    = UXN (0 = EL0 may execute)
     */
    for (u32 i = 0; i < 512; i++) {
        uintptr_t pa = 0x40000000UL + (uintptr_t)i * 0x200000UL;

        if (i >= VMM_USER_L2_START) {
            /*
             * User region (L2[384-511] = 0x70000000–0x7FFFFFFF, 256MB).
             *
             * AP=BOTH_RW: EL0 and EL1 can read/write.
             * UXN=0:      EL0 may execute (user code and stack live here).
             * PXN=1:      EL1 should not execute from user pages.
             *
             * Layout:
             *   0x70000000–0x7FEFFFFF  user code and data (ELF loads here)
             *   0x7FF00000–0x7FFFFFFF  user stack (SP_EL0 starts at 0x7FFFF000)
             */
            l2_table_ram[i] = pa
                            | PTE_ATTR_NORMAL
                            | PTE_AP_BOTH_RW  /* EL0+EL1 RW              */
                            | PTE_SH_INNER
                            | PTE_AF
                            | PTE_PXN         /* EL1 execute-never       */
                            | PTE_TYPE_BLOCK;
        } else {
            /*
             * Kernel region (L2[0-383] = 0x40000000–0x6FFFFFFF, 768MB).
             *
             * AP=EL1_RW: only EL1 can read/write. EL0 is blocked.
             * UXN=1: EL0 may not execute kernel pages.
             * PXN=0: EL1 may execute (kernel code lives here).
             */
            l2_table_ram[i] = pa
                            | PTE_ATTR_NORMAL
                            | PTE_AP_EL1_RW   /* EL1 only                */
                            | PTE_SH_INNER
                            | PTE_AF
                            | PTE_UXN         /* EL0 execute-never       */
                            | PTE_TYPE_BLOCK;
        }
    }

    /*
     * Step 1b: Build the L1 table.
     *
     * Entry 0: 1GB BLOCK for MMIO (0x00000000–0x3FFFFFFF).
     *   Device memory, EL1-only, non-shareable.
     *   A 1GB L1 block is fine here because this region is
     *   EL1_RW — not affected by the AP=BOTH_RW QEMU quirk.
     *
     * Entry 1: TABLE descriptor → l2_table_ram for RAM.
     *   This delegates permission control to the 2MB L2 entries.
     *
     * Entries 2–3: invalid (bit 0 = 0 → descriptor is invalid).
     */
    l1_table[0] = 0x00000000UL
                | PTE_ATTR_DEVICE    /* Device-nGnRnE               */
                | PTE_AP_EL1_RW     /* EL1 RW, EL0 no access       */
                | PTE_SH_NONE       /* non-shareable                */
                | PTE_AF            /* access flag                  */
                | PTE_TYPE_BLOCK;   /* 1GB block descriptor         */

    l1_table[1] = (uintptr_t)l2_table_ram
                | PTE_TYPE_TABLE;   /* L1 → L2 table descriptor     */

    l1_table[2] = 0;
    l1_table[3] = 0;

    kinfo("VMM: L1 table at %p  L2-RAM at %p\n",
          (void *)l1_table, (void *)l2_table_ram);
    kinfo("VMM: [0] MMIO  0x00000000–0x3FFFFFFF  → 1GB block (device, EL1)\n");
    kinfo("VMM: [1] RAM   0x40000000–0x6FFFFFFF  → L2[0-383]   768MB kernel (EL1)\n");
    kinfo("VMM: [1] RAM   0x70000000–0x7FFFFFFF  → L2[384-511] 256MB user (EL0+EL1)\n");

    /*
     * Step 2: Configure MAIR_EL1.
     *   Attr0 = 0xFF = Normal WB, read/write-allocate
     *   Attr1 = 0x00 = Device-nGnRnE
     */
    write_mair_el1(MAIR_EL1_VAL);

    /*
     * Step 3: Configure TCR_EL1.
     *   T0SZ=32  → 4GB address space for TTBR0
     *   TG0=4KB  → 4KB granule
     *   IPS=40   → 40-bit physical address
     *   Inner/outer WB cache for table walks, inner-shareable
     */
    write_tcr_el1(TCR_EL1_VAL);

    /*
     * Step 4: Load the L1 table address into TTBR0_EL1.
     */
    write_ttbr0_el1((uintptr_t)l1_table);

    /*
     * Step 5: Barriers before enabling MMU.
     */
    __asm__ volatile(
        "dsb sy\n"
        "isb\n"
        "tlbi vmalle1\n"
        "dsb sy\n"
        "isb\n"
        ::: "memory"
    );

    /*
     * Step 6: Enable the MMU (M), D-cache (C), I-cache (I).
     *
     * After the isb, all instruction fetches go through the L1/L2 tables.
     * Identity mapping means the kernel continues executing at the same PA.
     */
    u64 sctlr = read_sctlr_el1();
    sctlr |= SCTLR_M | SCTLR_C | SCTLR_I;
    write_sctlr_el1(sctlr);
    __asm__ volatile("isb" ::: "memory");

    kinfo("VMM: MMU enabled — caches on\n");
    kinfo("VMM: TCR_EL1=0x%lx  MAIR=0x%lx\n",
          (unsigned long)TCR_EL1_VAL,
          (unsigned long)MAIR_EL1_VAL);
}

/* ── EL0 launch ──────────────────────────────────────────────────────── */

/*
 * launch_el0 — jump from EL1 (kernel) to EL0 (user mode).
 *
 * AArch64 privilege is changed via `eret`:
 *   - ELR_EL1  = where to jump (user entry address)
 *   - SPSR_EL1 = what state to restore (EL0, interrupts enabled)
 *   - SP_EL0   = user stack pointer
 *   - eret executes: PC ← ELR_EL1, PSTATE ← SPSR_EL1
 */
__attribute__((noreturn))
void launch_el0(uintptr_t entry, uintptr_t user_stack)
{
    kinfo("VMM: launching EL0 process — entry=%p stack=%p\n",
          (void *)entry, (void *)user_stack);

    __asm__ volatile(
        "msr ELR_EL1,  %0\n"
        "msr SPSR_EL1, %1\n"   /* EL0t, all interrupts unmasked */
        "msr SP_EL0,   %2\n"
        "eret\n"
        :
        : "r"(entry), "r"(0UL), "r"(user_stack)
        : "memory"
    );

    __builtin_unreachable();
}
