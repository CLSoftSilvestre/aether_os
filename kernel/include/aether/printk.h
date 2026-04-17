#ifndef AETHER_PRINTK_H
#define AETHER_PRINTK_H

/*
 * AetherOS kernel logging — printk
 *
 * printk() is the kernel's printf(). It formats a string and sends it to
 * the UART. There is no buffering yet — every call blocks until the UART
 * transmit FIFO drains (polled I/O).
 *
 * Usage:
 *   printk(LOG_INFO, "Booted CPU %d\n", cpu_id);
 *   kinfo("Booted CPU %d\n", cpu_id);   // convenience macro
 */

/* Log levels — higher number = more severe */
#define LOG_DEBUG  0
#define LOG_INFO   1
#define LOG_WARN   2
#define LOG_ERROR  3
#define LOG_PANIC  4

/* Minimum level to actually print (change to LOG_DEBUG to see everything) */
#define LOG_LEVEL_MIN  LOG_DEBUG

/* Main logging function */
void printk(int level, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

/* Convenience macros — less typing in practice */
#define kdebug(fmt, ...)  printk(LOG_DEBUG, fmt, ##__VA_ARGS__)
#define kinfo(fmt, ...)   printk(LOG_INFO,  fmt, ##__VA_ARGS__)
#define kwarn(fmt, ...)   printk(LOG_WARN,  fmt, ##__VA_ARGS__)
#define kerror(fmt, ...)  printk(LOG_ERROR, fmt, ##__VA_ARGS__)

/*
 * kpanic — print message and halt the CPU.
 * The do{}while(0) wrapper makes it safe to use after if/else without braces.
 */
#define kpanic(fmt, ...) \
    do { \
        printk(LOG_PANIC, "PANIC at %s:%d: " fmt, __FILE__, __LINE__, ##__VA_ARGS__); \
        for (;;) { __asm__ volatile("wfi"); } \
    } while (0)

#endif /* AETHER_PRINTK_H */
