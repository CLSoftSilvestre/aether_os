#ifndef UART_PL011_H
#define UART_PL011_H

#include "aether/types.h"

/*
 * PL011 UART driver — AetherOS
 *
 * The PL011 is ARM's standard UART. It is present in:
 *   - QEMU "-M virt" at base address 0x09000000
 *   - Raspberry Pi 5 (via RP1 bridge) at a different address (Phase 2)
 *
 * Think of MMIO registers as memory addresses that control hardware —
 * just like the x86 "IN/OUT" port addresses, but accessed with normal
 * load/store instructions.
 */

/* QEMU virt machine: PL011 UART base address */
#define UART0_BASE  0x09000000UL

/* ── PL011 register offsets (from base address) ─────────────────────── */
#define UART_DR     0x000   /* Data Register — write byte here to send    */
#define UART_FR     0x018   /* Flag Register  — status bits               */
#define UART_IBRD   0x024   /* Integer Baud Rate Divisor                  */
#define UART_FBRD   0x028   /* Fractional Baud Rate Divisor               */
#define UART_LCR_H  0x02C   /* Line Control Register                      */
#define UART_CR     0x030   /* Control Register                           */
#define UART_IMSC   0x038   /* Interrupt Mask Set/Clear                   */
#define UART_MIS    0x040   /* Masked Interrupt Status                    */
#define UART_ICR    0x044   /* Interrupt Clear Register                   */

/* IMSC / ICR bit for the receive FIFO interrupt */
#define UART_INT_RX  (1U << 4)   /* RXIM / RXIC */

/* GIC SPI 1 = interrupt ID 33 — PL011 UART0 on QEMU virt */
#define UART_IRQ_ID  33U

/* ── Flag Register (FR) bits ──────────────────────────────────────────── */
#define UART_FR_RXFE  (1 << 4)   /* Receive FIFO Empty  */
#define UART_FR_TXFF  (1 << 5)   /* Transmit FIFO Full  */
#define UART_FR_RXFF  (1 << 6)   /* Receive FIFO Full   */
#define UART_FR_TXFE  (1 << 7)   /* Transmit FIFO Empty */

/* ── Line Control Register (LCR_H) bits ──────────────────────────────── */
#define UART_LCR_FEN  (1 << 4)   /* FIFO Enable  */
#define UART_LCR_8N1  (3 << 5)   /* 8-bit, no parity, 1 stop bit */

/* ── Control Register (CR) bits ──────────────────────────────────────── */
#define UART_CR_UARTEN  (1 << 0)  /* UART Enable  */
#define UART_CR_TXE     (1 << 8)  /* Transmit Enable */
#define UART_CR_RXE     (1 << 9)  /* Receive Enable  */

/* ── Public API ──────────────────────────────────────────────────────── */
void uart_init(void);
void uart_putc(char c);
void uart_puts(const char *s);
void uart_puthex(u64 value);
void uart_putdec(u64 value);

/* Enable RX interrupt in the UART and GIC.  Call after gic_init(). */
void uart_enable_rx_irq(void);

/* Called from el1_irq_handler when IRQ ID == UART_IRQ_ID */
void uart_irq_handler(void);

/* Non-blocking: returns 1 if receive ring buffer has data */
int  uart_rx_empty(void);

/* Non-blocking read — caller must check uart_rx_empty() first */
char uart_getc_nowait(void);

#endif /* UART_PL011_H */
