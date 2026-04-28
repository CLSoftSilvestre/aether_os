/*
 * AetherOS — FAT32 read-only filesystem (Phase 5.2)
 * File: kernel/fs/fat32.c
 *
 * Reads volumes created by mkfs.fat / mformat.
 * Supports:
 *   - FAT32 BPB parsing
 *   - Cluster chain traversal via FAT
 *   - Short (8.3) and Long File Name (LFN) directory entries
 *   - Subdirectory traversal for multi-component paths
 *
 * All I/O goes through virtio_blk_read_sectors().
 * Write support is not implemented (EROFS).
 */

#include "aether/fat32.h"
#include "aether/printk.h"
#include "aether/types.h"
#include "drivers/block/virtio_blk.h"

/* ── BPB (BIOS Parameter Block) ─────────────────────────────────────────── */

typedef struct {
    u16 bytes_per_sector;
    u8  sectors_per_cluster;
    u16 reserved_sectors;
    u8  num_fats;
    u32 fat_size;           /* FAT size in sectors */
    u32 root_cluster;       /* first cluster of root directory */
    u32 fat_lba;            /* LBA of first FAT copy */
    u32 data_lba;           /* LBA of first data cluster (cluster 2) */
    u64 total_sectors;
} fat32_bpb_t;

/* FAT32 directory entry (32 bytes) */
typedef struct {
    u8  name[8];
    u8  ext[3];
    u8  attr;
    u8  nt_res;         /* bit3=base lowercase, bit4=ext lowercase */
    u8  create_cs;
    u16 create_time;
    u16 create_date;
    u16 access_date;
    u16 cluster_hi;
    u16 write_time;
    u16 write_date;
    u16 cluster_lo;
    u32 file_size;
} __attribute__((packed)) fat32_dirent_t;

/* FAT32 long-filename entry (32 bytes, attr == ATTR_LFN == 0x0F) */
typedef struct {
    u8  order;          /* sequence number, bit6 set on last (first in dir) */
    u8  name1[10];      /* 5 UCS-2 chars */
    u8  attr;           /* 0x0F */
    u8  type;
    u8  checksum;
    u8  name2[12];      /* 6 UCS-2 chars */
    u16 cluster;        /* always 0 */
    u8  name3[4];       /* 2 UCS-2 chars */
} __attribute__((packed)) fat32_lfn_t;

#define ATTR_READ_ONLY  0x01u
#define ATTR_HIDDEN     0x02u
#define ATTR_SYSTEM     0x04u
#define ATTR_VOLUME_ID  0x08u
#define ATTR_DIRECTORY  0x10u
#define ATTR_ARCHIVE    0x20u
#define ATTR_LFN        0x0Fu   /* all four low attribute bits set */

#define FAT32_EOC       0x0FFFFFF8u   /* end-of-chain marker */
#define FAT32_BAD       0x0FFFFFF7u

/* ── Open file handle ────────────────────────────────────────────────────── */

typedef struct {
    int  used;
    u32  first_cluster;
    u32  cur_cluster;
    u32  file_size;
    u32  pos;               /* byte position within file */
    u32  cluster_pos;       /* byte offset within cur_cluster */
} fat32_file_t;

/* ── Static state ────────────────────────────────────────────────────────── */

static fat32_bpb_t  g_bpb;
static int          g_mounted;
static fat32_file_t g_files[FAT32_MAX_FILES];

/* Sector-sized read buffer (512 bytes, reused across all reads) */
static u8 g_sector_buf[512] __attribute__((aligned(4)));

/* LFN accumulation buffer — up to 20 LFN entries × 13 chars + NUL */
static char g_lfn_buf[261];
static int  g_lfn_valid;

/* ── Helpers ─────────────────────────────────────────────────────────────── */

static int read_sector(u64 lba, u8 *buf)
{
    return virtio_blk_read_sectors(lba, 1, buf);
}

static u32 cluster_to_lba(u32 cluster)
{
    return g_bpb.data_lba + (cluster - 2u) * (u32)g_bpb.sectors_per_cluster;
}

