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

void *memmove(void *dst, const void *src, size_t n)
{
    unsigned char       *d = dst;
    const unsigned char *s = src;
    if (d < s || d >= s + n) {
        while (n--) *d++ = *s++;
    } else {
        d += n; s += n;
        while (n--) *--d = *--s;
    }
    return dst;
}

char *strrchr(const char *s, int c)
{
    const char *last = NULL;
    for (; *s; s++)
        if ((unsigned char)*s == (unsigned char)c) last = s;
    return (c == '\0') ? (char *)s : (char *)last;
}

char *strstr(const char *haystack, const char *needle)
{
    if (!*needle) return (char *)haystack;
    for (; *haystack; haystack++) {
        const char *h = haystack, *n = needle;
        while (*h && *n && *h == *n) { h++; n++; }
        if (!*n) return (char *)haystack;
    }
    return NULL;
}

char *strpbrk(const char *s, const char *accept)
{
    for (; *s; s++)
        for (const char *a = accept; *a; a++)
            if (*s == *a) return (char *)s;
    return NULL;
}

size_t strspn(const char *s, const char *accept)
{
    size_t n = 0;
    for (; *s; s++, n++) {
        const char *a = accept;
        while (*a && *a != *s) a++;
        if (!*a) break;
    }
    return n;
}

size_t strcspn(const char *s, const char *reject)
{
    size_t n = 0;
    for (; *s; s++, n++) {
        const char *r = reject;
        while (*r && *r != *s) r++;
        if (*r) break;
    }
    return n;
}

int strcoll(const char *a, const char *b)
{
    return strcmp(a, b); /* locale-independent: same as strcmp */
}

char *strncat(char *dst, const char *src, size_t n)
{
    char *d = dst + strlen(dst);
    while (n-- && *src) *d++ = *src++;
    *d = '\0';
    return dst;
}

void *memchr(const void *s, int c, size_t n)
{
    const unsigned char *p = s;
    while (n--) {
        if (*p == (unsigned char)c) return (void *)p;
        p++;
    }
    return NULL;
}
