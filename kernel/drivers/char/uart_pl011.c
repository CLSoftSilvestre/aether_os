/*
 * PL011 UART Driver — AetherOS
 * File: kernel/drivers/char/uart_pl011.c
 *
 * TX: polled (busy-wait on TXFF).
 * RX: interrupt-driven.  A 256-byte ring buffer is filled by uart_irq_handler()
 *     (called from el1_irq_handler when IRQ 33 fires).  Consumers call
 *     uart_rx_empty() / uart_getc_nowait() to drain the buffer.
 */

#include "drivers/char/uart_pl011.h"
#include "drivers/irq/gic_v2.h"

/* ── RX ring buffer ──────────────────────────────────────────────────── */
#define RX_BUF_SIZE  256U

static volatile u8  rx_buf[RX_BUF_SIZE];
static volatile u32 rx_head = 0;   /* ISR writes here */
static volatile u32 rx_tail = 0;   /* reader reads here */

/* Helper: read a 32-bit MMIO register */
static inline u32 uart_read(u32 offset)
{
    return MMIO_READ32(UART0_BASE + offset);
}

/* Helper: write a 32-bit MMIO register */
static inline void uart_write(u32 offset, u32 value)
{
    MMIO_WRITE32(UART0_BASE + offset, value);
}

/*
 * uart_init — initialise the PL011 UART
 *
 * Clock assumptions (QEMU virt machine):
 *   Input clock: 24 MHz
 *   Target baud: 115200
 *   Divisor = 24_000_000 / (16 × 115_200) = 13.02
 *   IBRD = 13, FBRD = round(0.02 × 64) = 1
 *
 * On QEMU the baud rate is not actually enforced, so you will see output
 * regardless. On real hardware, incorrect baud settings produce garbage.
 */
void uart_init(void)
{
    /* 1. Disable the UART before reconfiguring */
    uart_write(UART_CR, 0);

    /* 2. Wait for any in-progress transmission to finish */
    while (uart_read(UART_FR) & UART_FR_TXFF)
        ;   /* spin — transmit FIFO must drain */

    /* 3. Clear all pending interrupts */
    uart_write(UART_ICR, 0x7FF);

    /* 4. Set baud rate: 115200 at 24 MHz input clock */
    uart_write(UART_IBRD, 13);
    uart_write(UART_FBRD, 1);

    /* 5. Line control: 8-bit, no parity, 1 stop bit, FIFO enabled */
    uart_write(UART_LCR_H, UART_LCR_8N1 | UART_LCR_FEN);

    /* 6. Mask all interrupts (polled mode — interrupt-driven comes in Phase 2) */
    uart_write(UART_IMSC, 0);

    /* 7. Enable UART, TX, and RX */
    uart_write(UART_CR, UART_CR_UARTEN | UART_CR_TXE | UART_CR_RXE);
}

/*
 * uart_putc — transmit one character
 *
 * Polls the TX FIFO Full flag before writing.
 * This is "polled" (busy-wait) I/O — simple but blocks the CPU.
 * We will switch to interrupt-driven I/O in Milestone 2.1.
 */
void uart_putc(char c)
{
    /* Translate newline to CR+LF so terminals display correctly */
    if (c == '\n')
        uart_putc('\r');

    /* Spin while transmit FIFO is full */
    while (uart_read(UART_FR) & UART_FR_TXFF)
        ;

    /* Write character — only the low 8 bits matter */
    uart_write(UART_DR, (u32)c);
}

/*
 * uart_puts — transmit a null-terminated string
 */
void uart_puts(const char *s)
{
    while (*s)
        uart_putc(*s++);
}

/*
 * uart_puthex — print a 64-bit integer in hexadecimal
 *
 * Useful for printing addresses and register values during debugging.
 * Example: uart_puthex(0xDEADBEEF) → "0xDEADBEEF"
 */
void uart_puthex(u64 value)
{
    static const char digits[] = "0123456789ABCDEF";
    char buf[19];   /* "0x" + 16 hex digits + NUL */
    int i = 18;

    buf[i--] = '\0';

    if (value == 0) {
        uart_puts("0x0");
        return;
    }

    while (value && i > 1) {
        buf[i--] = digits[value & 0xF];
        value >>= 4;
    }
    buf[i--] = 'x';
    buf[i]   = '0';

    uart_puts(&buf[i]);
}

/*
 * uart_putdec — print a 64-bit integer in decimal
 */
void uart_putdec(u64 value)
{
    char buf[21];   /* max u64 is 20 digits + NUL */
    int i = 20;

    buf[i--] = '\0';

    if (value == 0) {
        uart_putc('0');
        return;
    }

    while (value && i >= 0) {
        buf[i--] = '0' + (value % 10);
        value /= 10;
    }

    uart_puts(&buf[i + 1]);
}

/* ── RX interrupt support ────────────────────────────────────────────── */

void uart_enable_rx_irq(void)
{
    uart_write(UART_IMSC, UART_INT_RX);   /* unmask RX interrupt in UART */
    gic_enable_irq(UART_IRQ_ID);          /* enable in GIC distributor   */
}

/*
 * uart_irq_handler — called from el1_irq_handler when IRQ 33 fires.
 *
 * Drains the RX FIFO into the ring buffer. Drops bytes if the buffer
 * is full rather than blocking (safe in an ISR context).
 * Clears the interrupt after draining.
 */
void uart_irq_handler(void)
{
    while (!(uart_read(UART_FR) & UART_FR_RXFE)) {
        u8  ch   = (u8)(uart_read(UART_DR) & 0xFF);
        u32 next = (rx_head + 1u) & (RX_BUF_SIZE - 1u);
        if (next != rx_tail) {
            rx_buf[rx_head] = ch;
            rx_head = next;
        }
        /* else: buffer full — drop byte */
    }
    uart_write(UART_ICR, UART_INT_RX);   /* clear RX interrupt */
}

int uart_rx_empty(void)
{
    /* Drain hardware FIFO into the ring buffer so pollers work without
     * needing the UART RX interrupt to be enabled. */
    while (!(uart_read(UART_FR) & UART_FR_RXFE)) {
        u8  ch   = (u8)(uart_read(UART_DR) & 0xFF);
        u32 next = (rx_head + 1u) & (RX_BUF_SIZE - 1u);
        if (next != rx_tail) {
            rx_buf[rx_head] = ch;
            rx_head = next;
        }
    }
    return rx_head == rx_tail;
}

char uart_getc_nowait(void)
{
    u8 ch    = rx_buf[rx_tail];
    rx_tail  = (rx_tail + 1u) & (RX_BUF_SIZE - 1u);
    return (char)ch;
}
