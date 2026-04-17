#ifndef AETHER_KMALLOC_H
#define AETHER_KMALLOC_H

/*
 * AetherOS — Kernel Heap Allocator (kmalloc / kfree)
 *
 * Provides dynamic memory allocation within the kernel — like malloc()
 * but for kernel code, backed by physical pages from the PMM.
 *
 * Implementation: first-fit free-list allocator
 *   - Heap starts just after the kernel image in RAM
 *   - Grows by requesting new pages from the PMM
 *   - Each allocation has a small header (16 bytes) prepended
 *   - Adjacent free blocks are coalesced on kfree()
 *
 * This is not a high-performance allocator. It is correct, simple, and
 * easy to understand. A slab allocator (for fixed-size objects) will be
 * added in Phase 3 when we need per-object caches (tasks, file handles, etc.)
 */

#include "aether/types.h"

void  kmalloc_init(void);
void *kmalloc(size_t size);
void  kfree(void *ptr);

/* Zero-initialised allocation (like calloc(1, size)) */
void *kzalloc(size_t size);

/* Diagnostic: print heap statistics */
void kmalloc_print_stats(void);

#endif /* AETHER_KMALLOC_H */
