#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <sys.h>

/* ── vsnprintf ───────────────────────────────────────────────────────── */

static void _emit(char *buf, size_t size, size_t *pos, char c)
{
    if (*pos + 1 < size) buf[(*pos)++] = c;
}

static void _emit_str(char *buf, size_t size, size_t *pos, const char *s)
{
    if (!s) s = "(null)";
    while (*s) _emit(buf, size, pos, *s++);
}

static void _emit_u64(char *buf, size_t size, size_t *pos, unsigned long long v,
                      int base, int upper)
{
    static const char lo[] = "0123456789abcdef";
    static const char hi[] = "0123456789ABCDEF";
    const char *digits = upper ? hi : lo;
    char tmp[24];
    int  n = 0;
    if (v == 0) { _emit(buf, size, pos, '0'); return; }
    while (v) { tmp[n++] = digits[v % (unsigned)base]; v /= (unsigned)base; }
    while (n--) _emit(buf, size, pos, tmp[n]);
}

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
    size_t pos = 0;

    for (const char *p = fmt; *p; p++) {
        if (*p != '%') { _emit(buf, size, &pos, *p); continue; }
        p++;
        if (!*p) break;

        /* Long prefix */
        int is_long = 0;
        if (*p == 'l') { is_long = 1; p++; }
        if (*p == 'l') { p++; }   /* 'll' → treat as long long */

        switch (*p) {
        case 'd': case 'i': {
            long long v = is_long ? (long long)va_arg(ap, long)
                                  : (long long)va_arg(ap, int);
            if (v < 0) { _emit(buf, size, &pos, '-'); v = -v; }
            _emit_u64(buf, size, &pos, (unsigned long long)v, 10, 0);
            break;
        }
        case 'u': {
            unsigned long long v = is_long ? (unsigned long long)va_arg(ap, unsigned long)
                                           : (unsigned long long)va_arg(ap, unsigned int);
            _emit_u64(buf, size, &pos, v, 10, 0);
            break;
        }
        case 'x': {
            unsigned long long v = is_long ? (unsigned long long)va_arg(ap, unsigned long)
                                           : (unsigned long long)va_arg(ap, unsigned int);
            _emit_u64(buf, size, &pos, v, 16, 0);
            break;
        }
        case 'X': {
            unsigned long long v = is_long ? (unsigned long long)va_arg(ap, unsigned long)
                                           : (unsigned long long)va_arg(ap, unsigned int);
            _emit_u64(buf, size, &pos, v, 16, 1);
            break;
        }
        case 'p': {
            unsigned long long v = (unsigned long long)(uintptr_t)va_arg(ap, void *);
            _emit(buf, size, &pos, '0'); _emit(buf, size, &pos, 'x');
            _emit_u64(buf, size, &pos, v, 16, 0);
            break;
        }
        case 's':
            _emit_str(buf, size, &pos, va_arg(ap, const char *));
            break;
        case 'c':
            _emit(buf, size, &pos, (char)va_arg(ap, int));
            break;
        case '%':
            _emit(buf, size, &pos, '%');
            break;
        default:
            _emit(buf, size, &pos, '%');
            _emit(buf, size, &pos, *p);
            break;
        }
    }
    if (size > 0) buf[pos] = '\0';
    return (int)pos;
}

int snprintf(char *buf, size_t size, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return r;
}

/* ── printf ──────────────────────────────────────────────────────────── */

int printf(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    sys_write(STDOUT_FILENO, buf, n);
    return n;
}

/* ── putchar / puts ──────────────────────────────────────────────────── */

int putchar(int c)
{
    char ch = (char)c;
    sys_write(STDOUT_FILENO, &ch, 1);
    return c;
}

int puts(const char *s)
{
    sys_puts(s);
    sys_write(STDOUT_FILENO, "\n", 1);
    return 0;
}

/* ── readline ────────────────────────────────────────────────────────── */

/*
 * Read characters one at a time (blocking), handling backspace.
 * Echoes input to terminal. Returns on newline ('\n' or '\r').
 */
int readline(char *buf, int max)
{
    int n = 0;
    while (n < max - 1) {
        char c;
        sys_read(STDIN_FILENO, &c, 1);

        if (c == '\r' || c == '\n') {
            putchar('\n');
            break;
        }
        if ((c == '\b' || c == 127) && n > 0) {
            /* Backspace: erase last char on terminal */
            sys_write(STDOUT_FILENO, "\b \b", 3);
            n--;
            continue;
        }
        if ((unsigned char)c < 32) continue;   /* ignore other control chars */
        buf[n++] = c;
        putchar(c);   /* echo */
    }
    buf[n] = '\0';
    return n;
}
