/*
 * AetherOS — AetherFS native filesystem (Phase 5.2.10)
 * File: kernel/fs/aetherfs.c
 *
 * Read-only AetherFS driver.  All block I/O goes to virtio-blk device 1 (hd1).
 * Mount point /afs is registered in the VFS layer (vfs.c).
 *
 * On-disk layout:
 *   Block 0:      Superblock  (xxHash32 at byte 472 covers bytes 0..471)
 *   Block 1:      Inode bitmap
 *   Block 2..33:  Inode table  (1024 inodes × 128 bytes, 32 inodes per block)
 *   Block 34:     Data block bitmap
 *   Block 35+:    Data blocks (4096 bytes each, absolute block numbers in inodes)
 *
 * Inode layout (128 bytes):
 *   [0..3]   magic    u32  = AFS_INODE_MAGIC
 *   [4..5]   mode     u16  = AFS_MODE_FILE / AFS_MODE_DIR
 *   [6..7]   nlinks   u16
 *   [8..11]  refcount u32  (CoW reference count)
 *   [12..15] gen      u32  (generation, incremented on CoW)
 *   [16..23] size     u64  (bytes; 0 for directories)
 *   [24..31] ctime    u64
 *   [32..39] mtime    u64
 *   [40..43] checksum u32  = xxHash32(bytes 0..39)
 *   [44..107] direct[8] u64  (absolute disk block numbers)
 *   [108..115] indirect  u64  (single-indirect block pointer)
 *   [116..123] dbl_ind   u64  (double-indirect — reserved, not yet used)
 *   [124..127] _pad[4]
 *
 * Directory entry layout (64 bytes, 64 entries per block):
 *   [0..3]  inode_no u32  (0 = unused slot)
 *   [4]     type     u8   (AFS_TYPE_FILE / AFS_TYPE_DIR)
 *   [5]     name_len u8
 *   [6..7]  _pad     u8[2]
 *   [8..63] name     char[56]  (NUL-terminated, max 55 chars)
 */

#include "aether/aetherfs.h"
#include "aether/printk.h"
#include "aether/types.h"
#include "drivers/block/virtio_blk.h"

/* AetherFS lives on virtio-blk device index 1 */
#define AFS_DEV              1u

/* Derived geometry constants */
#define AFS_SECS_PER_BLK     8u          /* AFS_BLOCK_SIZE / 512 */
#define AFS_INODES_PER_BLK   32u         /* AFS_BLOCK_SIZE / 128 */
#define AFS_DIRENTS_PER_BLK  64u         /* AFS_BLOCK_SIZE / 64 */
#define AFS_PTRS_PER_BLK     512u        /* AFS_BLOCK_SIZE / 8 */

/* Superblock field byte offsets */
#define SB_OFF_TOTAL_BLOCKS  16u
#define SB_OFF_ROOT_INODE    32u
#define SB_OFF_INODE_COUNT   36u
#define SB_OFF_INODE_TBL_BLK 48u
#define SB_OFF_DATA_BMP_BLK  64u
#define SB_OFF_DATA_START    72u
#define SB_OFF_CHECKSUM      472u         /* xxHash32 of bytes 0..471 */

/* Inode field byte offsets (within a 128-byte inode record) */
#define INO_OFF_CHECKSUM     40u          /* xxHash32 of bytes 0..39 */
#define INO_OFF_DIRECT       44u          /* 8 × u64 block pointers */
#define INO_OFF_INDIRECT     108u
#define INO_OFF_DBL_IND      116u

/* ── In-memory parsed superblock ─────────────────────────────────────────── */

typedef struct {
    u64 total_blocks;
    u64 inode_table_blk;   /* typically block 2 */
    u64 data_bitmap_blk;   /* typically block 34 */
    u64 data_start_blk;    /* typically block 35 */
    u32 root_inode;        /* typically 0 */
    u32 inode_count;       /* typically 1024 */
} afs_sb_t;

static afs_sb_t g_sb;
static int      g_mounted;