/* Read one 4-byte FAT32 entry for 'cluster', return next-cluster value. */
static u32 fat_entry(u32 cluster)
{
    u32 byte_offset = cluster * 4u;
    u32 sector_idx  = byte_offset / g_bpb.bytes_per_sector;
    u32 byte_in_sec = byte_offset % g_bpb.bytes_per_sector;
    u64 lba         = (u64)(g_bpb.fat_lba + sector_idx);

    if (read_sector(lba, g_sector_buf) != 0) return 0;

    u32 entry = (u32)g_sector_buf[byte_in_sec]
              | ((u32)g_sector_buf[byte_in_sec+1] << 8)
              | ((u32)g_sector_buf[byte_in_sec+2] << 16)
              | ((u32)g_sector_buf[byte_in_sec+3] << 24);
    return entry & 0x0FFFFFFFu;
}

/* Extract a single UCS-2 char from an LFN byte pair (low byte only = ASCII). */
static char lfn_char(const u8 *p) { return (char)p[0]; }

/* Append LFN characters from one LFN entry (order byte already validated). */
static void lfn_accumulate(const fat32_lfn_t *e)
{
    /* Order: sequence 1..N in directory; last entry has bit6 set.
     * Characters go into g_lfn_buf at positions (seq-1)*13 .. seq*13-1. */
    int seq = (int)(e->order & 0x1Fu);   /* 1-based sequence number */
    if (seq < 1 || seq > 20) return;

    int base = (seq - 1) * 13;
    /* name1 = 5 chars at bytes 1..10 */
    for (int i = 0; i < 5; i++) {
        char c = lfn_char(e->name1 + i * 2);
        if (base + i < 260) g_lfn_buf[base + i] = c;
    }
    /* name2 = 6 chars at bytes 14..25 */
    for (int i = 0; i < 6; i++) {
        char c = lfn_char(e->name2 + i * 2);
        if (base + 5 + i < 260) g_lfn_buf[base + 5 + i] = c;
    }
    /* name3 = 2 chars at bytes 28..31 */
    for (int i = 0; i < 2; i++) {
        char c = lfn_char(e->name3 + i * 2);
        if (base + 11 + i < 260) g_lfn_buf[base + 11 + i] = c;
    }
}

/* Build final short-name string from a directory entry, applying NTRes case flags. */
static void short_name_to_str(const fat32_dirent_t *de, char *out, int max)
{
    int n = 0;
    /* Base name (8 chars, space-padded) */
    int base_lower = (de->nt_res & 0x08u) != 0;
    for (int i = 0; i < 8 && de->name[i] != ' ' && n < max - 1; i++) {
        char c = (char)de->name[i];
        if (base_lower && c >= 'A' && c <= 'Z') c = (char)(c + 32);
        out[n++] = c;
    }
    /* Extension (3 chars, space-padded) */
    int has_ext = 0;
    for (int i = 0; i < 3; i++) if (de->ext[i] != ' ') { has_ext = 1; break; }
    if (has_ext && n < max - 1) {
        int ext_lower = (de->nt_res & 0x10u) != 0;
        out[n++] = '.';
        for (int i = 0; i < 3 && de->ext[i] != ' ' && n < max - 1; i++) {
            char c = (char)de->ext[i];
            if (ext_lower && c >= 'A' && c <= 'Z') c = (char)(c + 32);
            out[n++] = c;
        }
    }
    out[n] = '\0';
}

/* Case-insensitive ASCII compare. */
static int fat_streq_ci(const char *a, const char *b)
{
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca + 32);
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb + 32);
        if (ca != cb) return 0;
        a++; b++;
    }
    return *a == *b;
}

/* String length */
static int fat_strlen(const char *s) { int n=0; while(s[n]) n++; return n; }

/* ── Directory iterator ──────────────────────────────────────────────────── */

/*
 * Callback type for iterate_dir().
 * Return 1 to stop iteration (entry found), 0 to continue.
 */
typedef int (*dir_cb_t)(const char *name, const fat32_dirent_t *de, void *ctx);

/*
 * Iterate all entries in the directory starting at 'dir_cluster'.
 * For each real (non-deleted, non-volume-label) entry, calls cb(name, de, ctx).
 * If cb returns 1, stops and returns 1.
 */
