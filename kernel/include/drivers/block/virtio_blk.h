#ifndef AETHER_VIRTIO_BLK_H
#define AETHER_VIRTIO_BLK_H

/*
 * AetherOS — VirtIO block driver (Phase 5.2)
 * File: kernel/include/drivers/block/virtio_blk.h
 *
 * Supports up to VIRTIO_BLK_MAX_DEV devices found on the PCI bus.
 * Device 0 is always the first virtio-blk-pci found; device 1 is the second.
 *
 * Single-device (backward-compatible) API uses device 0.
 * Multi-device API uses the _n suffix with an explicit device index.
 *
 * QEMU attachment:
 *   -drive file=disk.img,format=raw,if=none,id=hd0 -device virtio-blk-pci,drive=hd0
 *   -drive file=afs.img,format=raw,if=none,id=hd1  -device virtio-blk-pci,drive=hd1
 */

#include "aether/types.h"

#define VIRTIO_BLK_SECTOR_SIZE  512u
#define VIRTIO_BLK_MAX_DEV      2u

/* Initialize all VirtIO block devices found on the PCI bus.
 * Returns the number of devices successfully initialized (0 = no disk). */
int virtio_blk_init(void);

/* Number of devices that were successfully initialized. */
int virtio_blk_dev_count(void);

/* ── Single-device API (device 0) — backward-compatible ─────────────────── */

int  virtio_blk_ready       (void);
u64  virtio_blk_sector_count(void);
int  virtio_blk_read_sectors (u64 lba, u32 count, u8 *buf);
int  virtio_blk_write_sectors(u64 lba, u32 count, const u8 *buf);

/* ── Multi-device API ────────────────────────────────────────────────────── */

int  virtio_blk_ready_n       (u32 dev);
u64  virtio_blk_sector_count_n(u32 dev);
int  virtio_blk_read_sectors_n (u32 dev, u64 lba, u32 count, u8 *buf);
int  virtio_blk_write_sectors_n(u32 dev, u64 lba, u32 count, const u8 *buf);

#endif /* AETHER_VIRTIO_BLK_H */