/* ── In-memory inode (parsed) ────────────────────────────────────────────── */

typedef struct {
    u32 magic, refcount, gen, checksum;
    u16 mode, nlinks;
    u64 size, ctime, mtime;
    u64 direct[8];
    u64 indirect;
    u64 dbl_indirect;
} afs_inode_t;

/* ── Open file handles ───────────────────────────────────────────────────── */

typedef struct {
    int  used;
    u32  inode_no;
    u64  size;
    u32  pos;
    u64  direct[8];
    u64  indirect;
    u64  dbl_indirect;
} afs_file_t;

static afs_file_t g_files[AFS_MAX_FILES];

/* ── I/O scratch buffers (single-threaded kernel, no re-entrancy needed) ── */

static u8 g_blk0[AFS_BLOCK_SIZE];   /* inode table blocks + file data reads */
static u8 g_blk1[AFS_BLOCK_SIZE];   /* directory blocks + indirect block reads */

/* ── xxHash32 (inline, no external dependencies) ────────────────────────── */

static inline u32 rotl32(u32 x, int r) { return (x << r) | (x >> (32 - r)); }

static u32 xxhash32(const u8 *data, u32 len, u32 seed)
{
    static const u32 P1 = 2654435761u;
    static const u32 P2 = 2246822519u;
    static const u32 P3 = 3266489917u;
    static const u32 P4 =  668265263u;
    static const u32 P5 =  374761393u;

    const u8 *p   = data;
    const u8 *end = data + len;
    u32 h32;

#define RD32(pp) ((u32)(pp)[0] | ((u32)(pp)[1]<<8) | ((u32)(pp)[2]<<16) | ((u32)(pp)[3]<<24))

    if (len >= 16u) {
        u32 v1 = seed + P1 + P2;
        u32 v2 = seed + P2;
        u32 v3 = seed;
        u32 v4 = seed - P1;
        do {
            v1 = rotl32(v1 + RD32(p) * P2, 13) * P1; p += 4;
            v2 = rotl32(v2 + RD32(p) * P2, 13) * P1; p += 4;
            v3 = rotl32(v3 + RD32(p) * P2, 13) * P1; p += 4;
            v4 = rotl32(v4 + RD32(p) * P2, 13) * P1; p += 4;
        } while (p <= end - 16);
        h32 = rotl32(v1, 1) + rotl32(v2, 7) + rotl32(v3, 12) + rotl32(v4, 18);
    } else {
        h32 = seed + P5;
    }

    h32 += len;
    while (p <= end - 4) { h32 = rotl32(h32 + RD32(p) * P3, 17) * P4; p += 4; }
    while (p < end)       { h32 = rotl32(h32 + (u32)*p * P5, 11) * P1; p++; }

#undef RD32

    h32 ^= h32 >> 15; h32 *= P2;
    h32 ^= h32 >> 13; h32 *= P3;
    h32 ^= h32 >> 16;
    return h32;
}

/* ── Byte helpers ────────────────────────────────────────────────────────── */

static inline u32 rd32(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1]<<8) | ((u32)p[2]<<16) | ((u32)p[3]<<24);
}

static inline u64 rd64(const u8 *p)
{
    return (u64)p[0] | ((u64)p[1]<<8) | ((u64)p[2]<<16) | ((u64)p[3]<<24) |
           ((u64)p[4]<<32) | ((u64)p[5]<<40) | ((u64)p[6]<<48) | ((u64)p[7]<<56);
}

/* ── Block I/O ───────────────────────────────────────────────────────────── */

static int read_block(u64 blk, u8 *buf)
{
    return virtio_blk_read_sectors_n(AFS_DEV, blk * AFS_SECS_PER_BLK,
                                     AFS_SECS_PER_BLK, buf);
}

/* ── Inode I/O ───────────────────────────────────────────────────────────── */

