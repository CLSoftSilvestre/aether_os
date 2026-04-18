/*
 * AetherOS — VirtIO-Input Driver (MMIO transport, legacy v1)
 * File: kernel/drivers/input/virtio_input.c
 *
 * QEMU 10.x virt machine uses VirtIO MMIO v1 (force-legacy=true).
 * Devices are allocated from the top of the 32-slot bank; in practice
 * the tablet lands on slot 31 (MMIO 0x0a003e00, GIC INTID 79).
 *
 * V1 queue layout  (QueueAlign = 4096, QueueNum = 16):
 *   [0x0000] descriptor table  16 × 16 bytes = 256 bytes
 *   [0x0100] available ring    4 + 16×2 = 36 bytes
 *   [0x1000] used ring         4 + 16×8 = 132 bytes  (next page boundary)
 *
 * Add  -device virtio-tablet-device  to the QEMU command line.
 */

#include "drivers/input/virtio_input.h"
#include "drivers/input/pl050_mouse.h"
#include "drivers/input/keycodes.h"
#include "drivers/video/cursor.h"
#include "drivers/irq/gic_v2.h"
#include "aether/printk.h"
#include "aether/types.h"

/* ── VirtIO MMIO register offsets (common to v1 and v2) ─────────────────── */

#define VMIO_MAGIC              0x000u
#define VMIO_VERSION            0x004u
#define VMIO_DEVICE_ID          0x008u
#define VMIO_QUEUE_SEL          0x030u
#define VMIO_QUEUE_NUM_MAX      0x034u
#define VMIO_QUEUE_NUM          0x038u
#define VMIO_QUEUE_NOTIFY       0x050u
#define VMIO_INT_STATUS         0x060u
#define VMIO_INT_ACK            0x064u
#define VMIO_STATUS             0x070u

/* V1 (legacy) only ─────────────────────────────────────────────────────── */
#define VMIO_GUEST_FEATURES     0x020u   /* accepted feature bits (W) */
#define VMIO_GUEST_PAGE_SIZE    0x028u   /* page size used by driver (W) */
#define VMIO_QUEUE_ALIGN        0x03cu   /* used-ring alignment within page (W) */
#define VMIO_QUEUE_PFN          0x040u   /* queue physical page number (RW) */

/* ── VirtIO constants ────────────────────────────────────────────────────── */

#define VIRTIO_MAGIC_VALUE    0x74726976u
#define VIRTIO_VERSION_V1     1u
#define VIRTIO_DEV_INPUT      18u

#define VIRTIO_S_ACKNOWLEDGE  1u
#define VIRTIO_S_DRIVER       2u
#define VIRTIO_S_DRIVER_OK    4u

/* ── MMIO scan range ─────────────────────────────────────────────────────── */

#define VIRTIO_MMIO_BASE    0x0a000000UL
#define VIRTIO_MMIO_STRIDE  0x200u
#define VIRTIO_MMIO_COUNT   32u
#define VIRTIO_MMIO_IRQ0    48u     /* transport 0 → SPI 16 → INTID 48 */

/* ── Virtqueue layout ────────────────────────────────────────────────────── */

#define VIRTQ_SIZE         16u
#define VIRTQ_DESC_F_WRITE  2u      /* buffer is device-writable */
#define QUEUE_ALIGN        4096u

typedef struct {
    u64 addr;
    u32 len;
    u16 flags;
    u16 next;
} virtq_desc_t;

typedef struct {
    u16 flags;
    u16 idx;
    u16 ring[VIRTQ_SIZE];
} virtq_avail_t;

typedef struct { u32 id; u32 len; } virtq_used_elem_t;

typedef struct {
    u16              flags;
    u16              idx;
    virtq_used_elem_t ring[VIRTQ_SIZE];
} virtq_used_t;

/* ── VirtIO-input event (spec §5.8) ─────────────────────────────────────── */

typedef struct { u16 type; u16 code; u32 value; } virtio_input_event_t;

