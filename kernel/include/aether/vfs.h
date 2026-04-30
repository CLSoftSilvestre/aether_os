#ifndef AETHER_VFS_H
#define AETHER_VFS_H

/*
 * AetherOS — Virtual Filesystem Switch (Phase 5.2)
 * File: kernel/include/aether/vfs.h
 *
 * Thin abstraction over initrd and FAT32.
 * VFS file descriptors are returned as integers in [VFS_FD_BASE, VFS_FD_BASE+15].
 * These are distinct from task fd_table entries (0-7) and socket fds (100-107).
 *
 * Mount table (2 entries):
 *   "/"       → FAT32 on virtio-blk  (when disk present)
 *   "/initrd"  → embedded CPIO initrd (always present)
 */

#include "aether/types.h"

#define VFS_FD_BASE   200        /* VFS fds: 200 .. 215 */
#define VFS_MAX_FD    16

void vfs_init(void);             /* call after virtio_blk_init + fat32_mount */

/* Returns VFS_FD_BASE + slot, or -1 on error */
int  vfs_open   (const char *path);
int  vfs_read   (int vfd, u8 *buf, u32 len);
void vfs_close  (int vfd);

/* Fill buf with directory listing.  Returns bytes written or -1. */
int  vfs_readdir(const char *path, char *buf, u32 len);

/* Write support — only FAT32 paths; initrd/AetherFS return -1 (EROFS) */
int  vfs_create (const char *path);                    /* create/truncate; returns vfd or -1 */
int  vfs_write  (int vfd, const u8 *buf, u32 len);    /* bytes written or -1 */
int  vfs_mkdir  (const char *path);                    /* create directory; returns 0 or -1 */

/* 1 if the vfd number is in the VFS range */
static inline int vfs_is_vfd(int fd) { return fd >= VFS_FD_BASE && fd < VFS_FD_BASE + VFS_MAX_FD; }

#endif /* AETHER_VFS_H */