static int read_inode(u32 ino, afs_inode_t *out)
{
    u32 blk_in_table = ino / AFS_INODES_PER_BLK;
    u32 idx          = ino % AFS_INODES_PER_BLK;
    u64 blk          = g_sb.inode_table_blk + blk_in_table;

    if (read_block(blk, g_blk0) != 0) return -1;

    const u8 *p = g_blk0 + idx * 128u;

    /* Verify inode checksum (covers bytes 0..39) */
    u32 crc_stored = rd32(p + INO_OFF_CHECKSUM);
    u32 crc_calc   = xxhash32(p, INO_OFF_CHECKSUM, 0u);
    if (crc_stored != crc_calc) {
        kerror("aetherfs: inode %u checksum mismatch (stored 0x%x calc 0x%x)\n",
               (unsigned)ino, (unsigned)crc_stored, (unsigned)crc_calc);
        return -1;
    }

    out->magic    = rd32(p + 0u);
    if (out->magic != AFS_INODE_MAGIC) {
        kerror("aetherfs: inode %u bad magic 0x%x\n", (unsigned)ino, (unsigned)out->magic);
        return -1;
    }
    out->mode     = (u16)p[4] | ((u16)p[5] << 8);
    out->nlinks   = (u16)p[6] | ((u16)p[7] << 8);
    out->refcount = rd32(p + 8u);
    out->gen      = rd32(p + 12u);
    out->size     = rd64(p + 16u);
    out->ctime    = rd64(p + 24u);
    out->mtime    = rd64(p + 32u);
    out->checksum = crc_stored;

    for (int i = 0; i < 8; i++)
        out->direct[i] = rd64(p + INO_OFF_DIRECT + (u32)i * 8u);
    out->indirect     = rd64(p + INO_OFF_INDIRECT);
    out->dbl_indirect = rd64(p + INO_OFF_DBL_IND);
    return 0;
}

/* ── String helpers ──────────────────────────────────────────────────────── */

static int afs_strlen(const char *s) { int n = 0; while (s[n]) n++; return n; }

static int afs_streq(const char *a, const char *b, int len)
{
    for (int i = 0; i < len; i++)
        if (a[i] != b[i]) return 0;
    return 1;
}

/* ── Directory search ────────────────────────────────────────────────────── */

static int find_in_dir(const afs_inode_t *dir, const char *name,
                       u32 *out_ino, u8 *out_type)
{
    int namelen = afs_strlen(name);

    for (int di = 0; di < 8; di++) {
        u64 blk = dir->direct[di];
        if (blk == 0u) continue;
        if (read_block(blk, g_blk1) != 0) continue;

        for (u32 ei = 0; ei < AFS_DIRENTS_PER_BLK; ei++) {
            const u8 *e = g_blk1 + ei * 64u;
            u32 ino_no  = rd32(e + 0u);
            if (ino_no == 0u) continue;             /* unused slot */

            u8  type     = e[4];
            u8  name_len = e[5];
            if (name_len != (u8)namelen) continue;
            if (!afs_streq((const char *)(e + 8), name, namelen)) continue;

            *out_ino  = ino_no;
            if (out_type) *out_type = type;
            return 0;
        }
    }
    return -1;
}

/* ── Path resolution ─────────────────────────────────────────────────────── */

static int walk_path(const char *path, u32 *out_ino, u8 *out_type)
{
    while (*path == '/') path++;   /* strip leading slashes */

    u32 cur_ino  = g_sb.root_inode;
    u8  cur_type = AFS_TYPE_DIR;

    while (*path) {
        /* Extract next path component */
        char comp[64];
        int n = 0;
        while (*path && *path != '/' && n < 63)
            comp[n++] = *path++;
        comp[n] = '\0';
        while (*path == '/') path++;

        /* Load current directory inode into g_blk0 */
        afs_inode_t inode;
        if (read_inode(cur_ino, &inode) != 0) return -1;
        if (inode.mode != AFS_MODE_DIR) return -1;

        /* Search for component — reads into g_blk1 */
        u32 next_ino; u8 next_type;
        if (find_in_dir(&inode, comp, &next_ino, &next_type) != 0) return -1;

        cur_ino  = next_ino;
        cur_type = next_type;
    }

    *out_ino = cur_ino;
    if (out_type) *out_type = cur_type;
    return 0;
}

