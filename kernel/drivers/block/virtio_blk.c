/*
 * AetherOS — VirtIO block PCI driver (modern VirtIO 1.x, Phase 5.2)
 * File: kernel/drivers/block/virtio_blk.c
 *
 * QEMU: -drive file=disk.img,format=raw,if=none,id=hd0 -device virtio-blk-pci,drive=hd0
 * PCI vendor 0x1AF4, device 0x1042 (VirtIO ID 2 = block).
 *
 * One queue: request queue (index 0).
 * Each request is a 3-descriptor chain:
 *   [0] header  (16 bytes, device reads)
 *   [1] data    (512*n bytes, device reads for write / device writes for read)
 *   [2] status  (1 byte, device writes: 0=ok, 1=ioerr, 2=unsupported)
 *
 * All I/O is synchronous (busy-wait on used ring) — appropriate for a
 * sequential filesystem driver with no concurrency requirements.
 */

#include "drivers/block/virtio_blk.h"
#include "drivers/pci/pci_ecam.h"
#include "aether/printk.h"
#include "aether/types.h"

/* ── VirtIO PCI common-config offsets (same as virtio_net.c) ────────────── */
#define VCC_DEV_FEAT_SEL   0x00u
#define VCC_DEV_FEAT       0x04u
#define VCC_DRV_FEAT_SEL   0x08u
#define VCC_DRV_FEAT       0x0Cu
#define VCC_NUM_QUEUES     0x12u
#define VCC_DEV_STATUS     0x14u
#define VCC_QUEUE_SEL      0x16u
#define VCC_QUEUE_SIZE     0x18u
#define VCC_QUEUE_ENABLE   0x1Cu
#define VCC_QUEUE_NTFY_OFF 0x1Eu
#define VCC_QUEUE_DESC_LO  0x20u
#define VCC_QUEUE_DESC_HI  0x24u
#define VCC_QUEUE_DRV_LO   0x28u
#define VCC_QUEUE_DRV_HI   0x2Cu
#define VCC_QUEUE_DEV_LO   0x30u
#define VCC_QUEUE_DEV_HI   0x34u

#define VPCI_CAP_COMMON_CFG  1u
#define VPCI_CAP_NOTIFY_CFG  2u
#define VPCI_CAP_DEVICE_CFG  4u

#define VS_ACKNOWLEDGE  0x01u
#define VS_DRIVER       0x02u
#define VS_DRIVER_OK    0x04u
#define VS_FEATURES_OK  0x08u

/* VirtIO block feature bits */
#define VIRTIO_BLK_F_SIZE_MAX   (1u << 1)
#define VIRTIO_BLK_F_SEG_MAX    (1u << 2)
#define VIRTIO_BLK_F_BLK_SIZE   (1u << 6)
#define VIRTIO_F_VERSION_1      1u          /* page 1 bit 0 = bit 32 */

/* Request types */
#define VIRTIO_BLK_T_IN         0u   /* read from device */
#define VIRTIO_BLK_T_OUT        1u   /* write to device */

/* Status bytes (device writes after completing request) */
#define VIRTIO_BLK_S_OK         0u
#define VIRTIO_BLK_S_IOERR      1u
#define VIRTIO_BLK_S_UNSUPP     2u

/* ── Virtqueue types ─────────────────────────────────────────────────────── */
#define VBLK_QSIZ           8u
#define QUEUE_ALIGN         4096u
#define VIRTQ_DESC_F_NEXT   1u
#define VIRTQ_DESC_F_WRITE  2u

typedef struct {
    u64 addr;
    u32 len;
    u16 flags;
    u16 next;
} __attribute__((aligned(16))) virtq_desc_t;

typedef struct {
    u16 flags;
    u16 idx;
    u16 ring[VBLK_QSIZ];
} virtq_avail_t;

typedef struct { u32 id; u32 len; } virtq_used_elem_t;

typedef struct {
    u16              flags;
    u16              idx;
    virtq_used_elem_t ring[VBLK_QSIZ];
} virtq_used_t;

/* VirtIO block request header (16 bytes, device reads) */
typedef struct {
    u32 type;       /* VIRTIO_BLK_T_IN / OUT */
    u32 reserved;
    u64 sector;
} __attribute__((packed)) virtio_blk_req_hdr_t;

