/*
 * libaether_posix/memory.c — free-list heap allocator
 *
 * Provides a proper malloc/free/calloc/realloc backed by an 8 MB static
 * BSS pool.  Unlike the bump allocator in libaether/stdlib.c, this one
 * actually reclaims freed blocks so NetSurf / QuickJS can reuse memory.
 *
 * Block layout (each block starts on a 16-byte boundary):
 *
 *   ┌────────────────┬────────────────┬──────────────────────────┐
 *   │ size (size_t)  │ free (size_t)  │   user data …            │
 *   └────────────────┴────────────────┴──────────────────────────┘
 *   ◄──── HDR_SZ ───►
 *
 * size = number of user-data bytes in this block (not counting header)
 * free = 1 if free, 0 if allocated
 *
 * Blocks are stored contiguously.  Iterating from _pool[0] by advancing
 * (hdr + 1 block-worth of data = HDR_SZ + size bytes) walks every block.
 */

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>

#define POOL_SIZE   (8u * 1024u * 1024u)   /* 8 MB */
#define ALIGN       16u
#define HDR_SZ      (2u * sizeof(size_t))  /* 16 bytes on AArch64 */
#define MIN_SPLIT   (HDR_SZ + ALIGN)       /* don't split if remainder < this */

static unsigned char _pool[POOL_SIZE] __attribute__((aligned(ALIGN)));
static int _initialized = 0;

typedef struct {
    size_t size;   /* user-data bytes */
    size_t free;   /* 1 = free, 0 = used */
} hdr_t;

static inline hdr_t *block_at(void *p)   { return (hdr_t *)p; }
static inline hdr_t *next_block(hdr_t *h){ return (hdr_t *)((char *)h + HDR_SZ + h->size); }
static inline void  *user_ptr(hdr_t *h)  { return (char *)h + HDR_SZ; }
static inline hdr_t *hdr_of(void *p)     { return (hdr_t *)((char *)p - HDR_SZ); }

static void _init(void)
{
    hdr_t *first = block_at(_pool);
    first->size = POOL_SIZE - HDR_SZ;
    first->free = 1;
    _initialized = 1;
}

static void _coalesce(void)
{
    hdr_t *h = block_at(_pool);
    hdr_t *end = block_at(_pool + POOL_SIZE);
    while (h < end) {
        hdr_t *n = next_block(h);
        if (h->free && n < end && n->free) {
            /* merge h and n */
            h->size += HDR_SZ + n->size;
            /* re-check from same position */
            continue;
        }
        h = n;
    }
}

void *malloc(size_t size)
{
    if (!size) return NULL;
    if (!_initialized) _init();

    /* Round up to ALIGN boundary */
    size = (size + ALIGN - 1u) & ~(ALIGN - 1u);

    hdr_t *h   = block_at(_pool);
    hdr_t *end = block_at(_pool + POOL_SIZE);

    while (h < end) {
        if (h->free && h->size >= size) {
            /* Can we split? */
            if (h->size >= size + MIN_SPLIT) {
                hdr_t *split = (hdr_t *)((char *)h + HDR_SZ + size);
                split->size = h->size - size - HDR_SZ;
                split->free = 1;
                h->size = size;
            }
            h->free = 0;
            return user_ptr(h);
        }
        h = next_block(h);
    }

    /* Retry after coalescing adjacent free blocks */
    _coalesce();
    h = block_at(_pool);
    while (h < end) {
        if (h->free && h->size >= size) {
            if (h->size >= size + MIN_SPLIT) {
                hdr_t *split = (hdr_t *)((char *)h + HDR_SZ + size);
                split->size = h->size - size - HDR_SZ;
                split->free = 1;
                h->size = size;
            }
            h->free = 0;
            return user_ptr(h);
        }
        h = next_block(h);
    }

    errno = ENOMEM;
    return NULL;
}

void free(void *ptr)
{
    if (!ptr) return;
    hdr_t *h = hdr_of(ptr);
    h->free = 1;
    /* Lazy coalescing: done in malloc when needed */
}

void *calloc(size_t nmemb, size_t size)
{
    size_t total = nmemb * size;
    if (nmemb && total / nmemb != size) { errno = ENOMEM; return NULL; }
    void *p = malloc(total);
    if (p) {
        /* Zero the block — memset from string.h */
        unsigned char *b = p;
        for (size_t i = 0; i < total; i++) b[i] = 0;
    }
    return p;
}

void *realloc(void *ptr, size_t size)
{
    if (!ptr)   return malloc(size);
    if (!size)  { free(ptr); return NULL; }

    hdr_t *h = hdr_of(ptr);
    size_t new_size = (size + ALIGN - 1u) & ~(ALIGN - 1u);

    if (h->size >= new_size) {
        /* Current block is big enough — shrink in place if worthwhile */
        if (h->size >= new_size + MIN_SPLIT) {
            hdr_t *split = (hdr_t *)((char *)h + HDR_SZ + new_size);
            split->size = h->size - new_size - HDR_SZ;
            split->free = 1;
            h->size = new_size;
        }
        return ptr;
    }

    void *np = malloc(size);
    if (!np) return NULL;

    /* Copy old data */
    size_t copy = h->size < size ? h->size : size;
    unsigned char *s = (unsigned char *)ptr;
    unsigned char *d = (unsigned char *)np;
    for (size_t i = 0; i < copy; i++) d[i] = s[i];

    free(ptr);
    return np;
}

/* ── Minimal heap stats (for debugging) ──────────────────────────────── */

void posix_heap_stats(size_t *used_out, size_t *free_out)
{
    if (!_initialized) _init();
    size_t used = 0, freeb = 0;
    hdr_t *h   = block_at(_pool);
    hdr_t *end = block_at(_pool + POOL_SIZE);
    while (h < end) {
        if (h->free) freeb += h->size;
        else         used  += h->size;
        h = next_block(h);
    }
    if (used_out)  *used_out  = used;
    if (free_out)  *free_out  = freeb;
}