/* ── Public: mount ───────────────────────────────────────────────────────── */

int aetherfs_mount(void)
{
    if (!virtio_blk_ready_n(AFS_DEV)) {
        kinfo("aetherfs: device %u not ready (no second disk)\n", (unsigned)AFS_DEV);
        return -1;
    }

    if (read_block(0u, g_blk0) != 0) {
        kerror("aetherfs: cannot read superblock\n");
        return -1;
    }

    /* Verify magic */
    u64 magic = rd64(g_blk0 + 0u);
    if (magic != AFS_MAGIC) {
        kerror("aetherfs: bad magic 0x%lx\n", (unsigned long)magic);
        return -1;
    }

    /* Verify superblock checksum */
    u32 crc_stored = rd32(g_blk0 + SB_OFF_CHECKSUM);
    u32 crc_calc   = xxhash32(g_blk0, SB_OFF_CHECKSUM, 0u);
    if (crc_stored != crc_calc) {
        kerror("aetherfs: superblock checksum mismatch (stored 0x%x calc 0x%x)\n",
               (unsigned)crc_stored, (unsigned)crc_calc);
        return -1;
    }

    /* Parse key fields */
    g_sb.total_blocks    = rd64(g_blk0 + SB_OFF_TOTAL_BLOCKS);
    g_sb.root_inode      = rd32(g_blk0 + SB_OFF_ROOT_INODE);
    g_sb.inode_count     = rd32(g_blk0 + SB_OFF_INODE_COUNT);
    g_sb.inode_table_blk = rd64(g_blk0 + SB_OFF_INODE_TBL_BLK);
    g_sb.data_bitmap_blk = rd64(g_blk0 + SB_OFF_DATA_BMP_BLK);
    g_sb.data_start_blk  = rd64(g_blk0 + SB_OFF_DATA_START);

    kinfo("aetherfs: mounted — root=%u total_blocks=%lu data_start=%lu\n",
          (unsigned)g_sb.root_inode,
          (unsigned long)g_sb.total_blocks,
          (unsigned long)g_sb.data_start_blk);

    g_mounted = 1;
    return 0;
}

int aetherfs_ready(void) { return g_mounted; }

/* ── Public: open ────────────────────────────────────────────────────────── */

int aetherfs_open(const char *path)
{
    if (!g_mounted || !path) return -1;

    u32 ino; u8 type;
    if (walk_path(path, &ino, &type) != 0) return -1;
    if (type == AFS_TYPE_DIR) return -1;   /* cannot open a directory as a file */

    afs_inode_t inode;
    if (read_inode(ino, &inode) != 0) return -1;
    if (inode.mode != AFS_MODE_FILE) return -1;

    for (int i = 0; i < (int)AFS_MAX_FILES; i++) {
        if (!g_files[i].used) {
            g_files[i].used     = 1;
            g_files[i].inode_no = ino;
            g_files[i].size     = inode.size;
            g_files[i].pos      = 0u;
            for (int j = 0; j < 8; j++) g_files[i].direct[j] = inode.direct[j];
            g_files[i].indirect     = inode.indirect;
            g_files[i].dbl_indirect = inode.dbl_indirect;
            return i;
        }
    }
    return -1;   /* too many open files */
}

/* ── Public: read ────────────────────────────────────────────────────────── */

