#ifndef AETHER_PL050_MOUSE_H
#define AETHER_PL050_MOUSE_H

#include "keycodes.h"

/*
 * PL050 / ARM PrimeCell KMI — mouse controller.
 *
 * QEMU -M virt exposes:
 *   MMIO base : 0x09060000
 *   GIC IRQ   : 53  (SPI 21)
 *
 * Standard PS/2 3-byte packet protocol.
 * Absolute cursor position is maintained here and updated per packet.
 */

void pl050_mouse_init(void);
void pl050_mouse_irq_handler(void);

/* Returns 1 if the mouse-event ring buffer is empty */
int  mouse_event_empty(void);

/* Dequeue one event (caller must check !mouse_event_empty() first) */
unsigned long long mouse_get_event(void);  /* returns packed mouse_event_t */

/* Current absolute cursor position */
void mouse_get_pos(unsigned int *x, unsigned int *y);

#endif /* AETHER_PL050_MOUSE_H */
