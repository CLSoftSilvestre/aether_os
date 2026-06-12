/*
 * AetherOS — Virtual Filesystem Switch (Phase 5.2)
 * File: kernel/fs/vfs.c
 *
 * Mounts four filesystems:
 *   "/initrd"  → embedded CPIO initrd  (read-only, always available)
 *   "/"        → FAT32 on virtio-blk 0 (read-only, when disk.img attached)
 *   "/afs"     → AetherFS on virtio-blk 1 (read-only, when afs.img attached)
 *   "/usb"     → FAT32 on USB MSC disk   (read-only, when USB disk present)
 *
 * Path routing:
 *   Starts with "/initrd" → initrd backend
 *   Starts with "/afs"   → AetherFS backend
 *   Starts with "/usb"   → USB FAT32 backend
 *   Everything else       → FAT32 backend (if mounted), else -ENOENT
 *
 * File descriptors: vfd = VFS_FD_BASE + slot (200-215).
 * Each slot records which backend owns it so vfs_read/vfs_close dispatch correctly.
 */

#include "aether/vfs.h"
#include "aether/fat32.h"
#include "aether/aetherfs.h"
#include "aether/initrd.h"
#include "aether/usb_fat32.h"
#include "aether/printk.h"
#include "aether/types.h"
#include "aether/spinlock.h"

/* ── fd table ────────────────────────────────────────────────────────────── */

typedef enum {
    VFS_BACK_NONE   = 0,
    VFS_BACK_INITRD = 1,
    VFS_BACK_FAT32  = 2,
    VFS_BACK_AFS    = 3,
    VFS_BACK_USB    = 4,
} vfs_backend_t;

typedef struct {
    int          used;
    vfs_backend_t backend;
    union {
        struct {
            const u8 *data;
            u32       size;
            u32       pos;
        } ird;
        struct { int fh; } fat;
        struct { int fh; } afs;
        struct { int fh; } usb;
    };
} vfs_fd_t;

static vfs_fd_t g_fds[VFS_MAX_FD];

/*
 * Protects slot allocation/release in g_fds[].  vfs_open/create/close do a
 * check-then-act on g_fds[i].used; without this, two cores can claim the same
 * slot and alias one fd → table corruption.  Held across the backend open
 * (fat32/afs/usb), which take their own locks — order is always vfs→backend,
 * never the reverse, so no deadlock.  vfs_read/write touch only an already-open
 * distinct slot and need no lock.
 */
static spinlock_t g_vfs_lock = SPINLOCK_INIT;

/* ── String helpers ──────────────────────────────────────────────────────── */

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
    if (vfs_strncmp(path, "/initrd", 7) == 0) {
        char next = path[7];
        return (next == '\0' || next == '/');
    }
    return 0;
}

static int is_afs_path(const char *path)
{
    if (vfs_strncmp(path, "/afs", 4) == 0) {
        char next = path[4];
        return (next == '\0' || next == '/');
    }
    return 0;
}

static const char *initrd_subpath(const char *path)
{
    if (vfs_strncmp(path, "/initrd", 7) == 0)
        return path + 7;
    return path;
}

/* Strip "/afs" prefix, return the remainder (e.g. "/readme.txt" or ""). */
static const char *afs_subpath(const char *path)
{
    if (vfs_strncmp(path, "/afs", 4) == 0)
        return path + 4;   /* may be "" or "/foo" */
    return path;
}

static int is_usb_path(const char *path)
{
    if (vfs_strncmp(path, "/usb", 4) == 0) {
        char next = path[4];
        return (next == '\0' || next == '/');
    }
    return 0;
}

/* Strip "/usb" prefix, return the remainder (e.g. "/music.wav" or ""). */
static const char *usb_subpath(const char *path)
{
    if (vfs_strncmp(path, "/usb", 4) == 0)
        return path + 4;   /* may be "" or "/foo" */
    return path;
}

/* ── Public: init ────────────────────────────────────────────────────────── */

void vfs_init(void)
{
    for (int i = 0; i < VFS_MAX_FD; i++) g_fds[i].used = 0;

    kinfo("vfs: initrd always mounted at /initrd\n");
    if (fat32_ready())
        kinfo("vfs: FAT32 mounted at /\n");
    else
        kinfo("vfs: no FAT32 disk — / unavailable\n");
    if (aetherfs_ready())
        kinfo("vfs: AetherFS mounted at /afs\n");
    else
        kinfo("vfs: no AetherFS disk — /afs unavailable\n");
    if (usb_fat32_ready())
        kinfo("vfs: USB FAT32 mounted at /usb\n");
    else
        kinfo("vfs: no USB disk — /usb unavailable\n");
}

