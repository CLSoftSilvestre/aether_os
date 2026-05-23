#ifndef AETHER_USB_FAT32_H
#define AETHER_USB_FAT32_H

/*
 * AetherOS — USB FAT32 read-only filesystem (Phase 5.2.12)
 * File: kernel/include/aether/usb_fat32.h
 *
 * Independent FAT32 reader backed by USB mass storage sector reads.
 * Mounted at "/usb" in the VFS.  Read-only; no write support.
 */

#include "aether/types.h"

#define USB_FAT32_MAX_FILES  4

int  usb_fat32_mount(void);            /* mount from USB sector 0; returns 0 or -1 */
int  usb_fat32_ready(void);

int  usb_fat32_open   (const char *path);
int  usb_fat32_read   (int fh, u8 *buf, u32 len);
void usb_fat32_close  (int fh);
int  usb_fat32_readdir(const char *path, char *out, u32 out_len);

#endif /* AETHER_USB_FAT32_H */
