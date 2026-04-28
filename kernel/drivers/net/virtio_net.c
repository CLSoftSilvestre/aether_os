/*
 * AetherOS — VirtIO net PCI driver (modern VirtIO 1.x, Phase 5.1)
 * File: kernel/drivers/net/virtio_net.c
 *
 * QEMU: -device virtio-net-pci,netdev=n0 -netdev user,id=n0
 * PCI vendor 0x1AF4, device 0x1041 (VirtIO ID 1 = net).
 *
 * Two queues: 0 = receiveq, 1 = transmitq.
 * RX is polled at 100 Hz from net_rx_poll() in arm_timer.c.
 * TX is synchronous (one packet at a time).
 *
 * virtio_net_hdr without MRGRXBUF = 10 bytes (no num_buffers field).
 */

#include "drivers/net/virtio_net.h"
#include "drivers/pci/pci_ecam.h"
#include "aether/net.h"
#include "aether/printk.h"
#include "aether/types.h"

/* ── VirtIO PCI common-config offsets ───────────────────────────────────── */
#define VCC_DEV_FEAT_SEL   0x00u
#define VCC_DEV_FEAT       0x04u
#define VCC_DRV_FEAT_SEL   0x08u
#define VCC_DRV_FEAT       0x0Cu
#define VCC_MSIX_CONFIG    0x10u
#define VCC_NUM_QUEUES     0x12u
#define VCC_DEV_STATUS     0x14u
#define VCC_CFG_GEN        0x15u
#define VCC_QUEUE_SEL      0x16u
#define VCC_QUEUE_SIZE     0x18u
#define VCC_QUEUE_MSIX_VEC 0x1Au
#define VCC_QUEUE_ENABLE   0x1Cu
#define VCC_QUEUE_NTFY_OFF 0x1Eu
#define VCC_QUEUE_DESC_LO  0x20u
#define VCC_QUEUE_DESC_HI  0x24u
#define VCC_QUEUE_DRV_LO   0x28u
#define VCC_QUEUE_DRV_HI   0x2Cu
#define VCC_QUEUE_DEV_LO   0x30u
#define VCC_QUEUE_DEV_HI   0x34u

/* VirtIO PCI capability types */
#define VPCI_CAP_COMMON_CFG  1u
#define VPCI_CAP_NOTIFY_CFG  2u
#define VPCI_CAP_ISR_CFG     3u
#define VPCI_CAP_DEVICE_CFG  4u

/* VirtIO device status bits */
#define VS_ACKNOWLEDGE   0x01u
#define VS_DRIVER        0x02u
#define VS_DRIVER_OK     0x04u
#define VS_FEATURES_OK   0x08u

/* VirtIO net feature bits */
#define VIRTIO_NET_F_MAC      (1u << 5)
#define VIRTIO_F_VERSION_1    (1u)        /* page 1 bit 0 = bit 32 total */

/* VirtIO PCI IDs */
#define VIRTIO_VENDOR       0x1AF4u
#define VIRTIO_DEV_NET      0x1041u

/* ── Virtqueue layout ────────────────────────────────────────────────────── */
#define VNET_QSIZ       16u
#define VNET_HDR_SZ     10u             /* virtio_net_hdr without num_buffers */
#define VNET_FRAME_SZ   1514u
#define VNET_BUF_SZ     (VNET_HDR_SZ + VNET_FRAME_SZ)   /* 1524 */
#define QUEUE_ALIGN     4096u
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
    u16 ring[VNET_QSIZ];
} virtq_avail_t;

typedef struct { u32 id; u32 len; } virtq_used_elem_t;

typedef struct {
    u16              flags;
    u16              idx;
    virtq_used_elem_t ring[VNET_QSIZ];
} virtq_used_t;

/* virtio_net_hdr (no MRGRXBUF feature) */
typedef struct {
    u8  flags;
    u8  gso_type;
    u16 hdr_len;
    u16 gso_size;
    u16 csum_start;
    u16 csum_offset;
} __attribute__((packed)) vnet_hdr_t;    /* 10 bytes */

/* ── Static queue memory ─────────────────────────────────────────────────── */

/* RX queue: 16 descriptors, each VNET_BUF_SZ bytes */
static u8 rx_mem[VNET_QSIZ * 16u + 8192u]  __attribute__((aligned(4096)));
static u8 rx_buf[VNET_QSIZ][VNET_BUF_SZ]   __attribute__((aligned(4)));

