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
#include "aether/spinlock.h"
#include "aether/smp.h"
#include "drivers/char/uart_pl011.h"
#include "drivers/video/fb_console.h"
#include <stdarg.h>   /* va_list — provided by compiler even in -ffreestanding */

static spinlock_t    g_printk_lock  = SPINLOCK_INIT;
/* Core currently inside the printk critical section, or PRINTK_NO_OWNER. */
#define PRINTK_NO_OWNER  0xFFFFFFFFu
static volatile u32  g_printk_owner = PRINTK_NO_OWNER;

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

    if (min_digits < 1) min_digits = 1;

    if (value == 0) {
        for (int j = 0; j < min_digits; j++) pk_putc('0');
        return;
    }

    while (value && i > 0) {
        buf[--i] = digits[value & 0xF];
        value >>= 4;
        min_digits--;
    }
    while (min_digits > 0) { pk_putc('0'); min_digits--; }

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

        /* Parse optional zero-flag and width (e.g. %02x, %04x) */
        int width = 0;
        if (*fmt == '0') fmt++;          /* consume leading zero */
        while (*fmt >= '1' && *fmt <= '9') { width = width * 10 + (*fmt++ - '0'); }

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
                print_hex((u64)va_arg(args, unsigned long), 0, width ? width : 1);
            else
                print_hex((u64)va_arg(args, unsigned int), 0, width ? width : 1);
            break;

        case 'X':
            if (is_long)
                print_hex((u64)va_arg(args, unsigned long), 1, width ? width : 1);
            else
                print_hex((u64)va_arg(args, unsigned int), 1, width ? width : 1);
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

    /*
     * IRQ-safe, re-entrant, deadlock-tolerant critical section.
     *
     * Mask IRQs first: the SVC handler runs with IRQs unmasked, and the timer
     * ISR calls kinfo() — a timer firing while this core holds g_printk_lock
     * would otherwise self-deadlock.  daifset #2 masks IRQ/FIQ but NOT
     * synchronous exceptions, so a fault taken while we hold the lock would
     * re-enter printk() (via the panic path) and deadlock on a lock this core
     * already owns — silently freezing the whole machine and hiding the very
     * panic we need.  Guard against that with an owner check: if this core is
     * already inside printk (re-entered via a fault), print without re-taking
     * the lock.  Output may interleave with the message we were mid-way
     * through, but garbled diagnostics beat a silent freeze that hides the
     * panic entirely.
     */
    u64 daif_saved;
    __asm__ volatile(
        "mrs %0, DAIF\n"
        "msr daifset, #2\n"
        : "=r"(daif_saved) :: "memory"
    );

    u32  me        = cpu_id();
    int  reentrant = (g_printk_owner == me);

    if (!reentrant) {
        spin_lock(&g_printk_lock);
        g_printk_owner = me;
    }

    /* Print level prefix */
    if (level >= LOG_DEBUG && level <= LOG_PANIC)
        pk_puts(level_prefix[level]);

    va_list args;
    va_start(args, fmt);
    vprintk(fmt, args);
    va_end(args);

    if (!reentrant) {
        g_printk_owner = PRINTK_NO_OWNER;
        spin_unlock(&g_printk_lock);
    }

    /* Restore IRQ mask state (no-op if they were already disabled) */
    __asm__ volatile("msr DAIF, %0" :: "r"(daif_saved) : "memory");
}
