/*
 * AetherOS — VirtIO net driver interface
 * File: kernel/include/drivers/net/virtio_net.h
 */
#ifndef AETHER_VIRTIO_NET_H
#define AETHER_VIRTIO_NET_H

#include "aether/types.h"

/* Initialize VirtIO net PCI device.
 * On success: fills g_our_mac, returns 1. On failure: returns 0. */
int  virtio_net_init(void);

/* Transmit a raw Ethernet frame (max 1514 bytes). Returns 0 or -1. */
int  virtio_net_tx(const u8 *frame, u16 len);

/* Poll RX queue; delivers received frames via net_deliver_frame(). */
void virtio_net_rx_poll(void);

#endif /* AETHER_VIRTIO_NET_H */