/* TX queue */
static u8 tx_mem[VNET_QSIZ * 16u + 8192u]  __attribute__((aligned(4096)));
static u8 tx_buf[VNET_BUF_SZ]               __attribute__((aligned(4)));

static virtq_desc_t  *g_rx_desc;
static virtq_avail_t *g_rx_avail;
static virtq_used_t  *g_rx_used;
static u16            g_rx_last_used;

static virtq_desc_t  *g_tx_desc;
static virtq_avail_t *g_tx_avail;
static virtq_used_t  *g_tx_used;
static u16            g_tx_last_used;

/* PCI BAR MMIO addresses */
static uintptr_t g_cc;          /* COMMON_CFG base */
static uintptr_t g_nc_base;     /* NOTIFY_CFG base */
static u32       g_nc_mul;      /* notify_off_multiplier */
static uintptr_t g_dc;          /* DEVICE_CFG base (for MAC) */

static int g_net_initialized;

/* ── MMIO helpers ────────────────────────────────────────────────────────── */

#define DSB() __asm__ volatile("dsb sy" ::: "memory")
#define DMB() __asm__ volatile("dmb ish" ::: "memory")
#define ISB() __asm__ volatile("isb" ::: "memory")

static inline u8   vcfg_r8 (uintptr_t b, u32 o) { return *(volatile u8  *)(b+o); }
static inline u16  vcfg_r16(uintptr_t b, u32 o) { return *(volatile u16 *)(b+o); }
static inline u32  vcfg_r32(uintptr_t b, u32 o) { return *(volatile u32 *)(b+o); }
static inline void vcfg_w8 (uintptr_t b, u32 o, u8  v) { *(volatile u8  *)(b+o)=v; }
static inline void vcfg_w16(uintptr_t b, u32 o, u16 v) { *(volatile u16 *)(b+o)=v; }
static inline void vcfg_w32(uintptr_t b, u32 o, u32 v) { *(volatile u32 *)(b+o)=v; }

/* ── PCI capability walker (same logic as virtio_input.c) ───────────────── */

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

/* ── Queue helpers ───────────────────────────────────────────────────────── */

static void setup_queue(u8 qidx, u8 *mem_block,
                        virtq_desc_t **desc_out,
                        virtq_avail_t **avail_out,
                        virtq_used_t  **used_out)
{
    virtq_desc_t  *desc  = (virtq_desc_t  *)mem_block;
    virtq_avail_t *avail = (virtq_avail_t *)(mem_block + VNET_QSIZ * sizeof(virtq_desc_t));
    virtq_used_t  *used  = (virtq_used_t  *)(mem_block + QUEUE_ALIGN);

    *desc_out  = desc;
    *avail_out = avail;
    *used_out  = used;

    vcfg_w16(g_cc, VCC_QUEUE_SEL, qidx);

    u16 qmax = vcfg_r16(g_cc, VCC_QUEUE_SIZE);
    u16 qsz  = (qmax < (u16)VNET_QSIZ) ? qmax : (u16)VNET_QSIZ;
    vcfg_w16(g_cc, VCC_QUEUE_SIZE, qsz);

    uintptr_t pa_desc  = (uintptr_t)desc;
    uintptr_t pa_avail = (uintptr_t)avail;
    uintptr_t pa_used  = (uintptr_t)used;

    vcfg_w32(g_cc, VCC_QUEUE_DESC_LO, (u32)pa_desc);
    vcfg_w32(g_cc, VCC_QUEUE_DESC_HI, (u32)(pa_desc >> 32));
    vcfg_w32(g_cc, VCC_QUEUE_DRV_LO,  (u32)pa_avail);
    vcfg_w32(g_cc, VCC_QUEUE_DRV_HI,  (u32)(pa_avail >> 32));
    vcfg_w32(g_cc, VCC_QUEUE_DEV_LO,  (u32)pa_used);
    vcfg_w32(g_cc, VCC_QUEUE_DEV_HI,  (u32)(pa_used >> 32));
}

static void kick_queue(u16 qidx)
{
    vcfg_w16(g_cc, VCC_QUEUE_SEL, qidx);
    u16 ntfy_off = vcfg_r16(g_cc, VCC_QUEUE_NTFY_OFF);
    uintptr_t kick_addr = g_nc_base + (uintptr_t)ntfy_off * (uintptr_t)g_nc_mul;
    *(volatile u16 *)kick_addr = qidx;
}

