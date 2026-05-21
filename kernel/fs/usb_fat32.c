/*
 * AetherOS — USB FAT32 read-only filesystem (Phase 5.2.12)
 * File: kernel/fs/usb_fat32.c
 *
 * Self-contained FAT32 reader backed by usb_msc_read_sectors().
 * Mounted at "/usb" in the VFS.
 *
 * Supports: FAT32 BPB, cluster chain traversal, short (8.3) and LFN
 * directory entries, multi-level subdirectory paths.
 * No write support.
 */

#include "aether/usb_fat32.h"
#include "drivers/usb/msc.h"
#include "aether/printk.h"
#include "aether/types.h"

/* ── BPB ─────────────────────────────────────────────────────────────────── */

typedef struct {
    u16 bytes_per_sector;
    u8  sectors_per_cluster;
    u16 reserved_sectors;
    u8  num_fats;
    u32 fat_size;
    u32 root_cluster;
    u32 fat_lba;
    u32 data_lba;
    u64 total_sectors;
} uf_bpb_t;

/* FAT32 directory entry */
typedef struct {
    u8  name[8]; u8 ext[3];
    u8  attr; u8 nt_res; u8 create_cs;
    u16 create_time; u16 create_date; u16 access_date;
    u16 cluster_hi;
    u16 write_time; u16 write_date;
    u16 cluster_lo; u32 file_size;
} __attribute__((packed)) uf_dirent_t;

typedef struct {
    u8 order; u8 name1[10]; u8 attr; u8 type; u8 checksum;
    u8 name2[12]; u16 cluster; u8 name3[4];
} __attribute__((packed)) uf_lfn_t;

#define UF_ATTR_DIR  0x10u
#define UF_ATTR_LFN  0x0Fu
#define UF_EOC       0x0FFFFFF8u

/* ── File handles ─────────────────────────────────────────────────────────── */

typedef struct {
    int  used;
    u32  first_cluster;
    u32  cur_cluster;
    u32  file_size;
    u32  pos;
    u32  cluster_pos;
} uf_file_t;

/* ── Static state ─────────────────────────────────────────────────────────── */

static uf_bpb_t  g_bpb;
static int       g_mounted;
static uf_file_t g_files[USB_FAT32_MAX_FILES];
static u8        g_sec_buf[512]  __attribute__((aligned(4)));
static char      g_lfn_buf[261];
static int       g_lfn_valid;

/* ── Helpers ──────────────────────────────────────────────────────────────── */

static int uf_strlen(const char *s)
{
    int n = 0; while (s[n]) n++; return n;
}


static char uf_toupper(char c)
{
    return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
}

static int read_sector(u64 lba, u8 *buf)
{
    return usb_msc_read_sectors(lba, 1, buf);
}

static u32 cluster_to_lba(u32 cluster)
{
    return g_bpb.data_lba + (cluster - 2u) * (u32)g_bpb.sectors_per_cluster;
}

static u32 fat_entry(u32 cluster)
{
    u32 byte_offset  = cluster * 4u;
    u32 sector_idx   = byte_offset / 512u;
    u32 sector_off   = byte_offset % 512u;
    u64 lba          = g_bpb.fat_lba + sector_idx;

    if (read_sector(lba, g_sec_buf) != 0) return UF_EOC;
    u32 v = *(u32 *)&g_sec_buf[sector_off];
    return v & 0x0FFFFFFFu;
}

/* ── LFN helpers ─────────────────────────────────────────────────────────── */

static void lfn_accumulate(const uf_lfn_t *l)
{
    int seq = (l->order & 0x3Fu) - 1;
    if (seq < 0 || seq >= 20) return;
    int base = seq * 13;
    if (base + 13 > 260) return;

    for (int i = 0; i < 5;  i++) g_lfn_buf[base +  i] = l->name1[i * 2];
    for (int i = 0; i < 6;  i++) g_lfn_buf[base +  5 + i] = l->name2[i * 2];
    for (int i = 0; i < 2;  i++) g_lfn_buf[base + 11 + i] = l->name3[i * 2];

    if (l->order & 0x40u) {   /* last LFN entry (first encountered) */
        g_lfn_buf[base + 13] = '\0';
        g_lfn_valid = 1;
    }
}

/* ── Short name comparison ─────────────────────────────────────────────────── */

