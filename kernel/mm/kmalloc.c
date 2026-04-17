/*
 * AetherOS — Kernel Heap Allocator
 * File: kernel/mm/kmalloc.c
 *
 * First-fit free-list allocator. The heap is a contiguous region of
 * physical memory (obtained page-by-page from the PMM) laid out as:
 *
 *   ┌──────────┬──────────────┬──────────┬──────────────┬─── ...
 *   │  header  │  user data   │  header  │  user data   │
 *   │  16 bytes│  size bytes  │  16 bytes│  size bytes  │
 *   └──────────┴──────────────┴──────────┴──────────────┴─── ...
 *
 * A "header" (block_header_t) precedes every allocation. The user
 * receives a pointer to the data region immediately after the header.
 * kmalloc returns ptr-to-data; kfree receives that same pointer.
 *
 * When the heap runs out of space, kmalloc requests a new page from the
 * PMM and extends the heap region.
 *
 * Alignment: all allocations are 16-byte aligned (AArch64 ABI requirement
 * for stack frames; useful for SIMD even though we avoid SIMD in the kernel).
 */

#include "aether/kmalloc.h"
#include "aether/mm.h"
#include "aether/printk.h"
#include "aether/types.h"

/* Allocation alignment (AArch64 ABI: stack must be 16-byte aligned) */
#define HEAP_ALIGN       16
#define ALIGN_UP(n, a)   (((n) + (a) - 1) & ~((a) - 1))

/* Minimum allocation: enough for a useful object */
#define MIN_ALLOC_SIZE   16

/*
 * block_header_t — prepended to every allocation.
 * Exactly 16 bytes: magic(4) + size_flags(4) + next(8).
 *
 * The is_free flag is encoded in bit 31 of size_flags to keep the
 * struct 16 bytes without padding — this keeps all allocations
 * 16-byte aligned as long as the first header is 16-byte aligned.
 *
 * bit 31 of size_flags = 1 → free, 0 → used
 * bits 30:0 of size_flags = data size in bytes
 */
typedef struct block_header {
    u32                 magic;       /* 0xA57E1234 — detects corruption */
    u32                 size_flags;  /* bits[30:0]=size, bit[31]=is_free */
    struct block_header *next;       /* next block in the heap chain     */
} block_header_t;

_Static_assert(sizeof(block_header_t) == 16, "block_header_t must be 16 bytes");

#define HEADER_MAGIC     0xA57E1234u
#define HDR_FREE_FLAG    (1u << 31)
#define HDR_SIZE(h)      ((h)->size_flags & ~HDR_FREE_FLAG)
#define HDR_IS_FREE(h)   ((h)->size_flags &  HDR_FREE_FLAG)
#define HDR_SET_FREE(h)  ((h)->size_flags |=  HDR_FREE_FLAG)
#define HDR_SET_USED(h)  ((h)->size_flags &= ~HDR_FREE_FLAG)
#define HDR_SET_SIZE(h, s) \
    ((h)->size_flags = ((h)->size_flags & HDR_FREE_FLAG) | ((s) & ~HDR_FREE_FLAG))

/* Heap state */
static block_header_t *g_heap_head = NULL;   /* first block */
static block_header_t *g_heap_tail = NULL;   /* last block (for appending) */
static size_t          g_heap_used = 0;
static size_t          g_heap_total = 0;

/* ── Internal helpers ───────────────────────────────────────────────────── */

/* Extend heap by one PMM page, add as a large free block */
static bool heap_expand(void)
{
    uintptr_t page = pmm_alloc_page();
    if (!page) return false;

    block_header_t *blk = (block_header_t *)page;
    blk->magic      = HEADER_MAGIC;
    blk->size_flags = (PMM_PAGE_SIZE - sizeof(block_header_t)) | HDR_FREE_FLAG;
    blk->next       = NULL;

    if (!g_heap_head) {
        g_heap_head = blk;
        g_heap_tail = blk;
    } else {
        g_heap_tail->next = blk;
        g_heap_tail = blk;
    }

    g_heap_total += HDR_SIZE(blk);
    return true;
}

