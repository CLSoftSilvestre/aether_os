#!/bin/bash
# scripts/make_afs.sh — Create an AetherFS disk image (build/afs.img)
#
# Requires: python3 (standard library only)
# Output: build/afs.img (32 MB, AetherFS v1)
#
# On-disk layout (4096-byte blocks):
#   Block 0:       Superblock
#   Block 1:       Inode bitmap  (1 bit per inode, LSB-first)
#   Block 2..33:   Inode table   (1024 inodes × 128 bytes = 32 blocks)
#   Block 34:      Data bitmap
#   Block 35+:     Data blocks
#
# Test filesystem tree:
#   /readme.txt       — short welcome message
#   /version.txt      — version string
#   /docs/            — subdirectory
#     about.txt       — about AetherFS
#     format.txt      — on-disk format description

set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/../build"
OUT="${BUILD_DIR}/afs.img"

mkdir -p "${BUILD_DIR}"

python3 - "${OUT}" << 'PYEOF'
import sys, struct

OUT = sys.argv[1]

# ── Constants ────────────────────────────────────────────────────────────────

BLOCK_SIZE     = 4096
DISK_BLOCKS    = 8192          # 32 MB
IMAGE_SIZE     = DISK_BLOCKS * BLOCK_SIZE

AFS_MAGIC      = 0x4145544845524653  # "AETHERFS" little-endian
AFS_VERSION    = 1
AFS_INODE_MAGIC = 0xAEF51E00

AFS_MODE_FILE  = 1
AFS_MODE_DIR   = 2
AFS_TYPE_FILE  = 1
AFS_TYPE_DIR   = 2

INODE_TABLE_BLK  = 2
INODE_TBL_NBLKS  = 32
DATA_BITMAP_BLK  = 34
DATA_START_BLK   = 35
INODES_PER_BLK   = 32          # 4096 / 128
DIRENTS_PER_BLK  = 64          # 4096 / 64
MAX_INODES       = INODE_TBL_NBLKS * INODES_PER_BLK  # 1024

SB_CHECKSUM_OFF  = 472          # covers bytes 0..471

# ── xxHash32 ─────────────────────────────────────────────────────────────────

def xxhash32(data, seed=0):
    P1, P2, P3, P4, P5 = 2654435761, 2246822519, 3266489917, 668265263, 374761393
    def u32(x): return x & 0xFFFFFFFF
    def rotl32(x, r): return u32((x << r) | (x >> (32 - r)))

    data = bytes(data)
    length = len(data)
    p = 0

    if length >= 16:
        v1, v2, v3, v4 = u32(seed+P1+P2), u32(seed+P2), u32(seed), u32(seed-P1)
        while p + 16 <= length:
            w = struct.unpack_from('<I', data, p)[0]; p += 4
            v1 = u32(rotl32(u32(v1 + u32(w * P2)), 13) * P1)
            w = struct.unpack_from('<I', data, p)[0]; p += 4
            v2 = u32(rotl32(u32(v2 + u32(w * P2)), 13) * P1)
            w = struct.unpack_from('<I', data, p)[0]; p += 4
            v3 = u32(rotl32(u32(v3 + u32(w * P2)), 13) * P1)
            w = struct.unpack_from('<I', data, p)[0]; p += 4
            v4 = u32(rotl32(u32(v4 + u32(w * P2)), 13) * P1)
        h32 = u32(rotl32(v1,1) + rotl32(v2,7) + rotl32(v3,12) + rotl32(v4,18))
    else:
        h32 = u32(seed + P5)

    h32 = u32(h32 + length)
    while p + 4 <= length:
        w = struct.unpack_from('<I', data, p)[0]; p += 4
        h32 = u32(rotl32(u32(h32 + u32(w * P3)), 17) * P4)
    while p < length:
        h32 = u32(rotl32(u32(h32 + data[p] * P5), 11) * P1); p += 1

    h32 = u32(h32 ^ (h32 >> 15)); h32 = u32(h32 * P2)
    h32 = u32(h32 ^ (h32 >> 13)); h32 = u32(h32 * P3)
    return u32(h32 ^ (h32 >> 16))

# ── Image buffer ──────────────────────────────────────────────────────────────

img = bytearray(IMAGE_SIZE)

def blk_off(n): return n * BLOCK_SIZE

def w8(off, v):   img[off] = v & 0xFF
def w16(off, v):  struct.pack_into('<H', img, off, v & 0xFFFF)
def w32(off, v):  struct.pack_into('<I', img, off, v & 0xFFFFFFFF)
def w64(off, v):  struct.pack_into('<Q', img, off, v & 0xFFFFFFFFFFFFFFFF)
def wstr(off, s, maxlen):
    b = s.encode('utf-8')[:maxlen]
    img[off:off+len(b)] = b

