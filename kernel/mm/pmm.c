/*
 * AetherOS — Physical Memory Manager
 * File: kernel/mm/pmm.c
 *
 * Tracks free/used 4KB pages of physical RAM using a bitmap.
 * Bit 0 of bitmap[0] = page at RAM_START (0x40000000).
 * 0 = free, 1 = used.
 *
 * After pmm_init(), the layout in RAM is:
 *
 *   0x40000000  ┌───────────────────────────────┐
 *               │  Kernel image (.text, .data)  │
 *               │  Kernel BSS  (bitmap here!)   │
 *               │  Boot stack  (64KB)           │
 *   first_free  ├───────────────────────────────┤
 *               │  Available for allocation     │
 *   0x7FFFFFFF  └───────────────────────────────┘
 */

#include "aether/mm.h"
#include "aether/printk.h"
#include "aether/types.h"
/* No string.h in bare-metal — page zeroing is done with inline loops below */

/* The bitmap lives in BSS — zeroed by boot.S before kernel_main.           */
/* 262144 pages / 8 bits per byte = 32768 bytes = 32 KB.                    */
static u8 phys_bitmap[PMM_NUM_PAGES / 8];

/* Statistics */
static u32 g_free_pages = 0;
static u32 g_total_pages = 0;

/* ── Bitmap helpers ─────────────────────────────────────────────────────── */

static inline void bitmap_set(u32 idx)
{
    phys_bitmap[idx / 8] |= (1u << (idx % 8));
}

static inline void bitmap_clear(u32 idx)
{
    phys_bitmap[idx / 8] &= ~(1u << (idx % 8));
}

static inline bool bitmap_test(u32 idx)
{
    return (phys_bitmap[idx / 8] >> (idx % 8)) & 1u;
}

/* ── Public API ─────────────────────────────────────────────────────────── */

/*
 * We need the kernel end symbol from the linker script.
 * __stack_top is the highest address used by the kernel + its boot stack.
 */
extern u8 __stack_top[];

void pmm_init(void)
{
    /*
     * Step 1: Mark all pages as "used" (safe default).
     * We will selectively free the pages that are actually available.
     */
    for (u32 i = 0; i < PMM_NUM_PAGES / 8; i++)
        phys_bitmap[i] = 0xFF;

    g_total_pages = PMM_NUM_PAGES;
    g_free_pages  = 0;

    /*
     * Step 2: Compute first free physical page.
     *
     * The kernel occupies RAM from PMM_RAM_START to __stack_top.
     * We round __stack_top up to the next page boundary to get the
     * address of the first page we can hand out.
     */
    uintptr_t kernel_end_phys = PMM_PAGE_ALIGN((uintptr_t)__stack_top);
    u32 kernel_pages = PMM_ADDR_TO_IDX(kernel_end_phys);

    kinfo("PMM: RAM  %p – %p (%lu MB)\n",
          (void *)PMM_RAM_START,
          (void *)PMM_RAM_END,
          (unsigned long)(PMM_RAM_SIZE >> 20));
    kinfo("PMM: Kernel occupies pages 0 – %lu (up to %p)\n",
          (unsigned long)(kernel_pages - 1),
          (void *)kernel_end_phys);

    /*
     * Step 3: Free all pages above the kernel.
     * Pages 0 .. kernel_pages-1 stay marked as "used" (kernel territory).
     * Pages kernel_pages .. NUM_PAGES-1 are freed.
     */
    for (u32 i = kernel_pages; i < PMM_NUM_PAGES; i++) {
        bitmap_clear(i);
        g_free_pages++;
    }

    kinfo("PMM: %lu free pages (%lu MB available)\n",
          (unsigned long)g_free_pages,
          (unsigned long)(g_free_pages * PMM_PAGE_SIZE >> 20));
}

/*
 * pmm_alloc_page — allocate one free 4KB page.
 *
 * Linear scan: O(N) worst case. Acceptable for now — a buddy allocator
 * would be O(log N) and is planned for Phase 5.
 *
 * Returns physical address, or 0 on OOM.
 */
uintptr_t pmm_alloc_page(void)
{
    for (u32 i = 0; i < PMM_NUM_PAGES; i++) {
        if (!bitmap_test(i)) {
            bitmap_set(i);
            g_free_pages--;

            /* Zero the page before handing it out (security + correctness) */
            uintptr_t addr = PMM_IDX_TO_ADDR(i);
            u64 *p = (u64 *)addr;
            for (u32 j = 0; j < PMM_PAGE_SIZE / 8; j++)
                p[j] = 0;

            return addr;
        }
    }

    kerror("PMM: out of memory!\n");
    return 0;
}

/*
 * pmm_alloc_pages — allocate N physically contiguous pages.
 *
 * Searches for a run of N consecutive free bits.
 * Returns physical address of first page, or 0 on failure.
 */
uintptr_t pmm_alloc_pages(u32 count)
{
    if (count == 0) return 0;
    if (count == 1) return pmm_alloc_page();

    u32 run = 0;
    u32 run_start = 0;

    for (u32 i = 0; i < PMM_NUM_PAGES; i++) {
        if (!bitmap_test(i)) {
            if (run == 0) run_start = i;
            run++;
            if (run == count) {
                /* Found a run — mark all as used and zero them */
                for (u32 j = run_start; j < run_start + count; j++) {
                    bitmap_set(j);
                    g_free_pages--;

                    u64 *p = (u64 *)PMM_IDX_TO_ADDR(j);
                    for (u32 k = 0; k < PMM_PAGE_SIZE / 8; k++)
                        p[k] = 0;
                }
                return PMM_IDX_TO_ADDR(run_start);
            }
        } else {
            run = 0;   /* reset run on any used page */
        }
    }

    kerror("PMM: cannot allocate %lu contiguous pages\n", (unsigned long)count);
    return 0;
}

void pmm_free_page(uintptr_t addr)
{
    if (addr < PMM_RAM_START || addr >= PMM_RAM_END) {
        kwarn("PMM: free of out-of-range address %p\n", (void *)addr);
        return;
    }
    if (addr & (PMM_PAGE_SIZE - 1)) {
        kwarn("PMM: free of unaligned address %p\n", (void *)addr);
        return;
    }

    u32 idx = PMM_ADDR_TO_IDX(addr);
    if (!bitmap_test(idx)) {
        kwarn("PMM: double-free of page %p\n", (void *)addr);
        return;
    }

    bitmap_clear(idx);
    g_free_pages++;
}

void pmm_print_stats(void)
{
    kinfo("PMM stats: %lu/%lu pages free (%lu/%lu MB)\n",
          (unsigned long)g_free_pages,
          (unsigned long)g_total_pages,
          (unsigned long)(g_free_pages * PMM_PAGE_SIZE >> 20),
          (unsigned long)(g_total_pages * PMM_PAGE_SIZE >> 20));
}

u32 pmm_total_pages(void) { return g_total_pages; }
u32 pmm_free_pages(void)  { return g_free_pages;  }
