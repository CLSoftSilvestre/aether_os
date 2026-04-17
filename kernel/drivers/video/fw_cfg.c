/*
 * QEMU fw_cfg MMIO driver — AetherOS
 * File: kernel/drivers/video/fw_cfg.c
 *
 * QEMU fw_cfg lets the guest read/write named configuration blobs.
 * We use it solely to configure the ramfb device ("etc/ramfb").
 *
 * Register map (QEMU -M virt, base 0x09020000):
 *   +0x00  data    — 1-byte streaming data port
 *   +0x08  selector — 2-byte selector (big-endian write triggers seek)
 *   +0x10  dma_hi  — high 32 bits of DMA descriptor address (big-endian)
 *   +0x14  dma_lo  — low  32 bits of DMA descriptor address (big-endian, triggers DMA)
 *
 * DMA control flags (in the descriptor's control field, big-endian u32):
 *   bit 0: ERROR  — set by QEMU on failure
 *   bit 1: READ   — read from fw_cfg file into guest buffer
 *   bit 2: SKIP   — skip bytes (advance file position)
 *   bit 3: SELECT — set selector from bits [31:16] of control
 *   bit 4: WRITE  — write from guest buffer into fw_cfg file
 */

#include "drivers/video/fw_cfg.h"
#include "aether/printk.h"
#include "aether/types.h"

/* ── MMIO register addresses ─────────────────────────────────────────── */

#define FWCFG_BASE     0x09020000UL
#define FWCFG_DATA     (FWCFG_BASE + 0x00)
#define FWCFG_SEL      (FWCFG_BASE + 0x08)
#define FWCFG_DMA_HI   (FWCFG_BASE + 0x10)
#define FWCFG_DMA_LO   (FWCFG_BASE + 0x14)

/* ── Known selector keys ─────────────────────────────────────────────── */

#define FWCFG_KEY_SIGNATURE  0x0000   /* 4 bytes: "QEMU" */
#define FWCFG_KEY_ID         0x0001   /* 4 bytes: feature flags */
#define FWCFG_KEY_FILE_DIR   0x0019   /* file directory */

/* ── DMA control flags ───────────────────────────────────────────────── */

#define DMA_CTL_ERROR   (1u << 0)
#define DMA_CTL_READ    (1u << 1)
#define DMA_CTL_SKIP    (1u << 2)
#define DMA_CTL_SELECT  (1u << 3)
#define DMA_CTL_WRITE   (1u << 4)

/* ── DMA descriptor (must be physically addressable, big-endian fields) ─ */

struct fw_cfg_dma {
    volatile u32 control;   /* big-endian */
    u32          length;    /* big-endian */
    u64          address;   /* big-endian */
} __attribute__((packed, aligned(8)));

/* ── Byte-swap helpers ───────────────────────────────────────────────── */

static inline u16 bswap16(u16 v) { return (u16)((v >> 8) | (v << 8)); }
static inline u32 bswap32(u32 v)
{
    return ((v & 0x000000FFu) << 24)
         | ((v & 0x0000FF00u) <<  8)
         | ((v & 0x00FF0000u) >>  8)
         | ((v & 0xFF000000u) >> 24);
}
static inline u64 bswap64(u64 v)
{
    return ((u64)bswap32((u32)(v & 0xFFFFFFFFu)) << 32)
         |  (u64)bswap32((u32)(v >> 32));
}

/* ── Low-level MMIO helpers ──────────────────────────────────────────── */

static inline void sel_write(u16 key)
{
    /* selector is big-endian 16-bit */
    MMIO_WRITE16(FWCFG_SEL, bswap16(key));
    __asm__ volatile("dsb sy" ::: "memory");
}

static inline u8 data_read8(void)
{
    return MMIO_READ8(FWCFG_DATA);
}

/* Read n bytes from the currently-selected fw_cfg file into buf */
static void fwcfg_read_bytes(void *buf, u32 n)
{
    u8 *p = buf;
    for (u32 i = 0; i < n; i++)
        p[i] = data_read8();
}

/* ── DMA write ───────────────────────────────────────────────────────── */

/* Static descriptor — lives in BSS so it has a stable physical address */
static struct fw_cfg_dma dma_desc;

static void dma_execute(u32 control_be)
{
    dma_desc.control = control_be;          /* already big-endian */
    __asm__ volatile("dsb sy" ::: "memory");

    uintptr_t pa = (uintptr_t)&dma_desc;
    MMIO_WRITE32(FWCFG_DMA_HI, bswap32((u32)(pa >> 32)));
    __asm__ volatile("dsb sy" ::: "memory");
    MMIO_WRITE32(FWCFG_DMA_LO, bswap32((u32)(pa & 0xFFFFFFFFu)));
    __asm__ volatile("dsb sy" ::: "memory");

    /* Poll until QEMU clears the ERROR bit or zeroes control */
    u32 limit = 100000;
    while (limit--) {
        __asm__ volatile("dsb sy" ::: "memory");
        if (!(dma_desc.control & bswap32(DMA_CTL_ERROR))) break;
    }
}

/* ── fw_cfg file directory entry ─────────────────────────────────────── */

struct fw_cfg_file {
    u32  size;        /* big-endian */
    u16  select;      /* big-endian */
    u16  reserved;
    char name[56];
} __attribute__((packed));

/* ── Public API ──────────────────────────────────────────────────────── */

static int fwcfg_str_eq(const char *a, const char *b)
{
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == *b;
}

u16 fwcfg_find_file(const char *name, u32 *size_out)
{
    sel_write(FWCFG_KEY_FILE_DIR);

    /* First 4 bytes: number of directory entries (big-endian) */
    u32 count_be;
    fwcfg_read_bytes(&count_be, 4);
    u32 count = bswap32(count_be);

    struct fw_cfg_file entry;
    for (u32 i = 0; i < count; i++) {
        fwcfg_read_bytes(&entry, sizeof(entry));
        if (fwcfg_str_eq(entry.name, name)) {
            if (size_out) *size_out = bswap32(entry.size);
            return bswap16(entry.select);
        }
    }
    return 0;   /* not found */
}

void fwcfg_write_file(u16 selector, const void *data, u32 len)
{
    dma_desc.length  = bswap32(len);
    dma_desc.address = bswap64((u64)(uintptr_t)data);

    /* Control: SELECT (key in bits 31:16) | WRITE */
    u32 ctl = ((u32)selector << 16) | DMA_CTL_SELECT | DMA_CTL_WRITE;
    dma_execute(bswap32(ctl));
}
