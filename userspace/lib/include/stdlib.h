#ifndef _STDLIB_H
#define _STDLIB_H

#include <stddef.h>

void  *malloc(size_t size);
void   free(void *ptr);    /* no-op in bump allocator */
int    atoi(const char *s);
long   atol(const char *s);

__attribute__((noreturn))
void   exit(int code);

#endif /* _STDLIB_H */