/* ── Static queue memory ─────────────────────────────────────────────────── */
static u8 q_mem[VBLK_QSIZ * 16u + 8192u] __attribute__((aligned(4096)));

static virtq_desc_t  *g_desc;
static virtq_avail_t *g_avail;
static virtq_used_t  *g_used;
static u16            g_last_used;

/* Per-request buffers (one request in flight at a time) */
static virtio_blk_req_hdr_t g_req_hdr   __attribute__((aligned(16)));
static u8                   g_req_status __attribute__((aligned(4)));

/* PCI MMIO addresses */
static uintptr_t g_cc;          /* COMMON_CFG base */
static uintptr_t g_nc_base;     /* NOTIFY_CFG base */
static u32       g_nc_mul;      /* notify_off_multiplier */
static uintptr_t g_dc;          /* DEVICE_CFG base (sector count) */

static int g_initialized;
static u64 g_num_sectors;

/* ── MMIO helpers ────────────────────────────────────────────────────────── */

#define DSB() __asm__ volatile("dsb sy"  ::: "memory")
#define DMB() __asm__ volatile("dmb ish" ::: "memory")
#define ISB() __asm__ volatile("isb"     ::: "memory")

static inline u8   vcfg_r8 (uintptr_t b, u32 o) { return *(volatile u8  *)(b+o); }
static inline u16  vcfg_r16(uintptr_t b, u32 o) { return *(volatile u16 *)(b+o); }
static inline u32  vcfg_r32(uintptr_t b, u32 o) { return *(volatile u32 *)(b+o); }
static inline void vcfg_w8 (uintptr_t b, u32 o, u8  v) { *(volatile u8  *)(b+o)=v; }
static inline void vcfg_w16(uintptr_t b, u32 o, u16 v) { *(volatile u16 *)(b+o)=v; }
static inline void vcfg_w32(uintptr_t b, u32 o, u32 v) { *(volatile u32 *)(b+o)=v; }

/* ── PCI capability walker (same as virtio_net.c) ───────────────────────── */

static int find_cap(const pci_dev_t *pdev, u8 cfg_type,
                    u8 *out_bar, u32 *out_off, u32 *out_extra)
{
    u8  bus = pdev->bus, d = pdev->dev, fn = pdev->fn;
    u16 status = pci_read16(bus, d, fn, PCI_STATUS);
    if (!(status & PCI_STS_CAP_LIST)) return 0;

    u8 ptr = pci_read8(bus, d, fn, PCI_CAP_PTR) & 0xFCu;
    while (ptr) {
        u8 cap_id   = pci_read8(bus, d, fn, ptr + 0u);
        u8 cap_next = pci_read8(bus, d, fn, ptr + 1u);
        if (cap_id == PCI_CAP_VNDR) {
            u8  ct  = pci_read8 (bus, d, fn, ptr + 3u);
            u8  bar = pci_read8 (bus, d, fn, ptr + 4u);
            u32 off = pci_read32(bus, d, fn, ptr + 8u);
            if (ct == cfg_type) {
                *out_bar = bar;
                *out_off = off;
                if (out_extra) *out_extra = pci_read32(bus, d, fn, ptr + 16u);
                return 1;
            }
        }
        ptr = cap_next & 0xFCu;
    }
    return 0;
}

/* ── Queue kick ──────────────────────────────────────────────────────────── */

static void kick_queue(void)
{
    vcfg_w16(g_cc, VCC_QUEUE_SEL, 0u);
    u16 ntfy_off = vcfg_r16(g_cc, VCC_QUEUE_NTFY_OFF);
    uintptr_t addr = g_nc_base + (uintptr_t)ntfy_off * (uintptr_t)g_nc_mul;
    *(volatile u16 *)addr = 0u;
}

/* ── Public: init ────────────────────────────────────────────────────────── */