/* ── Public: open ────────────────────────────────────────────────────────── */

static int vfs_open_locked(const char *path)
{
    if (!path) return -1;

    int slot = -1;
    for (int i = 0; i < VFS_MAX_FD; i++) {
        if (!g_fds[i].used) { slot = i; break; }
    }
    if (slot < 0) {
        kwarn("vfs_open: no free fd slot (all %d busy) for '%s'\n", VFS_MAX_FD, path);
        return -1;
    }

    vfs_fd_t *f = &g_fds[slot];

    /* ── initrd ── */
    if (is_initrd_path(path)) {
        const char *sub = initrd_subpath(path);
        if (sub[0] == '/') sub++;
        if (sub[0] == '\0') return -1;

        u32 size = 0;
        const void *data = initrd_find(sub, &size);
        if (!data) return -1;

        f->used     = 1;
        f->backend  = VFS_BACK_INITRD;
        f->ird.data = (const u8 *)data;
        f->ird.size = size;
        f->ird.pos  = 0;
        return VFS_FD_BASE + slot;
    }

    /* ── AetherFS ── */
    if (is_afs_path(path)) {
        if (!aetherfs_ready()) return -1;
        const char *sub = afs_subpath(path);
        if (sub[0] == '\0') return -1;   /* cannot open /afs dir itself as file */

        int fh = aetherfs_open(sub);
        if (fh < 0) return -1;

        f->used     = 1;
        f->backend  = VFS_BACK_AFS;
        f->afs.fh   = fh;
        return VFS_FD_BASE + slot;
    }

    /* ── USB FAT32 ── */
    if (is_usb_path(path)) {
        if (!usb_fat32_ready()) return -1;
        const char *sub = usb_subpath(path);
        if (sub[0] == '\0') return -1;   /* cannot open /usb dir itself as file */

        int fh = usb_fat32_open(sub);
        if (fh < 0) return -1;

        f->used     = 1;
        f->backend  = VFS_BACK_USB;
        f->usb.fh   = fh;
        return VFS_FD_BASE + slot;
    }

    /* ── FAT32 ── */
    if (!fat32_ready()) return -1;

    int fh = fat32_open(path);
    if (fh < 0) {
        kwarn("vfs_open: fat32_open('%s') failed\n", path);
        return -1;
    }

    f->used    = 1;
    f->backend = VFS_BACK_FAT32;
    f->fat.fh  = fh;
    return VFS_FD_BASE + slot;
}

int vfs_open(const char *path)
{
    spin_lock(&g_vfs_lock);
    int r = vfs_open_locked(path);
    spin_unlock(&g_vfs_lock);
    return r;
}

/* ── Public: read ────────────────────────────────────────────────────────── */

int vfs_read(int vfd, u8 *buf, u32 len)
{
    if (!vfs_is_vfd(vfd)) return -1;
    int slot = vfd - VFS_FD_BASE;
    vfs_fd_t *f = &g_fds[slot];
    if (!f->used || !buf || len == 0) return -1;

    if (f->backend == VFS_BACK_INITRD) {
        if (f->ird.pos >= f->ird.size) return 0;
        u32 avail = f->ird.size - f->ird.pos;
        if (len > avail) len = avail;
        const u8 *src = f->ird.data + f->ird.pos;
        for (u32 i = 0; i < len; i++) buf[i] = src[i];
        f->ird.pos += len;
        return (int)len;
    }

    if (f->backend == VFS_BACK_FAT32)
        return fat32_read(f->fat.fh, buf, len);

    if (f->backend == VFS_BACK_AFS)
        return aetherfs_read(f->afs.fh, buf, len);

    if (f->backend == VFS_BACK_USB)
        return usb_fat32_read(f->usb.fh, buf, len);

    return -1;
}

/* ── Public: close ───────────────────────────────────────────────────────── */

void vfs_close(int vfd)
{
    if (!vfs_is_vfd(vfd)) return;
    int slot = vfd - VFS_FD_BASE;
    vfs_fd_t *f = &g_fds[slot];

    spin_lock(&g_vfs_lock);
    if (!f->used) { spin_unlock(&g_vfs_lock); return; }

    if (f->backend == VFS_BACK_FAT32)
        fat32_close(f->fat.fh);
    else if (f->backend == VFS_BACK_AFS)
        aetherfs_close(f->afs.fh);
    else if (f->backend == VFS_BACK_USB)
        usb_fat32_close(f->usb.fh);

    f->used = 0;
    spin_unlock(&g_vfs_lock);
}