static int short_name_match(const uf_dirent_t *de, const char *name)
{
    /* Build "NAME.EXT" from 8.3 field, NUL-terminate, compare case-insensitively */
    char buf[13];
    int  n = 0;
    for (int i = 0; i < 8 && de->name[i] != ' '; i++)
        buf[n++] = uf_toupper((char)de->name[i]);

    if (de->ext[0] != ' ') {
        buf[n++] = '.';
        for (int i = 0; i < 3 && de->ext[i] != ' '; i++)
            buf[n++] = uf_toupper((char)de->ext[i]);
    }
    buf[n] = '\0';

    /* Case-insensitive compare with caller's component */
    int  len = uf_strlen(name);
    if (len != n) return 0;
    for (int i = 0; i < n; i++)
        if (uf_toupper(name[i]) != buf[i]) return 0;
    return 1;
}

/* ── Directory search ─────────────────────────────────────────────────────── */

/*
 * find_in_dir — search cluster chain of a directory for a component.
 * Returns the dirent (first_cluster and file_size set) or -1 not found.
 */
static int find_in_dir(u32 dir_cluster, const char *name,
                       u32 *out_cluster, u32 *out_size, u8 *out_attr)
{
    u32 cluster = dir_cluster;
    g_lfn_valid = 0;

    while (cluster < UF_EOC) {
        u32 lba = cluster_to_lba(cluster);
        for (u8 s = 0; s < g_bpb.sectors_per_cluster; s++) {
            if (read_sector((u64)(lba + s), g_sec_buf) != 0) return -1;
            uf_dirent_t *de = (uf_dirent_t *)g_sec_buf;
            for (int e = 0; e < 16; e++, de++) {
                if (de->name[0] == 0x00) return -1;   /* end of directory */
                if ((u8)de->name[0] == 0xE5) { g_lfn_valid = 0; continue; } /* deleted */
                if (de->attr == UF_ATTR_LFN) {
                    lfn_accumulate((uf_lfn_t *)de);
                    continue;
                }
                if (de->attr & 0x08u) { g_lfn_valid = 0; continue; } /* volume label */

                /* Match: try LFN first, then 8.3 */
                int match = 0;
                if (g_lfn_valid) {
                    int llen = uf_strlen(g_lfn_buf);
                    int nlen = uf_strlen(name);
                    if (llen == nlen) {
                        match = 1;
                        for (int i = 0; i < llen; i++)
                            if (uf_toupper(g_lfn_buf[i]) != uf_toupper(name[i]))
                                { match = 0; break; }
                    }
                }
                if (!match) match = short_name_match(de, name);
                g_lfn_valid = 0;

                if (match) {
                    u32 fc = ((u32)de->cluster_hi << 16) | de->cluster_lo;
                    *out_cluster = fc;
                    *out_size    = de->file_size;
                    *out_attr    = de->attr;
                    return 0;
                }
            }
        }
        cluster = fat_entry(cluster);
    }
    return -1;
}

/* ── Path resolver ────────────────────────────────────────────────────────── */

static int resolve_path(const char *path, u32 *out_cluster,
                        u32 *out_size, u8 *out_attr)
{
    /* Strip leading slash */
    if (*path == '/') path++;

    u32 cluster = g_bpb.root_cluster;
    u32 size    = 0;
    u8  attr    = UF_ATTR_DIR;

    if (!*path) {   /* root directory itself */
        *out_cluster = cluster;
        *out_size    = 0;
        *out_attr    = UF_ATTR_DIR;
        return 0;
    }

    char comp[256];
    while (*path) {
        /* Extract one path component */
        int len = 0;
        while (*path && *path != '/') comp[len++] = *path++;
        comp[len] = '\0';
        if (*path == '/') path++;

        if (!(attr & UF_ATTR_DIR)) return -1;   /* not a dir */

        if (find_in_dir(cluster, comp, &cluster, &size, &attr) < 0)
            return -1;
    }

    *out_cluster = cluster;
    *out_size    = size;
    *out_attr    = attr;
    return 0;
}

/* ── Public API ───────────────────────────────────────────────────────────── */