#define EV_SYN      0u
#define EV_KEY      1u
#define EV_ABS      3u
#define ABS_X       0u
#define ABS_Y       1u
#define BTN_LEFT    0x110u
#define BTN_RIGHT   0x111u
#define BTN_MIDDLE  0x112u

#define TABLET_MAX   32767u
#define SCREEN_W     1024u
#define SCREEN_H      768u

/* ── Queue memory (v1: contiguous, page-aligned) ─────────────────────────── */
/*
 * Layout inside vi_queue_mem[]:
 *   [0x0000] vi_desc[16]   — descriptor table (256 bytes)
 *   [0x0100] vi_avail      — available ring   (36 bytes)
 *   [0x1000] vi_used       — used ring        (132 bytes, at next page)
 *
 * QueuePFN = (phys_addr of vi_queue_mem) >> 12
 */
static u8 vi_queue_mem[8192] __attribute__((aligned(4096)));

static virtq_desc_t  *vi_desc;
static virtq_avail_t *vi_avail;
static virtq_used_t  *vi_used;

static virtio_input_event_t vi_bufs[VIRTQ_SIZE];

/* ── Module state ────────────────────────────────────────────────────────── */

static volatile u32 *vi_base;
static int vi_irq = -1;

static u16 vi_last_used;

static u32 vi_cur_x;
static u32 vi_cur_y;
static u8  vi_cur_btns;
static int vi_has_pos;

/* ── MMIO helpers ────────────────────────────────────────────────────────── */

static inline u32  vr(u32 off) { return vi_base[off >> 2]; }
static inline void vw(u32 off, u32 val) { vi_base[off >> 2] = val; }

/* ── IRQ handler ─────────────────────────────────────────────────────────── */

void virtio_input_irq_handler(void)
{
    vw(VMIO_INT_ACK, vr(VMIO_INT_STATUS));
    __asm__ volatile("dmb ish" ::: "memory");

    while (vi_last_used != vi_used->idx) {
        u16 slot    = (u16)(vi_last_used % VIRTQ_SIZE);
        u32 desc_id = vi_used->ring[slot].id;
        vi_last_used++;

        virtio_input_event_t *ev = &vi_bufs[desc_id];

        if (ev->type == EV_ABS) {
            if (ev->code == ABS_X) {
                vi_cur_x  = (u32)ev->value * SCREEN_W / (TABLET_MAX + 1u);
                vi_has_pos = 1;
            } else if (ev->code == ABS_Y) {
                vi_cur_y  = (u32)ev->value * SCREEN_H / (TABLET_MAX + 1u);
                vi_has_pos = 1;
            }
        } else if (ev->type == EV_KEY) {
            u8 p = (ev->value != 0u) ? 1u : 0u;
            if      (ev->code == BTN_LEFT)   vi_cur_btns = (u8)((vi_cur_btns & ~1u) | p);
            else if (ev->code == BTN_RIGHT)  vi_cur_btns = (u8)((vi_cur_btns & ~2u) | (u8)(p << 1));
            else if (ev->code == BTN_MIDDLE) vi_cur_btns = (u8)((vi_cur_btns & ~4u) | (u8)(p << 2));
        } else if (ev->type == EV_SYN && ev->code == 0u) {
            if (vi_has_pos) {
                /* Move cursor immediately in IRQ context — no poll-loop lag */
                cursor_move(vi_cur_x, vi_cur_y);
                /* Also push to mouse ring so userspace can read button state */
                mouse_event_t me;
                me.x = vi_cur_x; me.y = vi_cur_y; me.buttons = vi_cur_btns;
                mouse_post_event(mouse_event_pack(me));
            }
        }

        /* Return descriptor to available ring and notify device */
        vi_avail->ring[vi_avail->idx % VIRTQ_SIZE] = (u16)desc_id;
        __asm__ volatile("dmb ish" ::: "memory");
        vi_avail->idx++;
        vw(VMIO_QUEUE_NOTIFY, 0u);
    }
}

int virtio_input_irq(void) { return vi_irq; }

/* ── Initialisation ─────────────────────────────────────────────────────── */

