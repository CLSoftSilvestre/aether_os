#ifndef AETHER_FAT32_H
#define AETHER_FAT32_H

/*
 * AetherOS — FAT32 read-only filesystem (Phase 5.2)
 * File: kernel/include/aether/fat32.h
 */

#include "aether/types.h"

#define FAT32_MAX_FILES      8     /* simultaneous open file handles */
#define FAT32_FILENAME_MAX   256   /* max path component length */

int  fat32_mount(void);            /* mount from virtio-blk sector 0; returns 0 or -1 */
int  fat32_ready(void);            /* 1 if mounted */

int  fat32_open   (const char *path);               /* returns handle 0..7 or -1 */
int  fat32_read   (int fh, u8 *buf, u32 len);       /* bytes read; 0=EOF; -1=error */
void fat32_close  (int fh);                         /* flushes size on write handles */
int  fat32_readdir(const char *path, char *buf, u32 len); /* returns bytes written */

/* Write support — creates/truncates a file; only 8.3-compatible names */
int  fat32_create (const char *path);               /* returns handle 0..7 or -1 */
int  fat32_write  (int fh, const u8 *buf, u32 len); /* bytes written or -1 */
int  fat32_mkdir  (const char *path);               /* create directory; returns 0 or -1 */

#endif /* AETHER_FAT32_H */
