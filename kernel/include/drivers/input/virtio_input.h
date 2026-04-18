#ifndef AETHER_VIRTIO_INPUT_H
#define AETHER_VIRTIO_INPUT_H

/*
 * VirtIO-Input driver (MMIO transport).
 *
 * virtio_input_init() scans the 32 VirtIO MMIO transports at
 * 0x0a000000+n*0x200 looking for Device ID 18 (virtio-input).
 * Add -device virtio-tablet-device to the QEMU command line.
 *
 * On success the tablet feeds absolute coordinates into the mouse
 * event ring via mouse_post_event(); sys_mouse_poll() drains it.
 */

void virtio_input_init(void);
void virtio_input_irq_handler(void);

/* Returns the registered GIC INTID, or -1 if no device found. */
int  virtio_input_irq(void);

#endif /* AETHER_VIRTIO_INPUT_H */