void virtio_input_init(void)
{
    /* Set up queue memory pointers */
    vi_desc  = (virtq_desc_t  *)(vi_queue_mem);
    vi_avail = (virtq_avail_t *)(vi_queue_mem + VIRTQ_SIZE * sizeof(virtq_desc_t));
    vi_used  = (virtq_used_t  *)(vi_queue_mem + QUEUE_ALIGN);

    for (u32 i = 0; i < VIRTIO_MMIO_COUNT; i++) {
        volatile u32 *base =
            (volatile u32 *)(VIRTIO_MMIO_BASE + (uintptr_t)i * VIRTIO_MMIO_STRIDE);

        if (base[VMIO_MAGIC    >> 2] != VIRTIO_MAGIC_VALUE) continue;
        if (base[VMIO_VERSION  >> 2] != VIRTIO_VERSION_V1)  continue;
        if (base[VMIO_DEVICE_ID >> 2] != VIRTIO_DEV_INPUT)  continue;

        vi_base = base;
        vi_irq  = (int)(VIRTIO_MMIO_IRQ0 + i);

        kinfo("virtio-input: slot %u  MMIO 0x%lx  IRQ %d (v1/legacy)\n",
              i, VIRTIO_MMIO_BASE + i * VIRTIO_MMIO_STRIDE, vi_irq);

        /* V1 init sequence (spec §4.2.3.1) */
        vw(VMIO_STATUS, 0u);                                      /* reset */
        vw(VMIO_STATUS, VIRTIO_S_ACKNOWLEDGE);
        vw(VMIO_STATUS, VIRTIO_S_ACKNOWLEDGE | VIRTIO_S_DRIVER);
        vw(VMIO_GUEST_FEATURES, 0u);                              /* no optional features */
        vw(VMIO_GUEST_PAGE_SIZE, QUEUE_ALIGN);                    /* 4096-byte pages */

        /* Queue 0 (eventq) */
        vw(VMIO_QUEUE_SEL, 0u);
        u32 qmax = vr(VMIO_QUEUE_NUM_MAX);
        u32 qsz  = (VIRTQ_SIZE < qmax) ? VIRTQ_SIZE : qmax;
        vw(VMIO_QUEUE_NUM, qsz);
        vw(VMIO_QUEUE_ALIGN, QUEUE_ALIGN);

        /* Descriptor table: each entry points to one event buffer */
        for (u32 j = 0; j < qsz; j++) {
            vi_desc[j].addr  = (u64)(uintptr_t)&vi_bufs[j];
            vi_desc[j].len   = (u32)sizeof(virtio_input_event_t);
            vi_desc[j].flags = (u16)VIRTQ_DESC_F_WRITE;
            vi_desc[j].next  = 0;
        }

        /* Offer all descriptors to the device */
        vi_avail->flags = 0;
        vi_avail->idx   = 0;
        for (u32 j = 0; j < qsz; j++) {
            vi_avail->ring[j] = (u16)j;
            vi_avail->idx++;
        }

        /* DMB before handing the queue to the device */
        __asm__ volatile("dmb ish" ::: "memory");

        /* QueuePFN: physical page number of the descriptor table */
        vw(VMIO_QUEUE_PFN, (u32)((uintptr_t)vi_queue_mem >> 12));

        /* Activate */
        vw(VMIO_STATUS, VIRTIO_S_ACKNOWLEDGE | VIRTIO_S_DRIVER | VIRTIO_S_DRIVER_OK);

        vi_cur_x    = SCREEN_W / 2u;
        vi_cur_y    = SCREEN_H / 2u;
        vi_cur_btns = 0u;
        vi_has_pos  = 0;
        vi_last_used = 0u;

        gic_enable_irq((u32)vi_irq);
        vw(VMIO_QUEUE_NOTIFY, 0u);   /* kick — tell device descriptors are ready */

        kinfo("virtio-input: ready  qsz=%u  QueuePFN=0x%lx\n",
              qsz, (unsigned long)((uintptr_t)vi_queue_mem >> 12));
        return;
    }

    kinfo("virtio-input: no device found (add -device virtio-tablet-device)\n");
}