int virtio_blk_init(void)
{
    pci_dev_t pdev;
    if (!pci_scan_virtio_blk(&pdev)) {
        kinfo("virtio-blk: no device found (disk image not attached)\n");
        return 0;
    }

    u8  cc_bar, nc_bar, dc_bar;
    u32 cc_off, nc_off, dc_off;
    if (!find_cap(&pdev, VPCI_CAP_COMMON_CFG, &cc_bar, &cc_off, NULL) ||
        !find_cap(&pdev, VPCI_CAP_NOTIFY_CFG, &nc_bar, &nc_off, &g_nc_mul) ||
        !find_cap(&pdev, VPCI_CAP_DEVICE_CFG, &dc_bar, &dc_off, NULL)) {
        kwarn("virtio-blk: missing PCI capabilities\n");
        return 0;
    }

    if (!pdev.bar[cc_bar] || !pdev.bar[nc_bar] || !pdev.bar[dc_bar]) {
        kwarn("virtio-blk: BAR not assigned\n");
        return 0;
    }

    g_cc      = pdev.bar[cc_bar] + cc_off;
    g_nc_base = pdev.bar[nc_bar] + nc_off;
    g_dc      = pdev.bar[dc_bar] + dc_off;

    /* Reset device */
    vcfg_w8(g_cc, VCC_DEV_STATUS, 0u);
    DSB(); ISB();

    /* Acknowledge + Driver */
    vcfg_w8(g_cc, VCC_DEV_STATUS, VS_ACKNOWLEDGE);
    vcfg_w8(g_cc, VCC_DEV_STATUS, VS_ACKNOWLEDGE | VS_DRIVER);

    /* Feature negotiation: VIRTIO_F_VERSION_1 only (minimal feature set) */
    vcfg_w32(g_cc, VCC_DRV_FEAT_SEL, 0u);
    vcfg_w32(g_cc, VCC_DRV_FEAT, 0u);   /* no device-specific features */
    vcfg_w32(g_cc, VCC_DRV_FEAT_SEL, 1u);
    vcfg_w32(g_cc, VCC_DRV_FEAT, VIRTIO_F_VERSION_1);

    vcfg_w8(g_cc, VCC_DEV_STATUS, VS_ACKNOWLEDGE | VS_DRIVER | VS_FEATURES_OK);
    DSB(); ISB();

    if (!(vcfg_r8(g_cc, VCC_DEV_STATUS) & VS_FEATURES_OK)) {
        kwarn("virtio-blk: FEATURES_OK refused\n");
        return 0;
    }

    /* Read capacity (u64 at device config offset 0) as two u32 reads — safer for MMIO alignment */
    u32 cap_lo    = vcfg_r32(g_dc, 0u);
    u32 cap_hi    = vcfg_r32(g_dc, 4u);
    g_num_sectors = ((u64)cap_hi << 32) | (u64)cap_lo;

    kinfo("virtio-blk: %lu sectors (%lu MB)\n",
          (unsigned long)g_num_sectors,
          (unsigned long)(g_num_sectors / 2048));

    /* Set up request queue (index 0) */
    g_desc  = (virtq_desc_t  *)q_mem;
    g_avail = (virtq_avail_t *)(q_mem + VBLK_QSIZ * sizeof(virtq_desc_t));
    g_used  = (virtq_used_t  *)(q_mem + QUEUE_ALIGN);
    g_last_used = 0;

    g_avail->flags = 1u;   /* suppress used-ring notifications */
    g_avail->idx   = 0u;

    vcfg_w16(g_cc, VCC_QUEUE_SEL, 0u);

    u16 qmax = vcfg_r16(g_cc, VCC_QUEUE_SIZE);
    u16 qsz  = (qmax < (u16)VBLK_QSIZ) ? qmax : (u16)VBLK_QSIZ;
    vcfg_w16(g_cc, VCC_QUEUE_SIZE, qsz);

    uintptr_t pa_desc  = (uintptr_t)g_desc;
    uintptr_t pa_avail = (uintptr_t)g_avail;
    uintptr_t pa_used  = (uintptr_t)g_used;

    vcfg_w32(g_cc, VCC_QUEUE_DESC_LO, (u32)pa_desc);
    vcfg_w32(g_cc, VCC_QUEUE_DESC_HI, (u32)(pa_desc >> 32));
    vcfg_w32(g_cc, VCC_QUEUE_DRV_LO,  (u32)pa_avail);
    vcfg_w32(g_cc, VCC_QUEUE_DRV_HI,  (u32)(pa_avail >> 32));
    vcfg_w32(g_cc, VCC_QUEUE_DEV_LO,  (u32)pa_used);
    vcfg_w32(g_cc, VCC_QUEUE_DEV_HI,  (u32)(pa_used >> 32));
    vcfg_w16(g_cc, VCC_QUEUE_ENABLE, 1u);

    /* DRIVER_OK */
    DMB();
    vcfg_w8(g_cc, VCC_DEV_STATUS,
            VS_ACKNOWLEDGE | VS_DRIVER | VS_FEATURES_OK | VS_DRIVER_OK);
    DMB();

    g_initialized = 1;
    kinfo("virtio-blk: ready\n");
    return 1;
}