/* ── Public: init ────────────────────────────────────────────────────────── */

int virtio_net_init(void)
{
    /* Scan PCI bus 0 for virtio-net (0x1AF4:0x1041) */
    pci_dev_t pdev;
    if (!pci_scan_virtio_net(&pdev)) {
        kwarn("virtio-net: no device found\n");
        return 0;
    }

    /* Find capabilities */
    u8 cc_bar, nc_bar, dc_bar;
    u32 cc_off, nc_off, dc_off;
    if (!find_cap(&pdev, VPCI_CAP_COMMON_CFG, &cc_bar, &cc_off, NULL) ||
        !find_cap(&pdev, VPCI_CAP_NOTIFY_CFG, &nc_bar, &nc_off, &g_nc_mul) ||
        !find_cap(&pdev, VPCI_CAP_DEVICE_CFG, &dc_bar, &dc_off, NULL)) {
        kwarn("virtio-net: missing capabilities\n");
        return 0;
    }

    if (!pdev.bar[cc_bar] || !pdev.bar[nc_bar] || !pdev.bar[dc_bar]) {
        kwarn("virtio-net: BAR not assigned\n");
        return 0;
    }

    g_cc      = pdev.bar[cc_bar] + cc_off;
    g_nc_base = pdev.bar[nc_bar] + nc_off;
    g_dc      = pdev.bar[dc_bar] + dc_off;

    kdebug("virtio-net: cc=0x%lx nc=0x%lx nc_mul=%lu dc=0x%lx\n",
           (unsigned long)g_cc, (unsigned long)g_nc_base,
           (unsigned long)g_nc_mul, (unsigned long)g_dc);

    /* Reset */
    vcfg_w8(g_cc, VCC_DEV_STATUS, 0u);
    DSB(); ISB();

    /* Acknowledge + Driver */
    vcfg_w8(g_cc, VCC_DEV_STATUS, VS_ACKNOWLEDGE);
    vcfg_w8(g_cc, VCC_DEV_STATUS, VS_ACKNOWLEDGE | VS_DRIVER);

    /* Feature negotiation: VIRTIO_F_VERSION_1 + VIRTIO_NET_F_MAC */
    vcfg_w32(g_cc, VCC_DRV_FEAT_SEL, 0u);
    vcfg_w32(g_cc, VCC_DRV_FEAT, VIRTIO_NET_F_MAC);
    vcfg_w32(g_cc, VCC_DRV_FEAT_SEL, 1u);
    vcfg_w32(g_cc, VCC_DRV_FEAT, VIRTIO_F_VERSION_1);

    vcfg_w8(g_cc, VCC_DEV_STATUS, VS_ACKNOWLEDGE | VS_DRIVER | VS_FEATURES_OK);
    DSB(); ISB();

    if (!(vcfg_r8(g_cc, VCC_DEV_STATUS) & VS_FEATURES_OK)) {
        kwarn("virtio-net: FEATURES_OK refused\n");
        return 0;
    }

    /* Read MAC address from device config region */
    for (int i = 0; i < 6; i++)
        g_our_mac[i] = vcfg_r8(g_dc, (u32)i);

    kinfo("virtio-net: MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
          g_our_mac[0], g_our_mac[1], g_our_mac[2],
          g_our_mac[3], g_our_mac[4], g_our_mac[5]);

    /* Set up RX queue (0) */
    setup_queue(0, rx_mem, &g_rx_desc, &g_rx_avail, &g_rx_used);
    g_rx_avail->flags = 1u;   /* suppress used-ring notifications */
    g_rx_avail->idx   = 0u;
    g_rx_last_used    = 0u;

    for (u32 i = 0; i < VNET_QSIZ; i++) {
        g_rx_desc[i].addr  = (u64)(uintptr_t)rx_buf[i];
        g_rx_desc[i].len   = VNET_BUF_SZ;
        g_rx_desc[i].flags = VIRTQ_DESC_F_WRITE;
        g_rx_desc[i].next  = 0;
        g_rx_avail->ring[i] = (u16)i;
        g_rx_avail->idx++;
    }

    /* DSB ensures all descriptor/avail ring writes (Normal memory) are
     * visible to QEMU before the QUEUE_ENABLE MMIO write (Device memory).
     * Without DSB, ARM allows the Device write to be observed first, so
     * QEMU would enable the queue before seeing any RX descriptors. */
    DSB();
    vcfg_w16(g_cc, VCC_QUEUE_SEL, 0u);
    vcfg_w16(g_cc, VCC_QUEUE_ENABLE, 1u);

    /* Set up TX queue (1) */
    setup_queue(1, tx_mem, &g_tx_desc, &g_tx_avail, &g_tx_used);
    g_tx_avail->flags = 1u;
    g_tx_avail->idx   = 0u;
    g_tx_last_used    = 0u;

    DSB();
    vcfg_w16(g_cc, VCC_QUEUE_SEL, 1u);
    vcfg_w16(g_cc, VCC_QUEUE_ENABLE, 1u);

    /* DRIVER_OK */
    DSB();
    vcfg_w8(g_cc, VCC_DEV_STATUS,
            VS_ACKNOWLEDGE | VS_DRIVER | VS_FEATURES_OK | VS_DRIVER_OK);
    DSB();

    /* Initial RX kick — DSB above ensures DRIVER_OK write is visible first */
    kick_queue(0u);

    g_net_initialized = 1;
    kinfo("virtio-net: ready\n");
    return 1;
}

