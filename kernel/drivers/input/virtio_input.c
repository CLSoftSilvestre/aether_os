/*
 * AetherOS — VirtIO-Input Driver (MMIO v1/v2 + PCI modern transport)
 * File: kernel/drivers/input/virtio_input.c
 *
 * Supports up to 2 VirtIO input devices.  Transport priority:
 *   1. PCI modern (virtio-tablet-pci / virtio-keyboard-pci)
 *   2. MMIO v2 (virtio-tablet-device,bus=virtio-mmio-bus.N)
 *   3. MMIO v1 / legacy (same bus, older QEMU)
 *
 * Input is collected by polling the used ring at 100 Hz from the timer IRQ
 * via virtio_input_poll().  No GIC IRQ wiring is required.
 */

#include "drivers/input/virtio_input.h"
#include "drivers/input/pl050_kbd.h"
#include "drivers/input/pl050_mouse.h"
#include "drivers/input/keycodes.h"
#include "drivers/pci/pci_ecam.h"
#include "aether/printk.h"
#include "aether/types.h"

/* ── VirtIO MMIO register offsets (all 32-bit) ───────────────────────────── */

#define VMMIO_MAGIC          0x000u
#define VMMIO_VERSION        0x004u
#define VMMIO_DEVICE_ID      0x008u
#define VMMIO_VENDOR_ID      0x00Cu
#define VMMIO_DEV_FEAT       0x010u
#define VMMIO_DEV_FEAT_SEL   0x014u
#define VMMIO_DRV_FEAT       0x020u
#define VMMIO_DRV_FEAT_SEL   0x024u
#define VMMIO_QUEUE_SEL      0x030u
#define VMMIO_QUEUE_NUM_MAX  0x034u
#define VMMIO_QUEUE_NUM      0x038u
#define VMMIO_QUEUE_READY    0x044u
#define VMMIO_QUEUE_NOTIFY   0x050u
#define VMMIO_INT_STATUS     0x060u
#define VMMIO_INT_ACK        0x064u
#define VMMIO_STATUS         0x070u
#define VMMIO_Q_DESC_LO      0x080u
#define VMMIO_Q_DESC_HI      0x084u
#define VMMIO_Q_AVAIL_LO     0x090u
#define VMMIO_Q_AVAIL_HI     0x094u
#define VMMIO_Q_USED_LO      0x0A0u
#define VMMIO_Q_USED_HI      0x0A4u

/* VirtIO MMIO v1 (legacy) additional registers */
#define VMMIO_GUEST_PAGE_SIZE 0x028u
#define VMMIO_QUEUE_ALIGN     0x03Cu
#define VMMIO_QUEUE_PFN       0x04Cu

/* VirtIO device status bits */
#define VS_ACKNOWLEDGE   0x01u
#define VS_DRIVER        0x02u
#define VS_DRIVER_OK     0x04u
#define VS_FEATURES_OK   0x08u

/* QEMU virt virtio-mmio bus */
#define VMMIO_BASE       0x0A000000UL
#define VMMIO_SLOT_SIZE  0x200UL
#define VMMIO_MAX_SLOTS  32

#define VIRTIO_MAGIC    0x74726976UL
#define VIRTIO_VERSION2 2UL
#define VIRTIO_ID_INPUT 18UL

/* ── VirtIO PCI modern — capability types ────────────────────────────────── */

#define VPCI_CAP_COMMON_CFG  1u   /* common configuration  */
#define VPCI_CAP_NOTIFY_CFG  2u   /* notification region   */

/* VirtIO common config register offsets (MMIO within the BAR region) */
#define VCC_DEV_FEAT_SEL   0x00u  /* u32 rw */
#define VCC_DEV_FEAT       0x04u  /* u32 r  */
#define VCC_DRV_FEAT_SEL   0x08u  /* u32 rw */
#define VCC_DRV_FEAT       0x0Cu  /* u32 rw */
#define VCC_MSIX_CONFIG    0x10u  /* u16 rw */
#define VCC_NUM_QUEUES     0x12u  /* u16 r  */
#define VCC_DEV_STATUS     0x14u  /* u8  rw */
#define VCC_CFG_GEN        0x15u  /* u8  r  */
#define VCC_QUEUE_SEL      0x16u  /* u16 rw */
#define VCC_QUEUE_SIZE     0x18u  /* u16 rw */
#define VCC_QUEUE_MSIX_VEC 0x1Au  /* u16 rw */
#define VCC_QUEUE_ENABLE   0x1Cu  /* u16 rw */
#define VCC_QUEUE_NTFY_OFF 0x1Eu  /* u16 r  */
#define VCC_QUEUE_DESC_LO  0x20u  /* u32 rw */
#define VCC_QUEUE_DESC_HI  0x24u  /* u32 rw */
#define VCC_QUEUE_DRV_LO   0x28u  /* u32 rw */
#define VCC_QUEUE_DRV_HI   0x2Cu  /* u32 rw */
#define VCC_QUEUE_DEV_LO   0x30u  /* u32 rw */
#define VCC_QUEUE_DEV_HI   0x34u  /* u32 rw */

