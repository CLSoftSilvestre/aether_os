#ifndef AETHER_AETHERFS_H
#define AETHER_AETHERFS_H

/*
 * AetherOS — AetherFS native filesystem (Phase 5.2.10)
 * File: kernel/include/aether/aetherfs.h
 *
 * AetherFS on-disk layout (4096-byte blocks, virtio-blk device 1):
 *
 *   Block 0:       Superblock  (magic + geometry + snapshots + xxHash32)
 *   Block 1:       Inode bitmap (1 bit per inode, LSB-first)
 *   Block 2..33:   Inode table  (1024 inodes × 128 bytes = 32 blocks)
 *   Block 34:      Data block bitmap
 *   Block 35+:     Data blocks (4096 bytes each)
 *
 * Inode direct[] entries hold absolute disk block numbers.
 *
 * Mount point: /afs  (registered in VFS layer)
 *
 * Write support (SYS_FS_WRITE / SYS_FS_CREATE) is Phase 5.5.
 */

#include "aether/types.h"

/* ── On-disk constants ───────────────────────────────────────────────────── */

#define AFS_MAGIC           0x4145544845524653ULL   /* "AETHERFS" LE */
#define AFS_INODE_MAGIC     0xAEF51E00u
#define AFS_VERSION         1u
#define AFS_BLOCK_SIZE      4096u

/* Inode mode values */
#define AFS_MODE_FREE       0u
#define AFS_MODE_FILE       1u
#define AFS_MODE_DIR        2u

/* Directory entry type values */
#define AFS_TYPE_FILE       1u
#define AFS_TYPE_DIR        2u

/* ── Driver limits ───────────────────────────────────────────────────────── */

#define AFS_MAX_FILES       8u    /* simultaneous open file handles */

/* ── Public API ──────────────────────────────────────────────────────────── */

/* Mount AetherFS from virtio-blk device 1. Returns 0 on success, -1 on error.
 * Gracefully returns -1 if no second disk is attached. */
int  aetherfs_mount(void);

/* 1 if an AetherFS volume is mounted and ready. */
int  aetherfs_ready(void);

/* Open a file by absolute path (e.g. "/readme.txt", "/docs/about.txt").
 * Returns a handle [0, AFS_MAX_FILES) or -1 on error. */
int  aetherfs_open(const char *path);

/* Read up to len bytes from handle fh. Returns bytes read, 0 = EOF, -1 = error. */
int  aetherfs_read(int fh, u8 *buf, u32 len);

/* Close file handle. */
void aetherfs_close(int fh);

/* Fill buf with a formatted directory listing of the given path.
 * Returns bytes written or -1. */
int  aetherfs_readdir(const char *path, char *buf, u32 len);

#endif /* AETHER_AETHERFS_H */