/* ── Public: transmit ────────────────────────────────────────────────────── */

int virtio_net_tx(const u8 *frame, u16 len)
{
    if (!g_net_initialized) return -1;
    if (len > VNET_FRAME_SZ) return -1;

    /* Build buffer: virtio_net_hdr (zeroed) + frame */
    vnet_hdr_t *hdr = (vnet_hdr_t *)tx_buf;
    hdr->flags = hdr->gso_type = hdr->hdr_len =
    hdr->gso_size = hdr->csum_start = hdr->csum_offset = 0;

    u8 *payload = tx_buf + VNET_HDR_SZ;
    for (u16 i = 0; i < len; i++) payload[i] = frame[i];

    u16 slot = g_tx_avail->idx % VNET_QSIZ;
    g_tx_desc[slot].addr  = (u64)(uintptr_t)tx_buf;
    g_tx_desc[slot].len   = (u32)(VNET_HDR_SZ + len);
    g_tx_desc[slot].flags = 0;
    g_tx_desc[slot].next  = 0;

    DMB();
    g_tx_avail->ring[slot] = (u16)slot;
    DMB();
    g_tx_avail->idx++;
    DMB();

    kick_queue(1u);

    /* Busy-wait for TX completion (synchronous path) */
    u64 timeout = 10000000ULL;
    while (g_tx_used->idx == g_tx_last_used && timeout--) {
        __asm__ volatile("nop");
        DMB();
    }
    if (g_tx_used->idx != g_tx_last_used) {
        g_tx_last_used = g_tx_used->idx;
    } else {
        kdebug("virtio-net: TX timeout (used=%u avail=%u)\n",
             (unsigned)g_tx_used->idx, (unsigned)g_tx_avail->idx);
    }

    return 0;
}

/* ── Public: poll RX ─────────────────────────────────────────────────────── */

void virtio_net_rx_poll(void)
{
    if (!g_net_initialized) return;

    DMB();
    while (g_rx_used->idx != g_rx_last_used) {
        u16 slot    = g_rx_last_used % VNET_QSIZ;
        u32 desc_id = g_rx_used->ring[slot].id;
        u32 used_len = g_rx_used->ring[slot].len;
        g_rx_last_used++;

        if (used_len > VNET_HDR_SZ) {
            u8  *buf       = rx_buf[desc_id];
            u16  frame_len = (u16)(used_len - VNET_HDR_SZ);
            kdebug("virtio-net: RX frame len=%u\n", (unsigned)frame_len);
            /* Deliver Ethernet frame (skip virtio_net_hdr) */
            net_deliver_frame(buf + VNET_HDR_SZ, frame_len);
        }

        /* Rearm descriptor */
        g_rx_desc[desc_id].addr  = (u64)(uintptr_t)rx_buf[desc_id];
        g_rx_desc[desc_id].len   = VNET_BUF_SZ;
        g_rx_desc[desc_id].flags = VIRTQ_DESC_F_WRITE;
        g_rx_desc[desc_id].next  = 0;

        u16 avail_slot = g_rx_avail->idx % VNET_QSIZ;
        g_rx_avail->ring[avail_slot] = (u16)desc_id;
        DMB();
        g_rx_avail->idx++;
        DMB();
    }

    /* Kick RX queue to tell device new buffers are available */
    kick_queue(0u);
}
