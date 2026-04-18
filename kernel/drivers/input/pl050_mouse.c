/*
 * AetherOS — PL050 KMI Mouse Driver
 * File: kernel/drivers/input/pl050_mouse.c
 *
 * ARM PrimeCell KMI (PL050) PS/2 mouse controller.
 * QEMU -M virt provides this device at 0x09060000, GIC IRQ 53 (SPI 21).
 *
 * PS/2 3-byte packet protocol:
 *   Byte 0: flags  [7=Y_ovf, 6=X_ovf, 5=Y_sign, 4=X_sign, 3=1, 2=mid, 1=right, 0=left]
 *   Byte 1: ΔX     (8-bit magnitude; sign bit in flags[4])
 *   Byte 2: ΔY     (8-bit magnitude; sign bit in flags[5]; Y positive = upward on screen)
 *
 * Initialization sends 0xF4 (Enable Data Reporting) to start movement packets.
 * Absolute position is clamped to the screen bounds (1024×768).
 */

#include "drivers/input/pl050_mouse.h"
#include "drivers/input/keycodes.h"
#include "aether/types.h"
#include "aether/printk.h"
#include "drivers/irq/gic_v2.h"

/* ── MMIO register map ──────────────────────────────────────────────────── */

#define KMI_MOUSE_BASE  0x09060000UL
#define KMI_IRQ_MOUSE   53

#define KMI_CR_OFF      0x00
#define KMI_STAT_OFF    0x04
#define KMI_DATA_OFF    0x08

#define KMI_CR_EN       (1u << 2)
#define KMI_CR_RXINTREN (1u << 4)
#define KMI_CR_TXINTREN (1u << 3)
#define KMI_STAT_RXFULL (1u << 4)
#define KMI_STAT_TXEMPTY (1u << 6)

#define REG32(base, off) (*((volatile u32 *)((uintptr_t)(base) + (off))))

/* ── Screen bounds ──────────────────────────────────────────────────────── */

#define SCREEN_W 1024
#define SCREEN_H  768

/* ── Absolute cursor state ──────────────────────────────────────────────── */

static int cursor_x = SCREEN_W / 2;
static int cursor_y = SCREEN_H / 2;

void mouse_get_pos(unsigned int *x, unsigned int *y)
{
    *x = (unsigned int)cursor_x;
    *y = (unsigned int)cursor_y;
}

/* ── Mouse event ring buffer ────────────────────────────────────────────── */

#define MOUSE_RING_SIZE 16

static unsigned long long mouse_ring[MOUSE_RING_SIZE];
static volatile unsigned int mouse_head;
static volatile unsigned int mouse_tail;

static void mouse_ring_push(unsigned long long ev)
{
    unsigned int next = (mouse_head + 1) % MOUSE_RING_SIZE;
    if (next == mouse_tail) return;   /* drop on overflow */
    mouse_ring[mouse_head] = ev;
    mouse_head = next;
}

int mouse_event_empty(void) { return mouse_head == mouse_tail; }

unsigned long long mouse_get_event(void)
{
    unsigned long long v = mouse_ring[mouse_tail];
    mouse_tail = (mouse_tail + 1) % MOUSE_RING_SIZE;
    return v;
}

/* ── PS/2 packet accumulator ────────────────────────────────────────────── */

static u8 pkt[3];
static int pkt_idx;

static void process_packet(void)
{
    u8 flags = pkt[0];
    u8 bx    = pkt[1];
    u8 by    = pkt[2];

    /* Discard packet if overflow bits are set */
    if ((flags & 0xC0) != 0) return;

    /* Sign-extend ΔX and ΔY from 9-bit 2's complement */
    int dx = (int)(unsigned int)bx;
    if (flags & (1u << 4)) dx -= 256;

    int dy = (int)(unsigned int)by;
    if (flags & (1u << 5)) dy -= 256;

    /* PS/2 Y is inverted relative to screen coords (positive = upward) */
    cursor_x += dx;
    cursor_y -= dy;

    if (cursor_x < 0) cursor_x = 0;
    if (cursor_x >= SCREEN_W) cursor_x = SCREEN_W - 1;
    if (cursor_y < 0) cursor_y = 0;
    if (cursor_y >= SCREEN_H) cursor_y = SCREEN_H - 1;

    unsigned int buttons =
        ((flags >> 0) & 1) |        /* left   → bit 0 */
        ((flags >> 1) & 1) << 1 |   /* right  → bit 1 */
        ((flags >> 2) & 1) << 2;    /* middle → bit 2 */

    mouse_event_t ev;
    ev.x       = (unsigned int)cursor_x;
    ev.y       = (unsigned int)cursor_y;
    ev.buttons = buttons;
    mouse_ring_push(mouse_event_pack(ev));
}

/* ── PS/2 send helper ───────────────────────────────────────────────────── */

static void ps2_send(u8 byte)
{
    /* Wait for TX FIFO empty, then write */
    int timeout = 10000;
    while (!(REG32(KMI_MOUSE_BASE, KMI_STAT_OFF) & KMI_STAT_TXEMPTY) && --timeout)
        ;
    REG32(KMI_MOUSE_BASE, KMI_DATA_OFF) = (u32)byte;
}

/* ── IRQ handler ────────────────────────────────────────────────────────── */

void pl050_mouse_irq_handler(void)
{
    while (REG32(KMI_MOUSE_BASE, KMI_STAT_OFF) & KMI_STAT_RXFULL) {
        u8 byte = (u8)REG32(KMI_MOUSE_BASE, KMI_DATA_OFF);

        /* Discard ACK bytes (0xFA) from init commands */
        if (byte == 0xFA) continue;

        pkt[pkt_idx++] = byte;
        if (pkt_idx == 3) {
            process_packet();
            pkt_idx = 0;
        }
    }
}

/* ── Initialization ─────────────────────────────────────────────────────── */

void pl050_mouse_init(void)
{
    cursor_x   = SCREEN_W / 2;
    cursor_y   = SCREEN_H / 2;
    pkt_idx    = 0;
    mouse_head = 0;
    mouse_tail = 0;

    /* Enable controller (TX needed to send init commands) */
    REG32(KMI_MOUSE_BASE, KMI_CR_OFF) = KMI_CR_EN;

    /* Enable Data Reporting — mouse won't send packets until this */
    ps2_send(0xF4);

    /* Switch on RX interrupt now that init command is sent */
    REG32(KMI_MOUSE_BASE, KMI_CR_OFF) = KMI_CR_EN | KMI_CR_RXINTREN;

    gic_enable_irq(KMI_IRQ_MOUSE);

    kinfo("MOUSE: PL050 mouse init — MMIO 0x%lx, IRQ %u\n",
          (unsigned long)KMI_MOUSE_BASE, KMI_IRQ_MOUSE);
}
