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
#define UART_ICR    0x044   /* Interrupt Clear Register                   */

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
void uart_puthex(u64 value);    /* print 64-bit value as hex — useful for debugging */
void uart_putdec(u64 value);    /* print 64-bit value as decimal */

#endif /* UART_PL011_H */