int usb_fat32_mount(void)
{
    if (!usb_msc_ready()) return -1;

    if (read_sector(0, g_sec_buf) != 0) {
        kwarn("usb_fat32: cannot read sector 0\n");
        return -1;
    }

    /* Validate BPB signature */
    if (g_sec_buf[510] != 0x55 || g_sec_buf[511] != 0xAA) {
        kwarn("usb_fat32: bad MBR signature\n");
        return -1;
    }

    /* Check if this is a FAT32 volume (offset 0) or look at partition 1 */
    u8 *b = g_sec_buf;

    /* If first byte is a jump instruction (0xEB or 0xE9) → VBR at sector 0 */
    u32 vbr_lba = 0;
    if (b[0] != 0xEBu && b[0] != 0xE9u) {
        /* It's an MBR — look at partition entry 1 (offset 0x1BE) */
        u32 part_lba  = (u32)(b[0x1BE + 8]) | ((u32)b[0x1BE + 9] << 8)
                      | ((u32)b[0x1BE + 10] << 16) | ((u32)b[0x1BE + 11] << 24);
        u8  part_type = b[0x1BE + 4];
        if (part_type == 0x0Bu || part_type == 0x0Cu || part_type == 0x0Cu) {
            vbr_lba = part_lba;
            if (read_sector(vbr_lba, g_sec_buf) != 0) {
                kwarn("usb_fat32: cannot read VBR at lba=%u\n", (unsigned)vbr_lba);
                return -1;
            }
            b = g_sec_buf;
        }
    }

    /* Parse BPB */
    u16 bps  = (u16)(b[11] | ((u16)b[12] << 8));
    u8  spc  = b[13];
    u16 rsv  = (u16)(b[14] | ((u16)b[15] << 8));
    u8  nfat = b[16];
    u32 fatsz;

    /* FAT32 fat size at offset 36 (32-bit), legacy at offset 22 if non-zero */
    u16 fat16sz = (u16)(b[22] | ((u16)b[23] << 8));
    if (fat16sz)
        fatsz = fat16sz;
    else
        fatsz = (u32)(b[36]) | ((u32)b[37] << 8) | ((u32)b[38] << 16) | ((u32)b[39] << 24);

    u32 root_clus = (u32)(b[44]) | ((u32)b[45] << 8)
                  | ((u32)b[46] << 16) | ((u32)b[47] << 24);

    /* Validate */
    if (bps != 512u || !spc || !nfat || !root_clus) {
        kwarn("usb_fat32: invalid BPB (bps=%u spc=%u nfat=%u root=%u)\n",
              (unsigned)bps, (unsigned)spc, (unsigned)nfat, (unsigned)root_clus);
        return -1;
    }

    /* Check FAT32 signature in extended BPB */
    if (b[66] != 0x29u && b[38] != 0x29u) {
        kwarn("usb_fat32: not FAT32 (no 0x29 sig)\n");
        return -1;
    }

    g_bpb.bytes_per_sector    = bps;
    g_bpb.sectors_per_cluster = spc;
    g_bpb.reserved_sectors    = rsv;
    g_bpb.num_fats            = nfat;
    g_bpb.fat_size            = fatsz;
    g_bpb.root_cluster        = root_clus;
    g_bpb.fat_lba             = vbr_lba + rsv;
    g_bpb.data_lba            = vbr_lba + rsv + (u32)nfat * fatsz;

    g_mounted = 1;
    kinfo("usb_fat32: mounted (fat_lba=%u data_lba=%u root=%u spc=%u)\n",
          (unsigned)g_bpb.fat_lba, (unsigned)g_bpb.data_lba,
          (unsigned)g_bpb.root_cluster, (unsigned)g_bpb.sectors_per_cluster);
    return 0;
}

int usb_fat32_ready(void) { return g_mounted; }

int usb_fat32_open(const char *path)
{
    if (!g_mounted) return -1;

    u32 cluster; u32 size; u8 attr;
    if (resolve_path(path, &cluster, &size, &attr) < 0) return -1;
    if (attr & UF_ATTR_DIR) return -1;   /* directories not openable as files */

    for (int i = 0; i < USB_FAT32_MAX_FILES; i++) {
        if (g_files[i].used) continue;
        g_files[i].used          = 1;
        g_files[i].first_cluster = cluster;
        g_files[i].cur_cluster   = cluster;
        g_files[i].file_size     = size;
        g_files[i].pos           = 0;
        g_files[i].cluster_pos   = 0;
        return i;
    }
    return -1;
}

