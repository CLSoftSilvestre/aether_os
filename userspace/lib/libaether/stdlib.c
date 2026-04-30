#include <stdlib.h>
#include <sys.h>

/* ── Bump allocator ──────────────────────────────────────────────────── */

/*
 * Simple bump allocator backed by a static heap.
 * free() is a no-op — memory is never reclaimed.
 * 256KB covers the text editor's textarea line buffer (1024 × 128 B = 128 KB)
 * plus future widget-heavy apps (Lua IDE, etc.).
 */
#define HEAP_SIZE  (256 * 1024)

static unsigned char _heap[HEAP_SIZE];
static size_t        _heap_pos;

void *malloc(size_t size)
{
    /* 8-byte align */
    size = (size + 7u) & ~7u;
    if (_heap_pos + size > HEAP_SIZE) return NULL;
    void *ptr = &_heap[_heap_pos];
    _heap_pos += size;
    return ptr;
}

void free(void *ptr)
{
    (void)ptr;   /* bump allocator — no-op */
}

void *realloc(void *ptr, size_t size)
{
    if (!size) return NULL;
    void *n = malloc(size);
    if (!n) return NULL;
    if (ptr) {
        /* Copy old data — we don't know the old size, so copy up to 'size'.
           This is safe: the bump allocator never unmaps old blocks. */
        unsigned char *s = (unsigned char *)ptr;
        unsigned char *d = (unsigned char *)n;
        for (size_t i = 0; i < size; i++) d[i] = s[i];
    }
    return n;
}

/* ── atoi / atol ─────────────────────────────────────────────────────── */

long atol(const char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    long sign = 1;
    if (*s == '-') { sign = -1; s++; }
    else if (*s == '+') s++;
    long v = 0;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s++ - '0'); }
    return sign * v;
}

int atoi(const char *s)
{
    return (int)atol(s);
}

int abs(int x)   { return x < 0 ? -x : x; }
long labs(long x){ return x < 0 ? -x : x; }

/* ── exit ────────────────────────────────────────────────────────────── */

__attribute__((noreturn))
void exit(int code)
{
    sys_exit(code);
}
