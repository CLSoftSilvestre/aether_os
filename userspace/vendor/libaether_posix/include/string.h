#ifndef _POSIX_STRING_H
#define _POSIX_STRING_H

#include <stddef.h>

/* Memory */
void  *memcpy(void *dst, const void *src, size_t n);
void  *memmove(void *dst, const void *src, size_t n);
void  *memset(void *s, int c, size_t n);
int    memcmp(const void *a, const void *b, size_t n);
void  *memchr(const void *s, int c, size_t n);

/* String length */
size_t strlen(const char *s);
size_t strnlen(const char *s, size_t maxlen);

/* String copy */
char  *strcpy(char *dst, const char *src);
char  *strncpy(char *dst, const char *src, size_t n);
char  *stpcpy(char *dst, const char *src);

/* String concatenation */
char  *strcat(char *dst, const char *src);
char  *strncat(char *dst, const char *src, size_t n);

/* String comparison */
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);
int    strcasecmp(const char *a, const char *b);
int    strncasecmp(const char *a, const char *b, size_t n);
int    strcoll(const char *a, const char *b);
size_t strxfrm(char *dst, const char *src, size_t n);

/* String searching */
char  *strchr(const char *s, int c);
char  *strrchr(const char *s, int c);
char  *strstr(const char *haystack, const char *needle);
char  *strcasestr(const char *haystack, const char *needle);
char  *strpbrk(const char *s, const char *accept);
size_t strspn(const char *s, const char *accept);
size_t strcspn(const char *s, const char *reject);
char  *strtok(char *s, const char *delim);
char  *strtok_r(char *s, const char *delim, char **saveptr);

/* String duplication (use malloc) */
char  *strdup(const char *s);
char  *strndup(const char *s, size_t n);

/* Error strings */
char  *strerror(int errnum);

#endif /* _POSIX_STRING_H */