# ── File content ──────────────────────────────────────────────────────────────

README_TXT = (
    "Welcome to AetherFS!\n"
    "This is the native filesystem of AetherOS.\n"
    "Mounted read-only at /afs — write support coming in Phase 5.5.\n"
).encode()

VERSION_TXT = (
    "AetherOS  v0.9.0\n"
    "AetherFS  v1.0 (read-only)\n"
    "Build     Phase 5.2.10\n"
).encode()

ABOUT_TXT = (
    "AetherFS — The AetherOS Native Filesystem\n"
    "==========================================\n"
    "Copy-on-Write B-tree structure (Phase 5.2)\n"
    "xxHash32 block checksums\n"
    "Snapshot table (up to 8 snapshots)\n"
    "Write support deferred to Phase 5.5\n"
).encode()

FORMAT_TXT = (
    "AetherFS On-Disk Format (v1)\n"
    "============================\n"
    "Block size:  4096 bytes (8 sectors)\n"
    "Block 0:     Superblock + checksum at byte 472\n"
    "Block 1:     Inode bitmap (1 bit per inode, LSB-first)\n"
    "Block 2..33: Inode table (1024 inodes x 128 bytes)\n"
    "Block 34:    Data block bitmap\n"
    "Block 35+:   Data blocks (absolute block numbers in inodes)\n"
    "\n"
    "Inode layout (128 bytes):\n"
    "  [0]  magic    0xAEF51E00\n"
    "  [4]  mode     1=file 2=dir\n"
    "  [16] size     u64\n"
    "  [40] checksum xxHash32(bytes 0..39)\n"
    "  [44] direct[8] u64 block pointers\n"
).encode()

# ── Layout plan ───────────────────────────────────────────────────────────────
#
#  Inode 0: root dir        direct[0] = block 35
#  Inode 1: readme.txt      direct[0] = block 37
#  Inode 2: version.txt     direct[0] = block 38
#  Inode 3: docs/           direct[0] = block 36
#  Inode 4: about.txt       direct[0] = block 39
#  Inode 5: format.txt      direct[0] = block 40
#
#  Block 35: root directory entries
#  Block 36: docs/ directory entries
#  Block 37: readme.txt content
#  Block 38: version.txt content
#  Block 39: about.txt content
#  Block 40: format.txt content

NUM_INODES_USED = 6
NUM_DATA_USED   = 6   # data blocks 0..5 (disk blocks 35..40)
FREE_BLOCKS     = DISK_BLOCKS - DATA_START_BLK - NUM_DATA_USED  # blocks after used data

# ── Helper: make inode bytes (128 bytes) ─────────────────────────────────────

def make_inode(mode, size, direct0, nlinks=1):
    ino = bytearray(128)
    struct.pack_into('<I', ino,  0, AFS_INODE_MAGIC)   # magic
    struct.pack_into('<H', ino,  4, mode)               # mode
    struct.pack_into('<H', ino,  6, nlinks)             # nlinks
    struct.pack_into('<I', ino,  8, 1)                  # refcount
    struct.pack_into('<I', ino, 12, 1)                  # gen
    struct.pack_into('<Q', ino, 16, size)               # size
    struct.pack_into('<Q', ino, 24, 0)                  # ctime
    struct.pack_into('<Q', ino, 32, 0)                  # mtime
    # checksum of bytes 0..39, stored at [40]
    crc = xxhash32(bytes(ino[0:40]))
    struct.pack_into('<I', ino, 40, crc)
    # direct[0] at offset 44
    struct.pack_into('<Q', ino, 44, direct0)
    return bytes(ino)

# ── Helper: make directory entry (64 bytes) ───────────────────────────────────

def make_dirent(inode_no, entry_type, name):
    e = bytearray(64)
    struct.pack_into('<I', e, 0, inode_no)
    e[4] = entry_type
    name_b = name.encode('utf-8')[:55]
    e[5] = len(name_b)
    e[8:8+len(name_b)] = name_b
    return bytes(e)

# ── Block 0: Superblock ───────────────────────────────────────────────────────

sb_off = blk_off(0)
w64(sb_off +  0, AFS_MAGIC)
w32(sb_off +  8, AFS_VERSION)
w32(sb_off + 12, BLOCK_SIZE)
w64(sb_off + 16, DISK_BLOCKS)            # total_blocks
w64(sb_off + 24, FREE_BLOCKS)            # free_blocks
w32(sb_off + 32, 0)                      # root_inode = 0
w32(sb_off + 36, MAX_INODES)             # inode_count
w64(sb_off + 40, 1)                      # inode_bitmap_blk = 1
w64(sb_off + 48, INODE_TABLE_BLK)        # inode_table_blk = 2
w32(sb_off + 56, INODE_TBL_NBLKS)        # inode_tbl_nblks = 32
w32(sb_off + 60, 0)                      # _pad0
w64(sb_off + 64, DATA_BITMAP_BLK)        # data_bitmap_blk = 34
w64(sb_off + 72, DATA_START_BLK)         # data_start_blk = 35
w32(sb_off + 80, 0)                      # snap_count = 0
w32(sb_off + 84, 0)                      # _pad1
# snapshots[384] at bytes 88..471 — all zeros

