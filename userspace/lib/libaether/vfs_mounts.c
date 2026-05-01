/*
 * AetherOS — VFS mount discovery (Phase 5.6)
 * File: userspace/lib/libaether/vfs_mounts.c
 */

#include <vfs_mounts.h>
#include <sys.h>

static mount_info_t g_mounts[3] = {
    { "FAT32 (/)",         "/",       TVICON_DRIVE_FAT32,  0 },
    { "InitRD (/initrd)",  "/initrd", TVICON_DRIVE_INITRD, 0 },
    { "AetherFS (/afs)",   "/afs",    TVICON_DRIVE_AFS,    0 },
};

void vfs_probe_mounts(void)
{
    char buf[64];
    for (int i = 0; i < 3; i++) {
        long n = sys_fs_readdir(g_mounts[i].path, buf, sizeof(buf) - 1);
        g_mounts[i].available = (n >= 0) ? 1 : 0;
    }
}

int vfs_get_mounts(mount_info_t *out, int max)
{
    int count = 0;
    for (int i = 0; i < 3 && count < max; i++) {
        if (g_mounts[i].available)
            out[count++] = g_mounts[i];
    }
    return count;
}
