#ifndef AETHER_PL050_KBD_H
#define AETHER_PL050_KBD_H

#include "keycodes.h"

/*
 * PL050 / ARM PrimeCell KMI — keyboard controller.
 *
 * QEMU -M virt exposes:
 *   MMIO base : 0x09050000
 *   GIC IRQ   : 52  (SPI 20)
 *
 * The controller speaks PS/2 Set 2 scan codes.
 * We decode them here into key_event_t structs stored in a ring buffer.
 */

void pl050_kbd_init(void);
void pl050_kbd_irq_handler(void);

/* Returns 1 if the key-event ring buffer is empty */
int  kbd_event_empty(void);

/* Dequeue one event (caller must check !kbd_event_empty() first) */
unsigned long long kbd_get_event(void);  /* returns packed key_event_t */

/* Current modifier bitmask (MOD_* flags) — updated as keys arrive */
unsigned int kbd_get_modifiers(void);

#endif /* AETHER_PL050_KBD_H */