int aetherfs_read(int fh, u8 *buf, u32 len)
{
    if (fh < 0 || fh >= (int)AFS_MAX_FILES || !g_files[fh].used) return -1;
    if (!buf || len == 0u) return 0;

    afs_file_t *f = &g_files[fh];
    if ((u64)f->pos >= f->size) return 0;

    u32 remaining = (u32)(f->size - (u64)f->pos);
    if (len > remaining) len = remaining;

    u32 bytes_read = 0u;

    while (bytes_read < len) {
        u32 block_idx    = f->pos / AFS_BLOCK_SIZE;
        u32 off_in_block = f->pos % AFS_BLOCK_SIZE;

        u64 blk_no = 0u;
        if (block_idx < 8u) {
            blk_no = f->direct[block_idx];
        } else if (block_idx < 8u + AFS_PTRS_PER_BLK) {
            if (f->indirect == 0u) break;
            if (read_block(f->indirect, g_blk1) != 0) break;
            u32 idx = block_idx - 8u;
            blk_no = rd64(g_blk1 + idx * 8u);
        } else {
            break;   /* double-indirect not yet implemented */
        }
        if (blk_no == 0u) break;

        if (read_block(blk_no, g_blk0) != 0) break;

        u32 avail = AFS_BLOCK_SIZE - off_in_block;
        u32 want  = len - bytes_read;
        if (want > avail) want = avail;

        for (u32 i = 0u; i < want; i++)
            buf[bytes_read + i] = g_blk0[off_in_block + i];

        bytes_read += want;
        f->pos     += want;
    }

    return (int)bytes_read;
}

/* ── Public: close ───────────────────────────────────────────────────────── */

void aetherfs_close(int fh)
{
    if (fh >= 0 && fh < (int)AFS_MAX_FILES)
        g_files[fh].used = 0;
}

/* ── Public: readdir ─────────────────────────────────────────────────────── */

static void rd_str(char *buf, u32 *out, u32 len, const char *s)
{
    while (*s && *out + 1u < len) buf[(*out)++] = *s++;
}

static void rd_u64(char *buf, u32 *out, u32 len, u64 v)
{
    char tmp[20]; int n = 0;
    if (v == 0u) { tmp[n++] = '0'; }
    else { while (v) { tmp[n++] = (char)('0' + v % 10u); v /= 10u; } }
    for (int k = n - 1; k >= 0 && *out + 1u < len; k--)
        buf[(*out)++] = tmp[k];
}

int aetherfs_readdir(const char *path, char *buf, u32 len)
{
    if (!g_mounted || !buf || len == 0u) return -1;

    u32 ino; u8 type;
    if (walk_path(path, &ino, &type) != 0) return -1;

    afs_inode_t inode;
    if (read_inode(ino, &inode) != 0) return -1;
    if (inode.mode != AFS_MODE_DIR) return -1;

    u32 out = 0u;

    for (int di = 0; di < 8; di++) {
        u64 blk = inode.direct[di];
        if (blk == 0u) continue;
        if (read_block(blk, g_blk1) != 0) continue;

        for (u32 ei = 0u; ei < AFS_DIRENTS_PER_BLK; ei++) {
            const u8 *e = g_blk1 + ei * 64u;
            u32 ino_no  = rd32(e + 0u);
            if (ino_no == 0u) continue;

            u8  etype    = e[4];
            u8  name_len = e[5];
            const char *name = (const char *)(e + 8u);

            /* Skip . and .. */
            if (name[0] == '.' &&
                (name[1] == '\0' || (name[1] == '.' && name[2] == '\0')))
                continue;

            if (etype == AFS_TYPE_DIR) {
                rd_str(buf, &out, len, "[");
                for (u8 k = 0; k < name_len && out + 1u < len; k++)
                    buf[out++] = name[k];
                rd_str(buf, &out, len, "]");
            } else {
                /* Name padded to column 20, then size in bytes */
                for (u8 k = 0; k < name_len && out + 1u < len; k++)
                    buf[out++] = name[k];
                for (int k = name_len; k < 20 && out + 1u < len; k++)
                    buf[out++] = ' ';
                /* Read file inode to get size — uses g_blk0 */
                afs_inode_t fi;
                if (read_inode(ino_no, &fi) == 0) {
                    rd_u64(buf, &out, len, fi.size);
                    rd_str(buf, &out, len, " bytes");
                }
            }
            if (out + 1u < len) buf[out++] = '\n';
        }
    }

    if (out < len) buf[out] = '\0';
    return (int)out;
}