crc = xxhash32(bytes(img[sb_off:sb_off + SB_CHECKSUM_OFF]))
w32(sb_off + SB_CHECKSUM_OFF, crc)

# ── Block 1: Inode bitmap (6 inodes used → first byte = 0x3F) ────────────────

bmp_off = blk_off(1)
img[bmp_off] = (1 << NUM_INODES_USED) - 1   # 0x3F

# ── Block 2: Inode table block 0 (holds inodes 0..31) ────────────────────────

ino_blk_off = blk_off(INODE_TABLE_BLK)

# Inode 0: root directory  (nlinks=2 for . and ..)
img[ino_blk_off + 0*128 : ino_blk_off + 1*128] = make_inode(
    AFS_MODE_DIR, 0, direct0=35, nlinks=2)

# Inode 1: readme.txt
img[ino_blk_off + 1*128 : ino_blk_off + 2*128] = make_inode(
    AFS_MODE_FILE, len(README_TXT), direct0=37)

# Inode 2: version.txt
img[ino_blk_off + 2*128 : ino_blk_off + 3*128] = make_inode(
    AFS_MODE_FILE, len(VERSION_TXT), direct0=38)

# Inode 3: docs/  (nlinks=2 for . and ..)
img[ino_blk_off + 3*128 : ino_blk_off + 4*128] = make_inode(
    AFS_MODE_DIR, 0, direct0=36, nlinks=2)

# Inode 4: about.txt
img[ino_blk_off + 4*128 : ino_blk_off + 5*128] = make_inode(
    AFS_MODE_FILE, len(ABOUT_TXT), direct0=39)

# Inode 5: format.txt
img[ino_blk_off + 5*128 : ino_blk_off + 6*128] = make_inode(
    AFS_MODE_FILE, len(FORMAT_TXT), direct0=40)

# ── Block 34: Data block bitmap (6 data blocks used) ─────────────────────────

img[blk_off(DATA_BITMAP_BLK)] = (1 << NUM_DATA_USED) - 1   # 0x3F

# ── Block 35: Root directory entries ─────────────────────────────────────────

root_off = blk_off(35)
dirents = [
    make_dirent(0, AFS_TYPE_DIR,  "."),
    make_dirent(0, AFS_TYPE_DIR,  ".."),
    make_dirent(1, AFS_TYPE_FILE, "readme.txt"),
    make_dirent(2, AFS_TYPE_FILE, "version.txt"),
    make_dirent(3, AFS_TYPE_DIR,  "docs"),
]
for i, de in enumerate(dirents):
    img[root_off + i*64 : root_off + i*64 + 64] = de

# ── Block 36: docs/ directory entries ────────────────────────────────────────

docs_off = blk_off(36)
doc_dirents = [
    make_dirent(3, AFS_TYPE_DIR,  "."),
    make_dirent(0, AFS_TYPE_DIR,  ".."),
    make_dirent(4, AFS_TYPE_FILE, "about.txt"),
    make_dirent(5, AFS_TYPE_FILE, "format.txt"),
]
for i, de in enumerate(doc_dirents):
    img[docs_off + i*64 : docs_off + i*64 + 64] = de

# ── Blocks 37..40: File data ──────────────────────────────────────────────────

for blk_no, content in [(37, README_TXT), (38, VERSION_TXT),
                         (39, ABOUT_TXT),  (40, FORMAT_TXT)]:
    off = blk_off(blk_no)
    img[off:off+len(content)] = content

# ── Write image ───────────────────────────────────────────────────────────────

with open(OUT, 'wb') as f:
    f.write(img)

size_mb = IMAGE_SIZE // (1024 * 1024)
print(f"[make_afs] Created {OUT} ({size_mb} MB, AetherFS v1)")
print(f"[make_afs] Layout: superblock=0 inode_table=2 data_start=35")
print(f"[make_afs] Inodes used: {NUM_INODES_USED}/1024")
print(f"[make_afs] Data blocks used: {NUM_DATA_USED} (blocks 35..{DATA_START_BLK+NUM_DATA_USED-1})")
print(f"[make_afs] Superblock checksum: 0x{crc:08x}")
print(f"[make_afs] Files: /readme.txt /version.txt /docs/about.txt /docs/format.txt")

PYEOF

echo "[make_afs] Done: ${OUT}"