int  virtio_blk_ready(void)        { return g_initialized; }
u64  virtio_blk_sector_count(void) { return g_num_sectors; }

/* ── Internal: submit one request and wait ───────────────────────────────── */

static int submit_request(u32 type, u64 lba, u32 num_sectors, u8 *data_buf)
{
    if (!g_initialized) return -1;

    /* Build request header */
    g_req_hdr.type     = type;
    g_req_hdr.reserved = 0;
    g_req_hdr.sector   = lba;
    g_req_status       = 0xFF;   /* device will overwrite with 0/1/2 */

    /*
     * 3-descriptor chain:
     *   desc[0] → req header  (device reads)
     *   desc[1] → data buffer (device writes for read, reads for write)
     *   desc[2] → status byte (device writes)
     */
    u16 d0 = g_avail->idx % VBLK_QSIZ;
    u16 d1 = (u16)((d0 + 1u) % VBLK_QSIZ);
    u16 d2 = (u16)((d0 + 2u) % VBLK_QSIZ);

    g_desc[d0].addr  = (u64)(uintptr_t)&g_req_hdr;
    g_desc[d0].len   = 16u;
    g_desc[d0].flags = VIRTQ_DESC_F_NEXT;
    g_desc[d0].next  = d1;

    g_desc[d1].addr  = (u64)(uintptr_t)data_buf;
    g_desc[d1].len   = num_sectors * VIRTIO_BLK_SECTOR_SIZE;
    g_desc[d1].flags = (u16)(VIRTQ_DESC_F_NEXT |
                              (type == VIRTIO_BLK_T_IN ? VIRTQ_DESC_F_WRITE : 0u));
    g_desc[d1].next  = d2;

    g_desc[d2].addr  = (u64)(uintptr_t)&g_req_status;
    g_desc[d2].len   = 1u;
    g_desc[d2].flags = VIRTQ_DESC_F_WRITE;
    g_desc[d2].next  = 0;

    /* Place head descriptor into avail ring */
    u16 avail_slot = g_avail->idx % VBLK_QSIZ;
    g_avail->ring[avail_slot] = d0;
    DMB();
    g_avail->idx++;
    DMB();

    kick_queue();

    /* Busy-wait for completion (used ring advances by one entry) */
    u64 timeout = 500000000ULL;
    while (g_used->idx == g_last_used && timeout--) {
        __asm__ volatile("nop");
        DMB();
    }

    if (g_used->idx == g_last_used) {
        kwarn("virtio-blk: request timeout (lba=%lu type=%u)\n",
              (unsigned long)lba, (unsigned)type);
        return -1;
    }

    g_last_used = g_used->idx;

    if (g_req_status != VIRTIO_BLK_S_OK) {
        kwarn("virtio-blk: device error status=%u lba=%lu\n",
              (unsigned)g_req_status, (unsigned long)lba);
        return -1;
    }

    return 0;
}

/* ── Public: read / write ────────────────────────────────────────────────── */

int virtio_blk_read_sectors(u64 lba, u32 count, u8 *buf)
{
    if (!buf || count == 0) return -1;
    return submit_request(VIRTIO_BLK_T_IN, lba, count, buf);
}

int virtio_blk_write_sectors(u64 lba, u32 count, const u8 *buf)
{
    if (!buf || count == 0) return -1;
    /* The API takes const u8* but submit needs u8*; write path does not modify buf */
    return submit_request(VIRTIO_BLK_T_OUT, lba, count, (u8 *)buf);
}
