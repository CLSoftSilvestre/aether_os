/*
 * AetherOS — Kernel logging (printk)
 * File: kernel/core/printk.c
 *
 * Provides printf-style formatted output to the UART.
 * No heap, no file I/O — just format → UART.
 *
 * Supported format specifiers:
 *   %c        — single character
 *   %s        — null-terminated string
 *   %d / %i   — signed 32-bit decimal
 *   %u        — unsigned 32-bit decimal
 *   %x        — unsigned 32-bit hex (lowercase)
 *   %X        — unsigned 32-bit hex (uppercase)
 *   %lx / %lX — unsigned 64-bit hex
 *   %lu       — unsigned 64-bit decimal
 *   %p        — pointer as 0x<16 hex digits>
 *   %%        — literal percent sign
 */

#include "aether/printk.h"
#include "drivers/char/uart_pl011.h"
#include "drivers/video/fb_console.h"
#include <stdarg.h>   /* va_list — provided by compiler even in -ffreestanding */

/* Output one character to all active sinks (UART always; framebuffer when ready) */
static void pk_putc(char c)
{
    uart_putc(c);
    fb_console_putc(c);
}

static void pk_puts(const char *s)
{
    while (*s) pk_putc(*s++);
}

/* ── Level prefixes ────────────────────────────────────────────────────── */
static const char *const level_prefix[] = {
    [LOG_DEBUG] = "[DBG] ",
    [LOG_INFO]  = "[INF] ",
    [LOG_WARN]  = "[WRN] ",
    [LOG_ERROR] = "[ERR] ",
    [LOG_PANIC] = "[!!!] ",
};

/* ── Internal number formatters ────────────────────────────────────────── */

static void print_hex(u64 value, int uppercase, int min_digits)
{
    static const char lo[] = "0123456789abcdef";
    static const char hi[] = "0123456789ABCDEF";
    const char *digits = uppercase ? hi : lo;

    char buf[17];
    int  i = 16;
    buf[i] = '\0';

    if (value == 0) {
        /* pad with zeros to min_digits */
        while (min_digits-- > 0)
            pk_putc('0');
        if (min_digits < 0)
            pk_putc('0');
        return;
    }

    while (value && i > 0) {
        buf[--i] = digits[value & 0xF];
        value >>= 4;
        min_digits--;
    }
    /* pad with leading zeros if needed */
    while (min_digits-- > 0)
        pk_putc('0');

    pk_puts(&buf[i]);
}

static void print_udec(u64 value)
{
    char buf[21];
    int  i = 20;
    buf[i] = '\0';

    if (value == 0) {
        pk_putc('0');
        return;
    }

    while (value && i > 0) {
        buf[--i] = '0' + (value % 10);
        value /= 10;
    }

    pk_puts(&buf[i]);
}

static void print_sdec(s64 value)
{
    if (value < 0) {
        uart_putc('-');
        /* careful: -INT64_MIN can't be represented as positive s64 */
        print_udec((u64)(-(value + 1)) + 1);
    } else {
        print_udec((u64)value);
    }
}

/* ── Core formatter ────────────────────────────────────────────────────── */

static void vprintk(const char *fmt, va_list args)
{
    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            pk_putc(*fmt);
            continue;
        }

        fmt++;   /* skip '%' */

        /* Check for 'l' length modifier */
        int is_long = 0;
        if (*fmt == 'l') {
            is_long = 1;
            fmt++;
        }

        switch (*fmt) {
        case 'c':
            pk_putc((char)va_arg(args, int));
            break;

        case 's': {
            const char *s = va_arg(args, const char *);
            pk_puts(s ? s : "(null)");
            break;
        }

        case 'd':
        case 'i':
            if (is_long)
                print_sdec((s64)va_arg(args, long));
            else
                print_sdec((s64)va_arg(args, int));
            break;

        case 'u':
            if (is_long)
                print_udec((u64)va_arg(args, unsigned long));
            else
                print_udec((u64)va_arg(args, unsigned int));
            break;

        case 'x':
            if (is_long)
                print_hex((u64)va_arg(args, unsigned long), 0, 1);
            else
                print_hex((u64)va_arg(args, unsigned int), 0, 1);
            break;

        case 'X':
            if (is_long)
                print_hex((u64)va_arg(args, unsigned long), 1, 1);
            else
                print_hex((u64)va_arg(args, unsigned int), 1, 1);
            break;

        case 'p': {
            /* Pointer: always 64-bit, printed as 0x<16 digits> */
            u64 ptr = (u64)(uintptr_t)va_arg(args, void *);
            pk_puts("0x");
            print_hex(ptr, 0, 16);
            break;
        }

        case '%':
            pk_putc('%');
            break;

        default:
            /* Unknown specifier — print literally */
            pk_putc('%');
            if (is_long) pk_putc('l');
            pk_putc(*fmt);
            break;
        }
    }
}

/* ── Public API ─────────────────────────────────────────────────────────── */

void printk(int level, const char *fmt, ...)
{
    if (level < LOG_LEVEL_MIN)
        return;

    /* Print level prefix */
    if (level >= LOG_DEBUG && level <= LOG_PANIC)
        pk_puts(level_prefix[level]);

    va_list args;
    va_start(args, fmt);
    vprintk(fmt, args);
    va_end(args);
}