/* ── Virtqueue layout ────────────────────────────────────────────────────── */

#define VIRTQ_SIZE         16u
#define VIRTQ_DESC_F_WRITE  2u
#define QUEUE_ALIGN      4096u

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

/* ── VirtIO-input event (spec §5.8) ──────────────────────────────────────── */

typedef struct { u16 type; u16 code; u32 value; } virtio_input_event_t;

#define EV_SYN     0u
#define EV_KEY     1u
#define EV_REL     2u
#define EV_ABS     3u
#define REL_X      0u
#define REL_Y      1u
#define ABS_X      0u
#define ABS_Y      1u
#define BTN_LEFT   0x110u
#define BTN_RIGHT  0x111u
#define BTN_MIDDLE 0x112u

#define TABLET_MAX  32767u
#define SCREEN_W     1024u
#define SCREEN_H      768u

/* ── Per-device state ────────────────────────────────────────────────────── */

#define VI_MAX_DEVS 2

typedef struct {
    volatile u32         *base;        /* MMIO slot base (NULL for PCI) */
    uintptr_t             notify_addr; /* PCI kick register address */
    int                   is_pci;      /* 1 = PCI modern, 0 = MMIO */
    virtq_desc_t         *desc;
    virtq_avail_t        *avail;
    virtq_used_t         *used;
    virtio_input_event_t *bufs;
    u16  last_used;
    u32  cur_x, cur_y;
    u8   cur_btns;
    int  has_pos;
    int  dbg_count;
} vi_dev_t;

/* Queue memory: two 8 KiB page-aligned blocks, one per device */
static u8 vi_mem[VI_MAX_DEVS][8192] __attribute__((aligned(4096)));
static virtio_input_event_t vi_bufs[VI_MAX_DEVS][VIRTQ_SIZE];

static vi_dev_t vi_devs[VI_MAX_DEVS];
static int      vi_ndevs;

/* ── MMIO register helpers ───────────────────────────────────────────────── */

static inline u32 mmio_r(volatile u32 *base, u32 off)
{
    return *(volatile u32 *)((uintptr_t)base + off);
}

static inline void mmio_w(volatile u32 *base, u32 off, u32 val)
{
    *(volatile u32 *)((uintptr_t)base + off) = val;
}

/* ── PCI BAR MMIO helpers (byte/u16/u32) ────────────────────────────────── */

static inline u8   vcfg_r8 (uintptr_t b, u32 o) { return *(volatile u8  *)(b + o); }
static inline u16  vcfg_r16(uintptr_t b, u32 o) { return *(volatile u16 *)(b + o); }
static inline u32  vcfg_r32(uintptr_t b, u32 o) { return *(volatile u32 *)(b + o); }
static inline void vcfg_w8 (uintptr_t b, u32 o, u8  v) { *(volatile u8  *)(b + o) = v; }
static inline void vcfg_w16(uintptr_t b, u32 o, u16 v) { *(volatile u16 *)(b + o) = v; }
static inline void vcfg_w32(uintptr_t b, u32 o, u32 v) { *(volatile u32 *)(b + o) = v; }

/* ── Linux evdev → AetherOS keycode table ───────────────────────────────── */

