#include <stdlib.h>
#include <sys.h>

/* ── Bump allocator ──────────────────────────────────────────────────── */

/*
 * Simple bump allocator backed by a static heap.
 * free() is a no-op — memory is never reclaimed.
 * 64KB is more than enough for the aesh shell.
 */
#define HEAP_SIZE  (64 * 1024)

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

/* ── exit ────────────────────────────────────────────────────────────── */

__attribute__((noreturn))
void exit(int code)
{
    sys_exit(code);
}
