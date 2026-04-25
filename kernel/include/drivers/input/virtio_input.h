#ifndef AETHER_VIRTIO_INPUT_H
#define AETHER_VIRTIO_INPUT_H

#include "aether/types.h"

/*
 * VirtIO-Input driver (MMIO transport, up to 2 devices).
 *
 * Add to QEMU command line:
 *   -device virtio-tablet-device    (absolute mouse)
 *   -device virtio-keyboard-device  (keyboard → kbd ring)
 *
 * virtio_input_init() scans all 32 VirtIO MMIO slots for Device ID 18.
 * IRQ dispatch: call virtio_input_owns_irq(irq) then virtio_input_dispatch(irq).
 */

void virtio_input_init(void);

/* Poll all devices — called from timer IRQ at 100 Hz */
void virtio_input_poll(void);

/* IRQ stubs (no longer used — kept for call-site compatibility) */
int  virtio_input_owns_irq(u32 irq);
void virtio_input_dispatch(u32 irq);
void virtio_input_irq_handler(void);
int  virtio_input_irq(void);

#endif /* AETHER_VIRTIO_INPUT_H */