static const u8 linux_to_aether[256] = {
    [ 1] = KEY_ESC,        [ 2] = KEY_1,          [ 3] = KEY_2,
    [ 4] = KEY_3,          [ 5] = KEY_4,           [ 6] = KEY_5,
    [ 7] = KEY_6,          [ 8] = KEY_7,           [ 9] = KEY_8,
    [10] = KEY_9,          [11] = KEY_0,           [12] = KEY_MINUS,
    [13] = KEY_EQUALS,     [14] = KEY_BACKSPACE,   [15] = KEY_TAB,
    [16] = KEY_Q,          [17] = KEY_W,           [18] = KEY_E,
    [19] = KEY_R,          [20] = KEY_T,           [21] = KEY_Y,
    [22] = KEY_U,          [23] = KEY_I,           [24] = KEY_O,
    [25] = KEY_P,          [26] = KEY_LBRACKET,    [27] = KEY_RBRACKET,
    [28] = KEY_ENTER,      [29] = KEY_LCTRL,
    [30] = KEY_A,          [31] = KEY_S,           [32] = KEY_D,
    [33] = KEY_F,          [34] = KEY_G,           [35] = KEY_H,
    [36] = KEY_J,          [37] = KEY_K,           [38] = KEY_L,
    [39] = KEY_SEMICOLON,  [40] = KEY_APOSTROPHE,  [41] = KEY_GRAVE,
    [42] = KEY_LSHIFT,     [43] = KEY_BACKSLASH,
    [44] = KEY_Z,          [45] = KEY_X,           [46] = KEY_C,
    [47] = KEY_V,          [48] = KEY_B,           [49] = KEY_N,
    [50] = KEY_M,          [51] = KEY_COMMA,       [52] = KEY_DOT,
    [53] = KEY_SLASH,      [54] = KEY_RSHIFT,
    [56] = KEY_LALT,       [57] = KEY_SPACE,       [58] = KEY_CAPS_LOCK,
    [59] = KEY_F1,         [60] = KEY_F2,          [61] = KEY_F3,
    [62] = KEY_F4,         [63] = KEY_F5,          [64] = KEY_F6,
    [65] = KEY_F7,         [66] = KEY_F8,          [67] = KEY_F9,
    [68] = KEY_F10,        [87] = KEY_F11,         [88] = KEY_F12,
    [97] = KEY_RCTRL,      [100]= KEY_RALT,
    [102]= KEY_HOME,       [103]= KEY_UP,          [104]= KEY_PGUP,
    [105]= KEY_LEFT,       [106]= KEY_RIGHT,       [107]= KEY_END,
    [108]= KEY_DOWN,       [109]= KEY_PGDN,
    [110]= KEY_INSERT,     [111]= KEY_DELETE,
};

/* ── Drain pending events from one device ───────────────────────────────── */

static void service_device(vi_dev_t *dev)
{
    while (dev->last_used != dev->used->idx) {
        u16 slot    = (u16)(dev->last_used % VIRTQ_SIZE);
        u32 desc_id = dev->used->ring[slot].id;
        dev->last_used++;

        virtio_input_event_t *ev = &dev->bufs[desc_id];

        if (dev->dbg_count < 32) {
            kinfo("vi ev type=%u code=%u val=%lu\n",
                  ev->type, ev->code, (unsigned long)ev->value);
            dev->dbg_count++;
        }

        if (ev->type == EV_REL) {
            if (ev->code == REL_X) {
                int nx = (int)dev->cur_x + (int)(s32)ev->value;
                if (nx < 0) nx = 0;
                if (nx >= (int)SCREEN_W) nx = (int)SCREEN_W - 1;
                dev->cur_x = (u32)nx;
                dev->has_pos = 1;
            } else if (ev->code == REL_Y) {
                int ny = (int)dev->cur_y + (int)(s32)ev->value;
                if (ny < 0) ny = 0;
                if (ny >= (int)SCREEN_H) ny = (int)SCREEN_H - 1;
                dev->cur_y = (u32)ny;
                dev->has_pos = 1;
            }
        } else if (ev->type == EV_ABS) {
            if (ev->code == ABS_X)
                dev->cur_x = (u32)ev->value * SCREEN_W / (TABLET_MAX + 1u);
            else if (ev->code == ABS_Y)
                dev->cur_y = (u32)ev->value * SCREEN_H / (TABLET_MAX + 1u);
            /* Post on every ABS event — EV_SYN may be absent */
            {
                mouse_event_t me;
                me.x       = dev->cur_x;
                me.y       = dev->cur_y;
                me.buttons = dev->cur_btns;
                mouse_post_event(mouse_event_pack(me));
            }
        } else if (ev->type == EV_KEY) {
            u16  code  = ev->code;
            int  press = (ev->value != 0u) ? 1 : 0;
            if (code == BTN_LEFT || code == BTN_RIGHT || code == BTN_MIDDLE) {
                /* mouse_event_t.buttons: bit0=left, bit1=middle, bit2=right */
                u8 bit = (code == BTN_LEFT)   ? 1u :
                         (code == BTN_MIDDLE) ? 2u : 4u;
                if (press) dev->cur_btns |=  bit;
                else       dev->cur_btns &= (u8)~bit;
            } else if (code < 256u && linux_to_aether[code] != KEY_NONE) {
                kbd_push_key((keycode_t)linux_to_aether[code], press);
            }
        } else if (ev->type == EV_SYN && ev->code == 0u) {
            if (dev->has_pos) {
                mouse_event_t me;
                me.x       = dev->cur_x;
                me.y       = dev->cur_y;
                me.buttons = dev->cur_btns;
                mouse_post_event(mouse_event_pack(me));
                dev->has_pos = 0;
            }
        }

        /* Hand descriptor back to the device */
        dev->avail->ring[dev->avail->idx % VIRTQ_SIZE] = (u16)desc_id;
        __asm__ volatile("dmb ish" ::: "memory");
        dev->avail->idx++;

        /* Kick the device so it refills the descriptor */
        if (dev->is_pci)
            *(volatile u16 *)dev->notify_addr = 0u;
        else
            mmio_w(dev->base, VMMIO_QUEUE_NOTIFY, 0u);
    }
}