/* ── Public: create ──────────────────────────────────────────────────────── */

static int vfs_create_locked(const char *path)
{
    /* Only FAT32 is writable; initrd, AetherFS, and USB are read-only */
    if (!path) return -1;
    if (is_initrd_path(path) || is_afs_path(path) || is_usb_path(path)) return -1;
    if (!fat32_ready()) {
        kwarn("vfs_create: FAT32 not ready for '%s'\n", path);
        return -1;
    }

    int slot = -1;
    for (int i = 0; i < VFS_MAX_FD; i++) {
        if (!g_fds[i].used) { slot = i; break; }
    }
    if (slot < 0) {
        kwarn("vfs_create: no free fd slot for '%s'\n", path);
        return -1;
    }

    int fh = fat32_create(path);
    if (fh < 0) {
        kwarn("vfs_create: fat32_create('%s') failed\n", path);
        return -1;
    }

    kwarn("vfs_create: '%s' ok vfd=%d fat_fh=%d\n", path, VFS_FD_BASE + slot, fh);
    vfs_fd_t *f = &g_fds[slot];
    f->used    = 1;
    f->backend = VFS_BACK_FAT32;
    f->fat.fh  = fh;
    return VFS_FD_BASE + slot;
}

int vfs_create(const char *path)
{
    spin_lock(&g_vfs_lock);
    int r = vfs_create_locked(path);
    spin_unlock(&g_vfs_lock);
    return r;
}

/* ── Public: mkdir ───────────────────────────────────────────────────────── */

int vfs_mkdir(const char *path)
{
    if (!path) return -1;
    if (is_initrd_path(path) || is_afs_path(path) || is_usb_path(path)) return -1;
    if (!fat32_ready()) return -1;
    return fat32_mkdir(path);
}

/* ── Public: rm ───────────────────────────────────────────────────────── */

int vfs_rm(const char *path)
{
    if (!path) return -1;
    if (is_initrd_path(path) || is_afs_path(path) || is_usb_path(path)) return -1;
    if (!fat32_ready()) return -1;
    return fat32_remove(path);
}

/* ── Public: write ───────────────────────────────────────────────────────── */

int vfs_write(int vfd, const u8 *buf, u32 len)
{
    if (!vfs_is_vfd(vfd)) return -1;
    int slot = vfd - VFS_FD_BASE;
    vfs_fd_t *f = &g_fds[slot];
    if (!f->used || !buf || len == 0) return -1;

    if (f->backend == VFS_BACK_FAT32)
        return fat32_write(f->fat.fh, buf, len);

    return -1;   /* initrd and AetherFS are read-only */
}

/* ── Public: readdir ─────────────────────────────────────────────────────── */

int vfs_readdir(const char *path, char *buf, u32 len)
{
    if (!path || !buf || len == 0) return -1;

    if (is_initrd_path(path))
        return (int)initrd_list(buf, len);

    if (is_afs_path(path)) {
        if (!aetherfs_ready()) {
            const char *msg = "(no AetherFS disk — attach afs.img as hd1)\n";
            int n = vfs_strlen(msg);
            if ((u32)n >= len) n = (int)len - 1;
            for (int i = 0; i < n; i++) buf[i] = msg[i];
            buf[n] = '\0';
            return n;
        }
        const char *sub = afs_subpath(path);
        if (sub[0] == '\0') sub = "/";   /* bare "/afs" → list root */
        return aetherfs_readdir(sub, buf, len);
    }

    if (is_usb_path(path)) {
        if (!usb_fat32_ready()) {
            const char *msg = "(no USB disk — attach a USB FAT32 image)\n";
            int n = vfs_strlen(msg);
            if ((u32)n >= len) n = (int)len - 1;
            for (int i = 0; i < n; i++) buf[i] = msg[i];
            buf[n] = '\0';
            return n;
        }
        const char *sub = usb_subpath(path);
        if (sub[0] == '\0') sub = "/";   /* bare "/usb" → list root */
        return usb_fat32_readdir(sub, buf, len);
    }

    /* FAT32 */
    if (!fat32_ready()) {
        const char *msg = "(no disk — attach a virtio-blk disk image)\n";
        int n = vfs_strlen(msg);
        if ((u32)n >= len) n = (int)len - 1;
        for (int i = 0; i < n; i++) buf[i] = msg[i];
        buf[n] = '\0';
        return n;
    }
    return fat32_readdir(path, buf, len);
}
