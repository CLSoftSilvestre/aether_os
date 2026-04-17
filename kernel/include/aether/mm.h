#ifndef AETHER_MM_H
#define AETHER_MM_H

/*
 * AetherOS — Physical Memory Manager (PMM)
 *
 * The PMM tracks which 4KB pages of physical RAM are free or in use.
 * It is the foundation of all memory allocation — kmalloc, stacks,
 * page tables, and future user-space mappings all come from here.
 *
 * Implementation: bitmap allocator
 *   - 1 bit per 4KB page (0 = free, 1 = used)
 *   - Covers RAM region 0x40000000 – 0x7FFFFFFF (1GB on QEMU -m 1G)
 *   - 262144 pages → 32768-byte bitmap (32KB in BSS)
 *
 * Why a bitmap? Simple, compact, and easy to reason about. A buddy
 * allocator is faster for large allocations but more complex — we will
 * add that in Phase 5 if performance becomes an issue.
 *
 * Analogous to x86 "e820 map" processing + page frame allocation.
 */

#include "aether/types.h"

/* Physical memory layout (QEMU virt machine, -m 1G) */
#define PMM_RAM_START   0x40000000UL    /* First byte of RAM */
#define PMM_RAM_SIZE    0x40000000UL    /* 1 GB */
#define PMM_RAM_END     (PMM_RAM_START + PMM_RAM_SIZE)
#define PMM_PAGE_SIZE   0x1000UL        /* 4 KB */
#define PMM_NUM_PAGES   (PMM_RAM_SIZE / PMM_PAGE_SIZE)  /* 262144 */

/* Round address up to next page boundary */
#define PMM_PAGE_ALIGN(addr)  (((addr) + PMM_PAGE_SIZE - 1) & ~(PMM_PAGE_SIZE - 1))

/* Convert physical address ↔ page index */
#define PMM_ADDR_TO_IDX(addr)  (((addr) - PMM_RAM_START) / PMM_PAGE_SIZE)
#define PMM_IDX_TO_ADDR(idx)   (PMM_RAM_START + (idx) * PMM_PAGE_SIZE)

/* ── Public API ───────────────────────────────────────────────────────── */

/*
 * pmm_init — discover free physical memory and build the bitmap.
 * Must be called before any pmm_alloc_*() call.
 */
void pmm_init(void);

/*
 * pmm_alloc_page — allocate one free 4KB page.
 * Returns physical address of the page, or 0 on failure.
 *
 * The returned page is guaranteed to be zeroed.
 */
uintptr_t pmm_alloc_page(void);

/*
 * pmm_alloc_pages — allocate N physically contiguous 4KB pages.
 * Returns physical address of the first page, or 0 on failure.
 * Returned region is zeroed.
 */
uintptr_t pmm_alloc_pages(u32 count);

/*
 * pmm_free_page — return a page to the free pool.
 * addr must be page-aligned and previously allocated.
 */
void pmm_free_page(uintptr_t addr);

/* Diagnostic: print free/used page counts */
void pmm_print_stats(void);

/* Total and free page counts */
u32 pmm_total_pages(void);
u32 pmm_free_pages(void);

#endif /* AETHER_MM_H */