/* ── Public poll entry (called from timer_irq_handler at 100 Hz) ─────────── */

void virtio_input_poll(void)
{
    static u32 tick = 0;
    tick++;

    if (tick == 5u) {
        mouse_event_t test_ev;
        test_ev.x = SCREEN_W / 2u;
        test_ev.y = SCREEN_H / 2u;
        test_ev.buttons = 0;
        mouse_post_event(mouse_event_pack(test_ev));
        kinfo("vi: software-path test — injected cursor→(%u,%u)\n",
              test_ev.x, test_ev.y);
    }

    u32 interval = (tick < 3000u) ? 50u : 500u;
    if (tick % interval == 0u) {
        kinfo("vi poll tick=%lu ndevs=%d\n",
              (unsigned long)tick, vi_ndevs);
        for (int i = 0; i < vi_ndevs; i++) {
            kinfo("  vi[%d] avail=%u used=%u last=%u\n", i,
                  (unsigned)vi_devs[i].avail->idx,
                  (unsigned)vi_devs[i].used->idx,
                  (unsigned)vi_devs[i].last_used);
        }
    }

    for (int i = 0; i < vi_ndevs; i++)
        service_device(&vi_devs[i]);
}

/* ── Unused IRQ stubs (kept for backward compat with exceptions.c) ────────── */

int  virtio_input_owns_irq(u32 irq) { (void)irq; return 0; }
void virtio_input_dispatch(u32 irq) { (void)irq; }
void virtio_input_irq_handler(void) { }
int  virtio_input_irq(void)         { return -1; }

/* ── MMIO v2 device init ─────────────────────────────────────────────────── */

