#ifndef AETHER_VFS_MOUNTS_H
#define AETHER_VFS_MOUNTS_H

/*
 * AetherOS — VFS mount discovery helper (Phase 5.6)
 *
 * Lightweight helper for enumerating the three known VFS mount points.
 * Uses sys_fs_readdir() to probe availability; no new syscall needed.
 */

#include <widget.h>   /* for TVICON_* constants */

typedef struct {
    const char   *label;      /* e.g. "FAT32 (/)" */
    const char   *path;       /* e.g. "/" */
    unsigned char icon_type;  /* TVICON_DRIVE_* */
    unsigned char available;  /* 1 if readdir succeeded */
} mount_info_t;

/* Probe all known mounts; marks available=1 on success. */
void vfs_probe_mounts(void);

/* Copy available mounts into out[]; returns count (0..max). */
int  vfs_get_mounts(mount_info_t *out, int max);

#endif /* AETHER_VFS_MOUNTS_H */