static int iterate_dir(u32 dir_cluster, dir_cb_t cb, void *ctx)
{
    u8 cluster_buf[512 * 8];    /* up to 8 sectors per cluster */
    u32 spc = g_bpb.sectors_per_cluster;
    if (spc > 8) spc = 8;       /* safety cap (should be 1..8 for typical FAT32) */
    u32 bytes_per_cluster = spc * 512u;

    g_lfn_valid = 0;
    for (int i = 0; i < 261; i++) g_lfn_buf[i] = '\0';

    u32 cluster = dir_cluster;

    while (cluster >= 2u && cluster < FAT32_EOC) {
        u32 lba = cluster_to_lba(cluster);
        /* Read all sectors of this cluster */
        for (u32 s = 0; s < spc; s++) {
            if (virtio_blk_read_sectors((u64)(lba + s), 1, cluster_buf + s * 512u) != 0)
                return 0;
        }

        /* Walk 32-byte directory entries within the cluster */
        for (u32 off = 0; off < bytes_per_cluster; off += 32u) {
            fat32_dirent_t *de = (fat32_dirent_t *)(cluster_buf + off);

            if (de->name[0] == 0x00u) goto done;   /* end of directory */
            if (de->name[0] == 0xE5u) {            /* deleted entry */
                g_lfn_valid = 0;
                for (int i = 0; i < 261; i++) g_lfn_buf[i] = '\0';
                continue;
            }
            if (de->attr == ATTR_LFN) {
                /* LFN entry — accumulate characters */
                const fat32_lfn_t *lfn = (const fat32_lfn_t *)(cluster_buf + off);
                if (lfn->order & 0x40u) {
                    /* This is the first LFN entry (last in file order) — reset */
                    for (int i = 0; i < 261; i++) g_lfn_buf[i] = '\0';
                }
                lfn_accumulate(lfn);
                g_lfn_valid = 1;
                continue;
            }
            /* Skip volume ID entries */
            if (de->attr & ATTR_VOLUME_ID) {
                g_lfn_valid = 0;
                continue;
            }

            /* Build display name */
            char name[261];
            if (g_lfn_valid && g_lfn_buf[0]) {
                /* Use LFN — find NUL terminator */
                int n = 0;
                while (n < 260 && g_lfn_buf[n]) { name[n] = g_lfn_buf[n]; n++; }
                name[n] = '\0';
            } else {
                short_name_to_str(de, name, 261);
            }

            /* Reset LFN state for next entry */
            g_lfn_valid = 0;
            for (int i = 0; i < 261; i++) g_lfn_buf[i] = '\0';

            /* Skip dot entries */
            if (name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0')))
                continue;

            if (cb(name, de, ctx)) return 1;
        }

        cluster = fat_entry(cluster);
    }

done:
    return 0;
}

/* ── Lookup: find entry by name in a directory ───────────────────────────── */

typedef struct {
    const char *target;
    u32         first_cluster;
    u32         file_size;
    u8          attr;
    int         found;
} lookup_ctx_t;

static int lookup_cb(const char *name, const fat32_dirent_t *de, void *ctx)
{
    lookup_ctx_t *lc = (lookup_ctx_t *)ctx;
    if (!fat_streq_ci(name, lc->target)) return 0;
    lc->first_cluster = ((u32)de->cluster_hi << 16) | (u32)de->cluster_lo;
    lc->file_size     = de->file_size;
    lc->attr          = de->attr;
    lc->found         = 1;
    return 1;   /* stop */
}

/*
 * Walk a path like "/docs/readme.txt" from the root cluster.
 * Returns the first cluster and file_size of the target, or -1.
 */
static int lookup_path(const char *path, u32 *cluster_out,
                       u32 *size_out, u8 *attr_out)
{
    /* Strip leading slashes */
    while (*path == '/') path++;

    u32 dir_cluster = g_bpb.root_cluster;

    /* Walk each path component */
    while (*path) {
        /* Extract next component */
        char component[261];
        int n = 0;
        while (*path && *path != '/' && n < 260)
            component[n++] = *path++;
        component[n] = '\0';
        while (*path == '/') path++;

        /* Look up component in dir_cluster */
        lookup_ctx_t lc = { component, 0, 0, 0, 0 };
        iterate_dir(dir_cluster, lookup_cb, &lc);
        if (!lc.found) return -1;

        if (*path) {
            /* More components to go — must be a directory */
            if (!(lc.attr & ATTR_DIRECTORY)) return -1;
            dir_cluster = lc.first_cluster;
        } else {
            /* Last component found */
            *cluster_out = lc.first_cluster;
            if (size_out) *size_out = lc.file_size;
            if (attr_out) *attr_out = lc.attr;
            return 0;
        }
    }

    /* Empty path → root directory */
    *cluster_out = g_bpb.root_cluster;
    if (size_out) *size_out = 0;
    if (attr_out) *attr_out = ATTR_DIRECTORY;
    return 0;
}

/* ── Public: mount ───────────────────────────────────────────────────────── */

int fat32_mount(void)
{
    if (!virtio_blk_ready()) return -1;

    u8 buf[512];
    if (read_sector(0, buf) != 0) {
        kerror("fat32: failed to read boot sector\n");
        return -1;
    }

    /* Verify FAT32 signature and FS type string */
    if (buf[510] != 0x55u || buf[511] != 0xAAu) {
        kerror("fat32: invalid boot sector signature\n");
        return -1;
    }
    /* FAT32 FS type at offset 82 */
    if (buf[82] != 'F' || buf[83] != 'A' || buf[84] != 'T') {
        kerror("fat32: not a FAT32 volume (fstype='%c%c%c')\n",
               buf[82], buf[83], buf[84]);
        return -1;
    }

    /* Parse BPB */
    g_bpb.bytes_per_sector   = (u16)buf[11] | ((u16)buf[12] << 8);
    g_bpb.sectors_per_cluster = buf[13];
    g_bpb.reserved_sectors   = (u16)buf[14] | ((u16)buf[15] << 8);
    g_bpb.num_fats            = buf[16];

    /* FAT32 extended BPB */
    g_bpb.fat_size    = (u32)buf[36] | ((u32)buf[37]<<8) | ((u32)buf[38]<<16) | ((u32)buf[39]<<24);
    g_bpb.root_cluster= (u32)buf[44] | ((u32)buf[45]<<8) | ((u32)buf[46]<<16) | ((u32)buf[47]<<24);
    g_bpb.total_sectors=(u64)((u32)buf[32]|((u32)buf[33]<<8)|((u32)buf[34]<<16)|((u32)buf[35]<<24));

    g_bpb.fat_lba  = (u32)g_bpb.reserved_sectors;
    g_bpb.data_lba = g_bpb.fat_lba + (u32)g_bpb.num_fats * g_bpb.fat_size;

    if (g_bpb.bytes_per_sector != 512u) {
        kerror("fat32: unsupported bytes_per_sector=%u\n",
               (unsigned)g_bpb.bytes_per_sector);
        return -1;
    }

    /* Volume label at offset 71 */
    char label[12];
    for (int i = 0; i < 11; i++) label[i] = (char)buf[71+i];
    label[11] = '\0';

    kinfo("fat32: mounted — label=\"%s\" root_cluster=%u data_lba=%u\n",
          label, (unsigned)g_bpb.root_cluster, (unsigned)g_bpb.data_lba);

    g_mounted = 1;
    return 0;
}

int fat32_ready(void) { return g_mounted; }

/* ── Public: open ────────────────────────────────────────────────────────── */

int fat32_open(const char *path)
{
    if (!g_mounted || !path) return -1;

    u32 cluster = 0, size = 0;
    u8  attr    = 0;
    if (lookup_path(path, &cluster, &size, &attr) != 0) return -1;
    if (attr & ATTR_DIRECTORY) return -1;   /* directories are not files */

    /* Find a free handle */
    for (int i = 0; i < FAT32_MAX_FILES; i++) {
        if (!g_files[i].used) {
            g_files[i].used          = 1;
            g_files[i].first_cluster = cluster;
            g_files[i].cur_cluster   = cluster;
            g_files[i].file_size     = size;
            g_files[i].pos           = 0;
            g_files[i].cluster_pos   = 0;
            return i;
        }
    }
    return -1;   /* too many open files */
}

/* ── Public: read ────────────────────────────────────────────────────────── */

int fat32_read(int fh, u8 *buf, u32 len)
{
    if (fh < 0 || fh >= FAT32_MAX_FILES || !g_files[fh].used) return -1;
    fat32_file_t *f = &g_files[fh];
    if (!buf || len == 0) return 0;
    if (f->pos >= f->file_size) return 0;   /* EOF */

    /* Cap read at remaining file bytes */
    u32 remaining = f->file_size - f->pos;
    if (len > remaining) len = remaining;

    u32 bytes_read = 0;
    u32 spc        = g_bpb.sectors_per_cluster;
    u32 cluster_sz = spc * 512u;

    while (bytes_read < len && f->cur_cluster >= 2u && f->cur_cluster < FAT32_EOC) {
        /* Determine byte offset within current cluster and how much to copy */
        u32 off_in_cluster = f->cluster_pos;
        u32 avail          = cluster_sz - off_in_cluster;
        u32 want           = len - bytes_read;
        if (want > avail) want = avail;

        /* Which sector within the cluster, and offset within that sector */
        u32 sector_idx    = off_in_cluster / 512u;
        u32 off_in_sector = off_in_cluster % 512u;
        u32 avail_sector  = 512u - off_in_sector;
        if (want > avail_sector) want = avail_sector;

        u64 lba = (u64)(cluster_to_lba(f->cur_cluster) + sector_idx);
        u8  tmp[512];
        if (read_sector(lba, tmp) != 0) return -1;

        for (u32 i = 0; i < want; i++)
            buf[bytes_read + i] = tmp[off_in_sector + i];

        bytes_read       += want;
        f->pos           += want;
        f->cluster_pos   += want;

        if (f->cluster_pos >= cluster_sz) {
            /* Advance to next cluster */
            f->cluster_pos = 0;
            f->cur_cluster = fat_entry(f->cur_cluster);
        }
    }

    return (int)bytes_read;
}

/* ── Public: close ───────────────────────────────────────────────────────── */

void fat32_close(int fh)
{
    if (fh >= 0 && fh < FAT32_MAX_FILES)
        g_files[fh].used = 0;
}

/* ── Public: readdir ─────────────────────────────────────────────────────── */

typedef struct {
    char *buf;
    u32   len;
    u32   out;
    int   is_root;   /* 1 for root, so we prefix "/" */
} readdir_ctx_t;

static void append_str(readdir_ctx_t *rc, const char *s)
{
    while (*s && rc->out + 1u < rc->len)
        rc->buf[rc->out++] = *s++;
}

static void append_u32(readdir_ctx_t *rc, u32 v)
{
    char tmp[12]; int n = 0;
    if (v == 0) { tmp[n++] = '0'; }
    else { while (v) { tmp[n++] = (char)('0' + v % 10); v /= 10; } }
    for (int k = n-1; k >= 0 && rc->out + 1u < rc->len; k--)
        rc->buf[rc->out++] = tmp[k];
}

static int readdir_cb(const char *name, const fat32_dirent_t *de, void *ctx)
{
    readdir_ctx_t *rc = (readdir_ctx_t *)ctx;
    int is_dir = (de->attr & ATTR_DIRECTORY) != 0;

    if (is_dir) {
        append_str(rc, "[");
        append_str(rc, name);
        append_str(rc, "]");
    } else {
        int nl = fat_strlen(name);
        append_str(rc, name);
        /* Pad name to column 20 */
        for (int i = nl; i < 20 && rc->out + 1u < rc->len; i++)
            rc->buf[rc->out++] = ' ';
        append_u32(rc, de->file_size);
        append_str(rc, " bytes");
    }
    if (rc->out + 1u < rc->len) rc->buf[rc->out++] = '\n';
    return 0;
}

int fat32_readdir(const char *path, char *buf, u32 len)
{
    if (!g_mounted || !buf || len == 0) return -1;

    u32 cluster = g_bpb.root_cluster;
    u8  attr    = ATTR_DIRECTORY;

    /* Strip leading slashes; empty path = root */
    const char *p = path;
    while (*p == '/') p++;

    if (*p != '\0') {
        u32 sz = 0;
        if (lookup_path(p, &cluster, &sz, &attr) != 0) return -1;
        if (!(attr & ATTR_DIRECTORY)) return -1;
    }

    readdir_ctx_t rc = { buf, len, 0, (*p == '\0') };
    iterate_dir(cluster, readdir_cb, &rc);

    if (rc.out < len) buf[rc.out] = '\0';
    return (int)rc.out;
}