static void init_device(int idx, volatile u32 *base)
{
    vi_dev_t *dev = &vi_devs[idx];
    u8       *mem =  vi_mem[idx];

    dev->base        = base;
    dev->notify_addr = 0;
    dev->is_pci      = 0;
    dev->desc        = (virtq_desc_t  *)(mem);
    dev->avail       = (virtq_avail_t *)(mem + VIRTQ_SIZE * sizeof(virtq_desc_t));
    dev->used        = (virtq_used_t  *)(mem + QUEUE_ALIGN);
    dev->bufs        = vi_bufs[idx];
    dev->last_used   = 0;
    dev->cur_x       = SCREEN_W / 2u;
    dev->cur_y       = SCREEN_H / 2u;
    dev->cur_btns    = 0;
    dev->has_pos     = 0;
    dev->dbg_count   = 0;

    mmio_w(base, VMMIO_STATUS, 0u);
    __asm__ volatile("dsb sy; isb" ::: "memory");

    mmio_w(base, VMMIO_STATUS, VS_ACKNOWLEDGE);
    mmio_w(base, VMMIO_STATUS, VS_ACKNOWLEDGE | VS_DRIVER);

    mmio_w(base, VMMIO_DRV_FEAT_SEL, 0u);
    mmio_w(base, VMMIO_DRV_FEAT,     0u);
    mmio_w(base, VMMIO_DRV_FEAT_SEL, 1u);
    mmio_w(base, VMMIO_DRV_FEAT,     1u);

    mmio_w(base, VMMIO_STATUS, VS_ACKNOWLEDGE | VS_DRIVER | VS_FEATURES_OK);
    __asm__ volatile("dsb sy; isb" ::: "memory");

    u32 status = mmio_r(base, VMMIO_STATUS);
    if (!(status & VS_FEATURES_OK)) {
        kwarn("virtio-input[%d]: FEATURES_OK refused\n", idx);
        return;
    }

    mmio_w(base, VMMIO_QUEUE_SEL, 0u);

    u32 qmax = mmio_r(base, VMMIO_QUEUE_NUM_MAX);
    if (qmax == 0u) { kwarn("virtio-input[%d]: queue size 0\n", idx); return; }
    u32 qsz = (qmax < VIRTQ_SIZE) ? qmax : VIRTQ_SIZE;
    mmio_w(base, VMMIO_QUEUE_NUM, qsz);

    uintptr_t pa_desc  = (uintptr_t)dev->desc;
    uintptr_t pa_avail = (uintptr_t)dev->avail;
    uintptr_t pa_used  = (uintptr_t)dev->used;

    mmio_w(base, VMMIO_Q_DESC_LO,  (u32)(pa_desc));
    mmio_w(base, VMMIO_Q_DESC_HI,  (u32)(pa_desc  >> 32));
    mmio_w(base, VMMIO_Q_AVAIL_LO, (u32)(pa_avail));
    mmio_w(base, VMMIO_Q_AVAIL_HI, (u32)(pa_avail >> 32));
    mmio_w(base, VMMIO_Q_USED_LO,  (u32)(pa_used));
    mmio_w(base, VMMIO_Q_USED_HI,  (u32)(pa_used  >> 32));

    for (u32 j = 0; j < qsz; j++) {
        dev->desc[j].addr  = (u64)(uintptr_t)&dev->bufs[j];
        dev->desc[j].len   = sizeof(virtio_input_event_t);
        dev->desc[j].flags = VIRTQ_DESC_F_WRITE;
        dev->desc[j].next  = 0;
    }

    dev->avail->flags = 1u;
    dev->avail->idx   = 0u;
    for (u32 j = 0; j < qsz; j++) {
        dev->avail->ring[j] = (u16)j;
        dev->avail->idx++;
    }

    __asm__ volatile("dmb ish" ::: "memory");
    mmio_w(base, VMMIO_QUEUE_READY, 1u);
    mmio_w(base, VMMIO_STATUS,
           VS_ACKNOWLEDGE | VS_DRIVER | VS_FEATURES_OK | VS_DRIVER_OK);
    __asm__ volatile("dmb ish" ::: "memory");
    mmio_w(base, VMMIO_QUEUE_NOTIFY, 0u);

    kinfo("virtio-input[%d]: MMIO v2 0x%lx  qsz=%u\n",
          idx, (unsigned long)(uintptr_t)base, (unsigned)qsz);
}

/* ── MMIO v1 (legacy) device init ───────────────────────────────────────── */

static void init_device_v1(int idx, volatile u32 *base)
{
    vi_dev_t *dev = &vi_devs[idx];
    u8       *mem =  vi_mem[idx];

    dev->base        = base;
    dev->notify_addr = 0;
    dev->is_pci      = 0;
    dev->desc        = (virtq_desc_t  *)(mem);
    dev->avail       = (virtq_avail_t *)(mem + VIRTQ_SIZE * sizeof(virtq_desc_t));
    dev->used        = (virtq_used_t  *)(mem + QUEUE_ALIGN);
    dev->bufs        = vi_bufs[idx];
    dev->last_used   = 0;
    dev->cur_x       = SCREEN_W / 2u;
    dev->cur_y       = SCREEN_H / 2u;
    dev->cur_btns    = 0;
    dev->has_pos     = 0;
    dev->dbg_count   = 0;

    mmio_w(base, VMMIO_STATUS, 0u);
    __asm__ volatile("dsb sy; isb" ::: "memory");

    mmio_w(base, VMMIO_STATUS, VS_ACKNOWLEDGE);
    mmio_w(base, VMMIO_STATUS, VS_ACKNOWLEDGE | VS_DRIVER);

    /* v1: accept all device features (32-bit only, no FEATURES_OK step) */
    mmio_w(base, VMMIO_DRV_FEAT, mmio_r(base, VMMIO_DEV_FEAT));

    mmio_w(base, VMMIO_GUEST_PAGE_SIZE, 4096u);
    mmio_w(base, VMMIO_QUEUE_SEL, 0u);

    u32 qmax = mmio_r(base, VMMIO_QUEUE_NUM_MAX);
    if (qmax == 0u) { kwarn("virtio-input v1[%d]: queue size 0\n", idx); return; }
    u32 qsz = (qmax < VIRTQ_SIZE) ? qmax : VIRTQ_SIZE;
    mmio_w(base, VMMIO_QUEUE_NUM,   qsz);
    mmio_w(base, VMMIO_QUEUE_ALIGN, QUEUE_ALIGN);

    for (u32 j = 0; j < qsz; j++) {
        dev->desc[j].addr  = (u64)(uintptr_t)&dev->bufs[j];
        dev->desc[j].len   = sizeof(virtio_input_event_t);
        dev->desc[j].flags = VIRTQ_DESC_F_WRITE;
        dev->desc[j].next  = 0;
    }

    dev->avail->flags = 1u;
    dev->avail->idx   = 0u;
    for (u32 j = 0; j < qsz; j++) {
        dev->avail->ring[j] = (u16)j;
        dev->avail->idx++;
    }

    __asm__ volatile("dmb ish" ::: "memory");
    uintptr_t pfn = (uintptr_t)mem / 4096u;
    mmio_w(base, VMMIO_QUEUE_PFN, (u32)pfn);
    mmio_w(base, VMMIO_STATUS, VS_ACKNOWLEDGE | VS_DRIVER | VS_DRIVER_OK);
    __asm__ volatile("dmb ish" ::: "memory");
    mmio_w(base, VMMIO_QUEUE_NOTIFY, 0u);

    kinfo("virtio-input v1[%d]: MMIO 0x%lx  qsz=%u  pfn=0x%lx\n",
          idx, (unsigned long)(uintptr_t)base, (unsigned)qsz,
          (unsigned long)pfn);
}

