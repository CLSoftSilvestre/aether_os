#include <string.h>

size_t strlen(const char *s)
{
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
    while (n-- && *a && *a == *b) { a++; b++; }
    if (n == (size_t)-1) return 0;
    return (unsigned char)*a - (unsigned char)*b;
}

char *strcpy(char *dst, const char *src)
{
    char *d = dst;
    while ((*d++ = *src++));
    return dst;
}

char *strncpy(char *dst, const char *src, size_t n)
{
    size_t i;
    for (i = 0; i < n && src[i]; i++) dst[i] = src[i];
    for (; i < n; i++) dst[i] = '\0';
    return dst;
}

char *strcat(char *dst, const char *src)
{
    char *d = dst + strlen(dst);
    while ((*d++ = *src++));
    return dst;
}

char *strchr(const char *s, int c)
{
    for (; *s; s++)
        if ((unsigned char)*s == (unsigned char)c) return (char *)s;
    return (c == '\0') ? (char *)s : NULL;
}

/* strtok state */
static char *strtok_ptr;

char *strtok(char *s, const char *delim)
{
    if (s) strtok_ptr = s;
    if (!strtok_ptr) return NULL;

    /* Skip leading delimiters */
    while (*strtok_ptr && strchr(delim, *strtok_ptr)) strtok_ptr++;
    if (!*strtok_ptr) return NULL;

    char *tok = strtok_ptr;
    while (*strtok_ptr && !strchr(delim, *strtok_ptr)) strtok_ptr++;
    if (*strtok_ptr) { *strtok_ptr = '\0'; strtok_ptr++; }
    return tok;
}

void *memcpy(void *dst, const void *src, size_t n)
{
    unsigned char       *d = dst;
    const unsigned char *s = src;
    while (n--) *d++ = *s++;
    return dst;
}

void *memset(void *s, int c, size_t n)
{
    unsigned char *p = s;
    while (n--) *p++ = (unsigned char)c;
    return s;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *pa = a, *pb = b;
    while (n--) {
        if (*pa != *pb) return *pa - *pb;
        pa++; pb++;
    }
    return 0;
}
