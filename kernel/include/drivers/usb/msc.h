/*
 * AetherOS — USB Mass Storage Class Driver (Phase 5.2.12)
 * File: kernel/include/drivers/usb/msc.h
 *
 * Implements USB MSC Bulk-Only Transport (BOT), class 0x08 / subclass 0x06
 * / protocol 0x50 (BBB).
 *
 * Exposes: usb_msc_read_sectors() — 512-byte sector reads for the VFS.
 */

#ifndef AETHER_USB_MSC_H
#define AETHER_USB_MSC_H

#include "aether/types.h"

/*
 * usb_msc_probe — called by xhci.c after a mass-storage device is found
 * and bulk endpoints are configured.
 * slot_id:  xHCI device slot
 * ep_out:   bulk OUT endpoint number (4-bit)
 * ep_in:    bulk IN  endpoint number (4-bit)
 * ep_mps:   bulk endpoint max packet size (usually 512)
 */
void usb_msc_probe(u8 slot_id, u8 ep_out, u8 ep_in, u16 ep_mps);

/* Returns 1 after usb_msc_probe() succeeded and disk is accessible. */
int  usb_msc_ready(void);

/* Returns the total number of 512-byte sectors on the disk. */
u32  usb_msc_sector_count(void);

/*
 * usb_msc_read_sectors — read count 512-byte sectors starting at lba into buf.
 * Returns 0 on success, -1 on error.
 * buf must be 4-byte aligned and count * 512 bytes long.
 */
int  usb_msc_read_sectors(u64 lba, u32 count, u8 *buf);

#endif /* AETHER_USB_MSC_H */