/* ── PCI capability walker ───────────────────────────────────────────────── */
/*
 * Walks the PCI vendor-specific (0x09) capability list looking for the
 * VirtIO capability with the given cfg_type.  If found, fills:
 *   *out_bar  — BAR index (index into pdev->bar[])
 *   *out_off  — byte offset within that BAR
 *   *out_extra — 4 bytes after the standard cap (notify_off_multiplier for
 *                NOTIFY_CFG; pass NULL if not needed)
 * Returns 1 if found, 0 if not.
 */
static int pci_find_virtio_cap(const pci_dev_t *pdev, u8 cfg_type,
                                u8 *out_bar, u32 *out_off, u32 *out_extra)
{
    u8  bus = pdev->bus, d = pdev->dev, fn = pdev->fn;
    u16 status = pci_read16(bus, d, fn, PCI_STATUS);
    if (!(status & PCI_STS_CAP_LIST)) return 0;

    u8 ptr = pci_read8(bus, d, fn, PCI_CAP_PTR) & 0xFCu;
    while (ptr != 0u) {
        u8 cap_id   = pci_read8(bus, d, fn, ptr + 0u);
        u8 cap_next = pci_read8(bus, d, fn, ptr + 1u);

        if (cap_id == PCI_CAP_VNDR) {
            u8  ct  = pci_read8 (bus, d, fn, ptr + 3u);
            u8  bar = pci_read8 (bus, d, fn, ptr + 4u);
            u32 off = pci_read32(bus, d, fn, ptr + 8u);
            if (ct == cfg_type) {
                *out_bar = bar;
                *out_off = off;
                if (out_extra)
                    *out_extra = pci_read32(bus, d, fn, ptr + 16u);
                return 1;
            }
        }
        ptr = cap_next & 0xFCu;
    }
    return 0;
}

/* ── PCI modern device init ──────────────────────────────────────────────── */
/*
 * VirtIO 1.x modern (non-transitional) PCI init sequence.
 * All device configuration is done through the COMMON_CFG BAR region.
 * Queue kick uses the address derived from NOTIFY_CFG + queue_notify_off.
 */
