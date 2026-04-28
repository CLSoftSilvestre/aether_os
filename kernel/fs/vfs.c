/*
 * AetherOS — Virtual Filesystem Switch (Phase 5.2)
 * File: kernel/fs/vfs.c
 *
 * Mounts two filesystems:
 *   "/initrd"  → embedded CPIO initrd  (read-only, always available)
 *   "/"        → FAT32 on virtio-blk   (read-only, present when disk attached)
 *
 * Path routing:
 *   Path starts with "/initrd" → initrd backend
 *   Everything else            → FAT32 backend (if mounted), else -ENOENT
 *
 * File descriptors: vfd = VFS_FD_BASE + slot (200-215).
 * Each slot records which backend owns it so vfs_read/vfs_close dispatch correctly.
 */

#include "aether/vfs.h"
#include "aether/fat32.h"
#include "aether/initrd.h"
#include "aether/printk.h"
#include "aether/types.h"

/* ── fd table ────────────────────────────────────────────────────────────── */

typedef enum {
    VFS_BACK_NONE   = 0,
    VFS_BACK_INITRD = 1,
    VFS_BACK_FAT32  = 2,
} vfs_backend_t;

typedef struct {
    int          used;
    vfs_backend_t backend;
    union {
        /* initrd backend */
        struct {
            const u8 *data;
            u32       size;
            u32       pos;
        } ird;
        /* FAT32 backend */
        struct {
            int fh;   /* fat32 file handle 0-7 */
        } fat;
    };
} vfs_fd_t;

static vfs_fd_t g_fds[VFS_MAX_FD];

/* ── String helpers (no libc) ────────────────────────────────────────────── */

static int vfs_strncmp(const char *a, const char *b, int n)
{
    for (int i = 0; i < n; i++) {
        if (a[i] != b[i]) return (unsigned char)a[i] - (unsigned char)b[i];
        if (!a[i]) return 0;
    }
    return 0;
}

static int vfs_strlen(const char *s) { int n = 0; while (s[n]) n++; return n; }

static int is_initrd_path(const char *path)
{
    /* "/initrd" or "/initrd/" or "/initrd/..." */
    if (vfs_strncmp(path, "/initrd", 7) == 0) {
        char next = path[7];
        return (next == '\0' || next == '/');
    }
    return 0;
}

/* Strip the "/initrd" prefix for initrd_find / initrd_list */
static const char *initrd_subpath(const char *path)
{
    /* path = "/initrd" → "/" (root of initrd, no specific file) */
    /* path = "/initrd/foo" → "/foo" */
    if (vfs_strncmp(path, "/initrd", 7) == 0)
        return path + 7;   /* may be "" or "/foo" */
    return path;
}

/* ── Public: init ────────────────────────────────────────────────────────── */

void vfs_init(void)
{
    for (int i = 0; i < VFS_MAX_FD; i++) g_fds[i].used = 0;

    /* FAT32 is already mounted by fat32_mount() in main.c if disk is present */
    kinfo("vfs: initrd always mounted at /initrd\n");
    if (fat32_ready())
        kinfo("vfs: FAT32 disk mounted at /\n");
    else
        kinfo("vfs: no disk — FAT32 not available\n");
}

/* ── Public: open ────────────────────────────────────────────────────────── */

int vfs_open(const char *path)
{
    if (!path) return -1;

    /* Find free slot */
    int slot = -1;
    for (int i = 0; i < VFS_MAX_FD; i++) {
        if (!g_fds[i].used) { slot = i; break; }
    }
    if (slot < 0) return -1;

    vfs_fd_t *f = &g_fds[slot];

    if (is_initrd_path(path)) {
        const char *sub = initrd_subpath(path);
        /* Strip leading slash for initrd_find */
        if (sub[0] == '/') sub++;
        if (sub[0] == '\0') return -1;   /* can't open the initrd dir itself as a file */

        u32 size = 0;
        const void *data = initrd_find(sub, &size);
        if (!data) return -1;

        f->used          = 1;
        f->backend       = VFS_BACK_INITRD;
        f->ird.data      = (const u8 *)data;
        f->ird.size      = size;
        f->ird.pos       = 0;
        return VFS_FD_BASE + slot;
    }

    /* FAT32 path */
    if (!fat32_ready()) return -1;

    /* Strip leading slash — fat32_open expects relative or absolute */
    int fh = fat32_open(path);
    if (fh < 0) return -1;

    f->used      = 1;
    f->backend   = VFS_BACK_FAT32;
    f->fat.fh    = fh;
    return VFS_FD_BASE + slot;
}

/* ── Public: read ────────────────────────────────────────────────────────── */

int vfs_read(int vfd, u8 *buf, u32 len)
{
    if (!vfs_is_vfd(vfd)) return -1;
    int slot = vfd - VFS_FD_BASE;
    vfs_fd_t *f = &g_fds[slot];
    if (!f->used || !buf || len == 0) return -1;

    if (f->backend == VFS_BACK_INITRD) {
        if (f->ird.pos >= f->ird.size) return 0;   /* EOF */
        u32 avail = f->ird.size - f->ird.pos;
        if (len > avail) len = avail;
        const u8 *src = f->ird.data + f->ird.pos;
        for (u32 i = 0; i < len; i++) buf[i] = src[i];
        f->ird.pos += len;
        return (int)len;
    }

    if (f->backend == VFS_BACK_FAT32)
        return fat32_read(f->fat.fh, buf, len);

    return -1;
}

/* ── Public: close ───────────────────────────────────────────────────────── */

void vfs_close(int vfd)
{
    if (!vfs_is_vfd(vfd)) return;
    int slot = vfd - VFS_FD_BASE;
    vfs_fd_t *f = &g_fds[slot];
    if (!f->used) return;

    if (f->backend == VFS_BACK_FAT32)
        fat32_close(f->fat.fh);

    f->used = 0;
}

/* ── Public: readdir ─────────────────────────────────────────────────────── */

int vfs_readdir(const char *path, char *buf, u32 len)
{
    if (!path || !buf || len == 0) return -1;

    if (is_initrd_path(path)) {
        /* List initrd contents */
        return (int)initrd_list(buf, len);
    }

    /* FAT32 directory listing */
    if (!fat32_ready()) {
        /* Return helpful message if disk not present */
        const char *msg = "(no disk — attach a virtio-blk disk image)\n";
        int n = vfs_strlen(msg);
        if ((u32)n >= len) n = (int)len - 1;
        for (int i = 0; i < n; i++) buf[i] = msg[i];
        buf[n] = '\0';
        return n;
    }

    return fat32_readdir(path, buf, len);
}
