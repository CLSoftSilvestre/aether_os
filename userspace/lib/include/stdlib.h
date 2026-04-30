#ifndef _STDLIB_H
#define _STDLIB_H

#include <stddef.h>

void  *malloc(size_t size);
void  *realloc(void *ptr, size_t size); /* allocates new block + copies; old ptr not freed */
void   free(void *ptr);    /* no-op in bump allocator */
int    atoi(const char *s);
long   atol(const char *s);
int    abs(int x);
long   labs(long x);

__attribute__((noreturn))
void   exit(int code);

#endif /* _STDLIB_H */
