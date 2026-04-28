#ifndef AETHER_VIRTIO_BLK_H
#define AETHER_VIRTIO_BLK_H

/*
 * AetherOS — VirtIO block driver (Phase 5.2)
 * File: kernel/include/drivers/block/virtio_blk.h
 *
 * QEMU: -drive file=disk.img,format=raw,if=none,id=hd0 -device virtio-blk-pci,drive=hd0
 * PCI vendor 0x1AF4, device 0x1042 (VirtIO device-id 2 = block).
 */

#include "aether/types.h"

#define VIRTIO_BLK_SECTOR_SIZE 512u

int  virtio_blk_init(void);
int  virtio_blk_ready(void);
u64  virtio_blk_sector_count(void);
int  virtio_blk_read_sectors (u64 lba, u32 count, u8 *buf);
int  virtio_blk_write_sectors(u64 lba, u32 count, const u8 *buf);

#endif /* AETHER_VIRTIO_BLK_H */