int usb_fat32_read(int fh, u8 *buf, u32 len)
{
    if (fh < 0 || fh >= USB_FAT32_MAX_FILES || !g_files[fh].used) return -1;
    uf_file_t *f = &g_files[fh];

    u32 can_read = f->file_size - f->pos;
    if (len > can_read) len = can_read;
    if (!len) return 0;

    u32 bytes_read = 0;
    u32 clust_size = (u32)g_bpb.sectors_per_cluster * 512u;

    while (bytes_read < len) {
        if (f->cur_cluster >= UF_EOC) break;

        u32 offset_in_cluster = f->pos % clust_size;
        u32 sec_in_cluster    = offset_in_cluster / 512u;
        u32 offset_in_sector  = offset_in_cluster % 512u;
        u64 lba               = cluster_to_lba(f->cur_cluster) + sec_in_cluster;

        if (read_sector(lba, g_sec_buf) != 0) return -1;

        u32 avail = 512u - offset_in_sector;
        u32 want  = len - bytes_read;
        u32 chunk = avail < want ? avail : want;

        /* Don't read past end of file */
        u32 to_eof = f->file_size - f->pos;
        if (chunk > to_eof) chunk = to_eof;
        if (!chunk) break;

        for (u32 i = 0; i < chunk; i++)
            buf[bytes_read + i] = g_sec_buf[offset_in_sector + i];

        bytes_read   += chunk;
        f->pos       += chunk;
        f->cluster_pos += chunk;

        /* Advance to next cluster if we crossed the boundary */
        if (f->cluster_pos >= clust_size) {
            f->cluster_pos -= clust_size;
            f->cur_cluster  = fat_entry(f->cur_cluster);
        }
    }

    return (int)bytes_read;
}

void usb_fat32_close(int fh)
{
    if (fh >= 0 && fh < USB_FAT32_MAX_FILES)
        g_files[fh].used = 0;
}

int usb_fat32_readdir(const char *path, char *out, u32 out_len)
{
    if (!g_mounted) return -1;

    u32 cluster; u32 size; u8 attr;
    if (resolve_path(path, &cluster, &size, &attr) < 0) return -1;
    if (!(attr & UF_ATTR_DIR)) return -1;

    u32 written = 0;
    g_lfn_valid = 0;

    while (cluster < UF_EOC) {
        u32 lba = cluster_to_lba(cluster);
        for (u8 s = 0; s < g_bpb.sectors_per_cluster; s++) {
            if (read_sector((u64)(lba + s), g_sec_buf) != 0) return -1;
            uf_dirent_t *de = (uf_dirent_t *)g_sec_buf;
            for (int e = 0; e < 16; e++, de++) {
                if (de->name[0] == 0x00) goto done;
                if ((u8)de->name[0] == 0xE5) { g_lfn_valid = 0; continue; }
                if (de->attr == UF_ATTR_LFN) {
                    lfn_accumulate((uf_lfn_t *)de); continue;
                }
                if (de->attr & 0x08u) { g_lfn_valid = 0; continue; }

                const char *entry_name;
                char shortbuf[13]; int n = 0;
                if (g_lfn_valid) {
                    entry_name = g_lfn_buf;
                } else {
                    for (int i = 0; i < 8 && de->name[i] != ' '; i++)
                        shortbuf[n++] = (char)de->name[i];
                    if (de->ext[0] != ' ') {
                        shortbuf[n++] = '.';
                        for (int i = 0; i < 3 && de->ext[i] != ' '; i++)
                            shortbuf[n++] = (char)de->ext[i];
                    }
                    shortbuf[n] = '\0';
                    entry_name = shortbuf;
                }
                g_lfn_valid = 0;

                /* Append entry_name + type indicator + '\n' to output */
                int elen = uf_strlen(entry_name);
                int need = elen + 2;   /* name + type_char + '\n' */
                if (written + (u32)need >= out_len) goto done;
                for (int i = 0; i < elen; i++) out[written++] = entry_name[i];
                out[written++] = (de->attr & UF_ATTR_DIR) ? '/' : ' ';
                out[written++] = '\n';
            }
        }
        cluster = fat_entry(cluster);
    }
done:
    if (written < out_len) out[written] = '\0';
    return (int)written;
}
