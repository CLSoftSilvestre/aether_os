#ifndef _STDIO_H
#define _STDIO_H

#include <stdarg.h>
#include <stddef.h>

int  printf(const char *fmt, ...);
int  snprintf(char *buf, size_t size, const char *fmt, ...);
int  vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
int  putchar(int c);
int  puts(const char *s);

/* Read a line from stdin (blocking). Returns number of chars read (excl NUL). */
int  readline(char *buf, int max);

#endif /* _STDIO_H */