static void init_device_pci(int idx, const pci_dev_t *pdev)
{
    vi_dev_t *dev = &vi_devs[idx];
    u8       *mem =  vi_mem[idx];

    /* Locate COMMON_CFG */
    u8  cc_bar; u32 cc_off, cc_dummy;
    if (!pci_find_virtio_cap(pdev, VPCI_CAP_COMMON_CFG,
                             &cc_bar, &cc_off, &cc_dummy)) {
        kwarn("virtio-input PCI[%d]: no COMMON_CFG cap\n", idx);
        return;
    }
    if (pdev->bar[cc_bar] == 0u) {
        kwarn("virtio-input PCI[%d]: COMMON_CFG BAR %u not assigned\n",
              idx, (unsigned)cc_bar);
        return;
    }
    uintptr_t cc = pdev->bar[cc_bar] + cc_off;

    /* Locate NOTIFY_CFG (notify_off_multiplier is 4 bytes past cap end) */
    u8  nc_bar; u32 nc_off, nc_mul;
    if (!pci_find_virtio_cap(pdev, VPCI_CAP_NOTIFY_CFG,
                             &nc_bar, &nc_off, &nc_mul)) {
        kwarn("virtio-input PCI[%d]: no NOTIFY_CFG cap\n", idx);
        return;
    }
    if (pdev->bar[nc_bar] == 0u) {
        kwarn("virtio-input PCI[%d]: NOTIFY_CFG BAR %u not assigned\n",
              idx, (unsigned)nc_bar);
        return;
    }
    uintptr_t nc_base = pdev->bar[nc_bar] + nc_off;

    dev->base        = (volatile u32 *)0;
    dev->notify_addr = 0;
    dev->is_pci      = 1;
    dev->desc        = (virtq_desc_t  *)(mem);
    dev->avail       = (virtq_avail_t *)(mem + VIRTQ_SIZE * sizeof(virtq_desc_t));
    dev->used        = (virtq_used_t  *)(mem + QUEUE_ALIGN);
    dev->bufs        = vi_bufs[idx];
    dev->last_used   = 0;
    dev->cur_x       = SCREEN_W / 2u;
    dev->cur_y       = SCREEN_H / 2u;
    dev->cur_btns    = 0;
    dev->has_pos     = 0;
    dev->dbg_count   = 0;

    /* 1. Reset */
    vcfg_w8(cc, VCC_DEV_STATUS, 0u);
    __asm__ volatile("dsb sy; isb" ::: "memory");

    /* 2–3. Acknowledge + Driver */
    vcfg_w8(cc, VCC_DEV_STATUS, VS_ACKNOWLEDGE);
    vcfg_w8(cc, VCC_DEV_STATUS, VS_ACKNOWLEDGE | VS_DRIVER);

    /* 4. Feature negotiation: VIRTIO_F_VERSION_1 (bit 32 = page-1 bit-0) */
    vcfg_w32(cc, VCC_DRV_FEAT_SEL, 0u);
    vcfg_w32(cc, VCC_DRV_FEAT,     0u);
    vcfg_w32(cc, VCC_DRV_FEAT_SEL, 1u);
    vcfg_w32(cc, VCC_DRV_FEAT,     1u);

    /* 5. FEATURES_OK */
    vcfg_w8(cc, VCC_DEV_STATUS,
            VS_ACKNOWLEDGE | VS_DRIVER | VS_FEATURES_OK);
    __asm__ volatile("dsb sy; isb" ::: "memory");

    u8 dstatus = vcfg_r8(cc, VCC_DEV_STATUS);
    if (!(dstatus & VS_FEATURES_OK)) {
        kwarn("virtio-input PCI[%d]: FEATURES_OK refused (status=0x%x)\n",
              idx, (unsigned)dstatus);
        return;
    }

    /* 6. Queue 0 (eventq) setup */
    vcfg_w16(cc, VCC_QUEUE_SEL, 0u);

    u16 qmax = vcfg_r16(cc, VCC_QUEUE_SIZE);
    if (qmax == 0u) {
        kwarn("virtio-input PCI[%d]: queue size 0\n", idx);
        return;
    }
    u16 qsz = (qmax < (u16)VIRTQ_SIZE) ? qmax : (u16)VIRTQ_SIZE;
    vcfg_w16(cc, VCC_QUEUE_SIZE, qsz);

    uintptr_t pa_desc  = (uintptr_t)dev->desc;
    uintptr_t pa_avail = (uintptr_t)dev->avail;
    uintptr_t pa_used  = (uintptr_t)dev->used;

    vcfg_w32(cc, VCC_QUEUE_DESC_LO, (u32)(pa_desc));
    vcfg_w32(cc, VCC_QUEUE_DESC_HI, (u32)(pa_desc  >> 32));
    vcfg_w32(cc, VCC_QUEUE_DRV_LO,  (u32)(pa_avail));
    vcfg_w32(cc, VCC_QUEUE_DRV_HI,  (u32)(pa_avail >> 32));
    vcfg_w32(cc, VCC_QUEUE_DEV_LO,  (u32)(pa_used));
    vcfg_w32(cc, VCC_QUEUE_DEV_HI,  (u32)(pa_used  >> 32));

    for (u32 j = 0; j < (u32)qsz; j++) {
        dev->desc[j].addr  = (u64)(uintptr_t)&dev->bufs[j];
        dev->desc[j].len   = sizeof(virtio_input_event_t);
        dev->desc[j].flags = VIRTQ_DESC_F_WRITE;
        dev->desc[j].next  = 0;
    }

    dev->avail->flags = 1u;
    dev->avail->idx   = 0u;
    for (u32 j = 0; j < (u32)qsz; j++) {
        dev->avail->ring[j] = (u16)j;
        dev->avail->idx++;
    }

    __asm__ volatile("dmb ish" ::: "memory");

    /* 7. Enable queue */
    vcfg_w16(cc, VCC_QUEUE_ENABLE, 1u);

    /* 8. Compute kick address from queue_notify_off × notify_off_multiplier */
    u16 ntfy_off = vcfg_r16(cc, VCC_QUEUE_NTFY_OFF);
    dev->notify_addr = nc_base + (uintptr_t)ntfy_off * (uintptr_t)nc_mul;

    /* 9. Driver OK */
    vcfg_w8(cc, VCC_DEV_STATUS,
            VS_ACKNOWLEDGE | VS_DRIVER | VS_FEATURES_OK | VS_DRIVER_OK);
    __asm__ volatile("dmb ish" ::: "memory");

    /* 10. Initial kick */
    *(volatile u16 *)dev->notify_addr = 0u;

    kinfo("virtio-input PCI[%d]: %x:%x.0  qsz=%u  kick=0x%lx\n",
          idx, (unsigned)pdev->bus, (unsigned)pdev->dev,
          (unsigned)qsz, (unsigned long)dev->notify_addr);
}