/* Split a block if it's much larger than requested */
static void maybe_split(block_header_t *blk, size_t size)
{
    size_t remaining = HDR_SIZE(blk) - size;
    /* Only split if remainder can hold a header + at least MIN_ALLOC_SIZE */
    if (remaining < sizeof(block_header_t) + MIN_ALLOC_SIZE)
        return;

    /* Carve a new free block from the tail of this one */
    block_header_t *newblk = (block_header_t *)((u8 *)(blk + 1) + size);
    newblk->magic      = HEADER_MAGIC;
    newblk->size_flags = (remaining - sizeof(block_header_t)) | HDR_FREE_FLAG;
    newblk->next       = blk->next;

    HDR_SET_SIZE(blk, size);
    blk->next = newblk;

    if (g_heap_tail == blk)
        g_heap_tail = newblk;
}

/* Coalesce adjacent free blocks (called after kfree) */
static void coalesce(void)
{
    block_header_t *cur = g_heap_head;
    while (cur && cur->next) {
        if (HDR_IS_FREE(cur) && HDR_IS_FREE(cur->next)) {
            /* Merge: absorb next block's header + data into cur */
            u32 merged = HDR_SIZE(cur) + sizeof(block_header_t) + HDR_SIZE(cur->next);
            if (g_heap_tail == cur->next)
                g_heap_tail = cur;
            cur->next = cur->next->next;
            HDR_SET_SIZE(cur, merged);
            /* Don't advance — try to merge again with new next */
        } else {
            cur = cur->next;
        }
    }
}

/* ── Public API ─────────────────────────────────────────────────────────── */

void kmalloc_init(void)
{
    g_heap_head  = NULL;
    g_heap_tail  = NULL;
    g_heap_used  = 0;
    g_heap_total = 0;

    /* Pre-allocate 2 pages so the heap has some initial space */
    heap_expand();
    heap_expand();

    kinfo("kmalloc: heap initialised, %lu KB available\n",
          (unsigned long)(g_heap_total >> 10));
}

void *kmalloc(size_t size)
{
    if (!size) return NULL;

    /* Round up to alignment requirement */
    size = ALIGN_UP(size, HEAP_ALIGN);

    /* First-fit search */
    block_header_t *blk = g_heap_head;
    while (blk) {
        if (HDR_IS_FREE(blk) && HDR_SIZE(blk) >= size) {
            maybe_split(blk, size);
            HDR_SET_USED(blk);
            g_heap_used += HDR_SIZE(blk);
            return (void *)(blk + 1);   /* pointer just past the header */
        }
        blk = blk->next;
    }

    /* No suitable block — expand heap and retry once */
    if (!heap_expand()) {
        kerror("kmalloc: OOM (requested %lu bytes)\n", (unsigned long)size);
        return NULL;
    }

    /* Retry with new page */
    blk = g_heap_tail;   /* newly added page is at the tail */
    if (HDR_IS_FREE(blk) && HDR_SIZE(blk) >= size) {
        maybe_split(blk, size);
        HDR_SET_USED(blk);
        g_heap_used += HDR_SIZE(blk);
        return (void *)(blk + 1);
    }

    kerror("kmalloc: allocation of %lu bytes failed\n", (unsigned long)size);
    return NULL;
}

void kfree(void *ptr)
{
    if (!ptr) return;

    block_header_t *blk = ((block_header_t *)ptr) - 1;

    if (blk->magic != HEADER_MAGIC) {
        kpanic("kfree: corrupted heap at %p (bad magic 0x%x)\n",
               ptr, blk->magic);
    }
    if (HDR_IS_FREE(blk)) {
        kwarn("kfree: double-free detected at %p\n", ptr);
        return;
    }

    g_heap_used -= HDR_SIZE(blk);
    HDR_SET_FREE(blk);
    coalesce();
}

void *kzalloc(size_t size)
{
    void *p = kmalloc(size);
    if (p) {
        u8 *b = (u8 *)p;
        for (size_t i = 0; i < size; i++)
            b[i] = 0;
    }
    return p;
}

void kmalloc_print_stats(void)
{
    kinfo("kmalloc: %lu/%lu bytes used (%lu KB free)\n",
          (unsigned long)g_heap_used,
          (unsigned long)g_heap_total,
          (unsigned long)((g_heap_total - g_heap_used) >> 10));
}