/* ── Probe and initialise all input devices ──────────────────────────────── */

void virtio_input_init(void)
{
    vi_ndevs = 0;

    /* ── Try PCI first (virtio-tablet-pci / virtio-keyboard-pci) ──────── */
    pci_dev_t pci_devs[VI_MAX_DEVS];
    int npci = pci_scan_virtio_input(pci_devs, VI_MAX_DEVS);

    if (npci > 0) {
        kinfo("virtio-input: found %d PCI device(s)\n", npci);
        for (int i = 0; i < npci && vi_ndevs < VI_MAX_DEVS; i++) {
            init_device_pci(vi_ndevs, &pci_devs[i]);
            vi_ndevs++;
        }
        kinfo("virtio-input: %d PCI device(s) ready (polling at 100 Hz)\n",
              vi_ndevs);
        return;
    }

    /* ── Fall back to MMIO scan ────────────────────────────────────────── */
    kinfo("virtio-input: no PCI devices — scanning MMIO bus\n"
          "  (base=0x%lx slot_sz=0x%lx)\n",
          (unsigned long)VMMIO_BASE, (unsigned long)VMMIO_SLOT_SIZE);

    for (int slot = 0; slot < 4; slot++) {
        volatile u32 *b =
            (volatile u32 *)(VMMIO_BASE + (uintptr_t)slot * VMMIO_SLOT_SIZE);
        kinfo("  slot[%d] magic=0x%lx ver=%lu devid=%lu\n",
              slot,
              (unsigned long)mmio_r(b, VMMIO_MAGIC),
              (unsigned long)mmio_r(b, VMMIO_VERSION),
              (unsigned long)mmio_r(b, VMMIO_DEVICE_ID));
    }

    for (int slot = 0; slot < VMMIO_MAX_SLOTS && vi_ndevs < VI_MAX_DEVS; slot++) {
        volatile u32 *base =
            (volatile u32 *)(VMMIO_BASE + (uintptr_t)slot * VMMIO_SLOT_SIZE);

        u32 magic   = mmio_r(base, VMMIO_MAGIC);
        u32 version = mmio_r(base, VMMIO_VERSION);
        u32 dev_id  = mmio_r(base, VMMIO_DEVICE_ID);

        if (magic   != (u32)VIRTIO_MAGIC)    continue;
        if (version != 1u && version != (u32)VIRTIO_VERSION2) continue;
        if (dev_id  != (u32)VIRTIO_ID_INPUT) continue;

        kinfo("virtio-input: slot %d at 0x%lx (v%u id=%u)\n",
              slot, (unsigned long)(uintptr_t)base,
              (unsigned)version, (unsigned)dev_id);

        if (version == 1u)
            init_device_v1(vi_ndevs, base);
        else
            init_device(vi_ndevs, base);
        vi_ndevs++;
    }

    if (vi_ndevs == 0) {
        kinfo("virtio-input: no input devices found\n"
              "  PCI: add -device virtio-tablet-pci -device virtio-keyboard-pci\n"
              "  MMIO: add -device virtio-tablet-device,bus=virtio-mmio-bus.0\n");
        return;
    }

    kinfo("virtio-input: %d MMIO device(s) ready (polling at 100 Hz)\n",
          vi_ndevs);
}
