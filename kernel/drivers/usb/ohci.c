/*
 * AetherOS — USB OHCI Host Controller + USB HID Driver
 * File: kernel/drivers/usb/ohci.c
 *
 * OHCI spec 1.0a; USB HID spec 1.11.
 *
 * Supports: usb-kbd (8-byte boot-protocol reports)
 *           usb-tablet (7-byte QEMU absolute-position reports)
 * Strategy: poll interrupt TDs at 100 Hz from timer_irq_handler().
 *           No IRQ wiring needed — we check TD Condition Code in BSS.
 */

#include "drivers/usb/ohci.h"
#include "drivers/pci/pci_ecam.h"
#include "drivers/input/pl050_kbd.h"
#include "drivers/input/pl050_mouse.h"
#include "drivers/input/keycodes.h"
#include "aether/printk.h"
#include "aether/types.h"

/* ── Static OHCI data structures (kernel BSS, identity-mapped) ──────────── */

static hcca_t     g_hcca              __attribute__((aligned(256)));
static ohci_ed_t  g_ctrl_ed           __attribute__((aligned(16)));
static ohci_td_t  g_ctrl_td[4]        __attribute__((aligned(16)));
static u8         g_ctrl_buf[256]     __attribute__((aligned(4)));

#define MAX_HID  2
static ohci_ed_t  g_int_ed[MAX_HID]        __attribute__((aligned(16)));
static ohci_td_t  g_int_td[MAX_HID]        __attribute__((aligned(16)));
static ohci_td_t  g_int_td_dummy[MAX_HID]  __attribute__((aligned(16)));
static u8         g_int_buf[MAX_HID][8]    __attribute__((aligned(4)));

/* Kernel BSS is identity-mapped: virtual addr == physical addr */
#define PHYS(ptr) ((u32)(uintptr_t)(ptr))

/* Freestanding memset/memcpy (no libc in kernel) */
static void km_set(void *dst, u8 val, u32 n)
{
    u8 *p = (u8 *)dst;
    while (n--) *p++ = val;
}

static void km_copy(void *dst, const void *src, u32 n)
{
    u8 *d = (u8 *)dst;
    const u8 *s = (const u8 *)src;
    while (n--) *d++ = *s++;
}

/* ── Per-device state ───────────────────────────────────────────────────── */

typedef struct {
    int  active;
    int  is_kbd;
    u8   addr;
    u8   ep_num;
    u16  max_packet;
    int  is_ls;
    u8   prev_kbd[8];
} usb_hid_dev_t;

static usb_hid_dev_t g_hid[MAX_HID];
static int           g_hid_count;

/* ── OHCI MMIO ──────────────────────────────────────────────────────────── */

static uintptr_t g_ohci_base;

static inline u32 ohci_rd(u32 reg)
{
    return *(volatile u32 *)(g_ohci_base + reg);
}

static inline void ohci_wr(u32 reg, u32 val)
{
    *(volatile u32 *)(g_ohci_base + reg) = val;
    __asm__ volatile("dsb sy" ::: "memory");
}

/* ── ARM counter timing ─────────────────────────────────────────────────── */

static inline u64 arm_cntpct(void)
{
    u64 v; __asm__ volatile("mrs %0, CNTPCT_EL0" : "=r"(v)); return v;
}

static inline u64 arm_cntfrq(void)
{
    u64 v; __asm__ volatile("mrs %0, CNTFRQ_EL0" : "=r"(v)); return v;
}

static void mdelay(u32 ms)
{
    u64 freq = arm_cntfrq();
    if (!freq) freq = 62500000ULL;
    u64 start = arm_cntpct();
    u64 ticks = (freq / 1000ULL) * ms;
    while ((arm_cntpct() - start) < ticks)
        __asm__ volatile("nop");
}

/* Memory barrier shorthand */
#define DSB() __asm__ volatile("dsb sy" ::: "memory")

/* Volatile read of TD flags (prevents compiler from caching across DSB) */
static inline u32 td_cc(const ohci_td_t *td)
{
    u32 f = *(const volatile u32 *)&td->flags;
    return (f >> 28) & 0xFu;
}

/* ── Synchronous control transfer ───────────────────────────────────────── */
/*
 * ctrl_xfer — issue one USB control transfer and busy-wait for completion.
 *
 * setup: 8-byte SETUP packet (direction derived from bmRequestType bit 7)
 * data:  buffer for DATA phase; NULL if setup->wLength == 0
 *
 * Returns 0 on success, -1 on error/timeout.
 */
static int ctrl_xfer(u8 addr, u8 ep0_mps, int is_ls,
                     const usb_setup_t *setup, void *data)
{
    int data_is_in = (setup->bmRequestType & 0x80u) ? 1 : 0;
    u16 data_len   = setup->wLength;

    /* ---- Build structures ---- */
    km_set(&g_ctrl_ed,  0, sizeof(g_ctrl_ed));
    km_set(g_ctrl_td,   0, sizeof(g_ctrl_td));

    g_ctrl_ed.flags = ED_FA(addr) | ED_EN(0) | ED_DIR_TD
                    | (is_ls ? ED_SPEED_LS : ED_SPEED_FS)
                    | ED_MPS(ep0_mps);

    /* Dummy tail sentinel (TD[3]) */
    g_ctrl_td[3].flags = TD_CC_NOTACCESSED;

    /* TD[0]: SETUP (always DATA0) */
    km_copy(g_ctrl_buf, setup, 8);
    g_ctrl_td[0].flags   = (u32)(TD_CC_NOTACCESSED | TD_T_DATA0
                                | TD_DP_SETUP | TD_DI_NONE);
    g_ctrl_td[0].cbp     = PHYS(g_ctrl_buf);
    g_ctrl_td[0].be      = PHYS(g_ctrl_buf) + 7u;
    g_ctrl_td[0].next_td = data_len ? PHYS(&g_ctrl_td[1]) : PHYS(&g_ctrl_td[2]);

    /* TD[1]: DATA phase (optional) */
    if (data_len) {
        if (!data_is_in && data)
            km_copy(g_ctrl_buf + 8, data, data_len);

        g_ctrl_td[1].flags   = (u32)(TD_CC_NOTACCESSED | TD_T_DATA1
                                    | (data_is_in ? TD_DP_IN : TD_DP_OUT)
                                    | TD_R | TD_DI_NONE);
        g_ctrl_td[1].cbp     = PHYS(g_ctrl_buf + 8);
        g_ctrl_td[1].be      = PHYS(g_ctrl_buf + 8) + (u32)(data_len - 1u);
        g_ctrl_td[1].next_td = PHYS(&g_ctrl_td[2]);
    }

    /* TD[2]: STATUS (opposite direction, zero-length, DATA1) */
    g_ctrl_td[2].flags   = (u32)(TD_CC_NOTACCESSED | TD_T_DATA1
                                | (data_is_in ? TD_DP_OUT : TD_DP_IN)
                                | TD_DI_NONE);
    g_ctrl_td[2].cbp     = 0u;
    g_ctrl_td[2].be      = 0u;
    g_ctrl_td[2].next_td = PHYS(&g_ctrl_td[3]);

    g_ctrl_ed.tail_td = PHYS(&g_ctrl_td[3]);
    g_ctrl_ed.head_td = PHYS(&g_ctrl_td[0]);
    g_ctrl_ed.next_ed = 0u;

    DSB();

    /* ---- Submit to HC ---- */
    ohci_wr(OHCI_HcControlHeadED,    PHYS(&g_ctrl_ed));
    ohci_wr(OHCI_HcControlCurrentED, 0u);
    ohci_wr(OHCI_HcControl, (ohci_rd(OHCI_HcControl) | OHCI_CTRL_CLE));
    ohci_wr(OHCI_HcCommandStatus, OHCI_CS_CLF);

    /* ---- Wait for each TD ---- */
    u64 freq = arm_cntfrq();
    if (!freq) freq = 62500000ULL;
    u64 timeout_ticks = (freq / 1000ULL) * 500ULL;  /* 500 ms per TD */

#define WAIT_TD(td_idx) \
    do { \
        u64 _s = arm_cntpct(); \
        while (td_cc(&g_ctrl_td[(td_idx)]) == 0xFu) { \
            DSB(); \
            if ((arm_cntpct() - _s) >= timeout_ticks) { \
                kwarn("USB: ctrl TD[%d] timeout\n", (td_idx)); \
                goto xfer_err; \
            } \
        } \
        if (td_cc(&g_ctrl_td[(td_idx)]) != TD_CC_OK) { \
            kwarn("USB: ctrl TD[%d] CC=%u\n", \
                  (td_idx), td_cc(&g_ctrl_td[(td_idx)])); \
            goto xfer_err; \
        } \
    } while (0)

    WAIT_TD(0);
    if (data_len) WAIT_TD(1);
    WAIT_TD(2);

#undef WAIT_TD

    /* Copy received data to caller's buffer */
    if (data_len && data_is_in && data)
        km_copy(data, g_ctrl_buf + 8, data_len);

    ohci_wr(OHCI_HcControl,
            ohci_rd(OHCI_HcControl) & ~OHCI_CTRL_CLE);
    return 0;

xfer_err:
    /* Halt the control ED to stop HC from retrying */
    g_ctrl_ed.head_td |= ED_HEAD_HALTED;
    DSB();
    ohci_wr(OHCI_HcControl,
            ohci_rd(OHCI_HcControl) & ~OHCI_CTRL_CLE);
    return -1;
}

/* ── USB descriptor helpers ─────────────────────────────────────────────── */

static usb_setup_t make_get_desc(u8 type, u8 idx, u16 len)
{
    usb_setup_t s = {
        .bmRequestType = USB_RT_DEV_TO_HOST,
        .bRequest      = USB_REQ_GET_DESCRIPTOR,
        .wValue        = (u16)((u16)type << 8 | idx),
        .wIndex        = 0,
        .wLength       = len,
    };
    return s;
}

static usb_setup_t make_set_addr(u8 addr)
{
    usb_setup_t s = {
        .bmRequestType = USB_RT_HOST_TO_DEV,
        .bRequest      = USB_REQ_SET_ADDRESS,
        .wValue        = addr,
        .wIndex        = 0,
        .wLength       = 0,
    };
    return s;
}

static usb_setup_t make_set_cfg(u8 cfg)
{
    usb_setup_t s = {
        .bmRequestType = USB_RT_HOST_TO_DEV,
        .bRequest      = USB_REQ_SET_CONFIGURATION,
        .wValue        = cfg,
        .wIndex        = 0,
        .wLength       = 0,
    };
    return s;
}

static usb_setup_t make_set_protocol(u8 iface, u16 protocol)
{
    usb_setup_t s = {
        .bmRequestType = USB_RT_CLASS_IF,
        .bRequest      = USB_REQ_SET_PROTOCOL,
        .wValue        = protocol,
        .wIndex        = iface,
        .wLength       = 0,
    };
    return s;
}

static usb_setup_t make_set_idle(u8 iface)
{
    usb_setup_t s = {
        .bmRequestType = USB_RT_CLASS_IF,
        .bRequest      = USB_REQ_SET_IDLE,
        .wValue        = 0,   /* duration=0 (indefinite), report_id=0 (all) */
        .wIndex        = iface,
        .wLength       = 0,
    };
    return s;
}

/* ── Config descriptor parser ───────────────────────────────────────────── */

typedef struct {
    u8  cfg_value;
    u8  iface_num;
    u8  iface_sub;
    u8  iface_proto;
    u8  ep_addr;
    u16 ep_mps;
    int found;
} hid_info_t;

static int parse_config_blob(const u8 *blob, u16 total_len, hid_info_t *out)
{
    const u8 *p   = blob;
    const u8 *end = p + total_len;
    int in_hid    = 0;

    km_set(out, 0, sizeof(*out));

    if (p >= end || p[1] != USB_DT_CONFIG) return -1;
    out->cfg_value = p[5];
    p += p[0];

    while (p < end && p + 2u <= end) {
        u8 dlen  = p[0];
        u8 dtype = p[1];
        if (dlen < 2u) break;

        if (dtype == USB_DT_INTERFACE) {
            in_hid = (p[5] == 3u);   /* bInterfaceClass == HID */
            if (in_hid) {
                out->iface_num   = p[2];
                out->iface_sub   = p[6];
                out->iface_proto = p[7];
            }
        } else if (dtype == USB_DT_ENDPOINT && in_hid && !out->found) {
            u8 ep_addr = p[2];
            u8 attr    = p[3];
            if ((attr & 0x03u) == 3u && (ep_addr & 0x80u)) {
                out->ep_addr = ep_addr;
                out->ep_mps  = (u16)(p[4] | ((u16)p[5] << 8));
                out->found   = 1;
            }
        }
        p += dlen;
    }
    return out->found ? 0 : -1;
}

/* ── USB device enumeration ─────────────────────────────────────────────── */

static void enumerate_port(u32 port, u8 new_addr, int is_ls)
{
    usb_setup_t s;
    int rc;

    kinfo("USB: enumerating port %u (ls=%d) → addr %u\n",
          (unsigned)port, is_ls, (unsigned)new_addr);

    /* 1. GET_DESCRIPTOR(Device, 18) at addr 0, MPS=8 */
    u8 dev_desc[18];
    s  = make_get_desc(USB_DT_DEVICE, 0, 18);
    rc = ctrl_xfer(0, 8, is_ls, &s, dev_desc);
    if (rc < 0) { kwarn("USB: port %u GET_DESCRIPTOR(Dev) failed\n", port); return; }

    u8 ep0_mps = dev_desc[7];
    if (!ep0_mps) ep0_mps = 8;

    kinfo("USB: dev VID=0x%x PID=0x%x mps=%u\n",
          (unsigned)(dev_desc[8] | ((u16)dev_desc[9] << 8)),
          (unsigned)(dev_desc[10] | ((u16)dev_desc[11] << 8)),
          (unsigned)ep0_mps);

    /* 2. SET_ADDRESS */
    s  = make_set_addr(new_addr);
    rc = ctrl_xfer(0, ep0_mps, is_ls, &s, NULL);
    if (rc < 0) { kwarn("USB: port %u SET_ADDRESS failed\n", port); return; }
    mdelay(5);  /* device needs time to switch address */

    /* 3. GET_DESCRIPTOR(Config, 9) to learn wTotalLength */
    u8 cfg_hdr[9];
    s  = make_get_desc(USB_DT_CONFIG, 0, 9);
    rc = ctrl_xfer(new_addr, ep0_mps, is_ls, &s, cfg_hdr);
    if (rc < 0) { kwarn("USB: addr %u GET_DESCRIPTOR(Cfg/9) failed\n", new_addr); return; }

    u16 total_len = (u16)(cfg_hdr[2] | ((u16)cfg_hdr[3] << 8));
    if (total_len < 9u || total_len > 240u) total_len = 240u;

    /* 4. GET_DESCRIPTOR(Config, total_len) — full blob */
    u8 cfg_blob[240];
    s  = make_get_desc(USB_DT_CONFIG, 0, total_len);
    rc = ctrl_xfer(new_addr, ep0_mps, is_ls, &s, cfg_blob);
    if (rc < 0) { kwarn("USB: addr %u GET_DESCRIPTOR(Cfg/full) failed\n", new_addr); return; }

    /* 5. Parse blob for HID interface + interrupt IN endpoint */
    hid_info_t hid;
    if (parse_config_blob(cfg_blob, total_len, &hid) < 0) {
        kwarn("USB: addr %u no HID interface found\n", new_addr);
        return;
    }

    int is_kbd = (hid.iface_sub == 1 && hid.iface_proto == 1);

    kinfo("USB: addr %u iface=%u sub=%u proto=%u ep=0x%x mps=%u %s\n",
          (unsigned)new_addr, (unsigned)hid.iface_num,
          (unsigned)hid.iface_sub, (unsigned)hid.iface_proto,
          (unsigned)hid.ep_addr, (unsigned)hid.ep_mps,
          is_kbd ? "kbd" : "tablet");

    /* 6. SET_CONFIGURATION */
    s  = make_set_cfg(hid.cfg_value);
    rc = ctrl_xfer(new_addr, ep0_mps, is_ls, &s, NULL);
    if (rc < 0) { kwarn("USB: addr %u SET_CONFIGURATION failed\n", new_addr); return; }
    mdelay(2);

    /* 7. SET_PROTOCOL(0) = boot protocol (keyboard only) */
    if (is_kbd) {
        s  = make_set_protocol(hid.iface_num, 0);
        rc = ctrl_xfer(new_addr, ep0_mps, is_ls, &s, NULL);
        if (rc < 0) kwarn("USB: addr %u SET_PROTOCOL failed (non-fatal)\n", new_addr);
    }

    /* 8. SET_IDLE(0) = disable periodic idle reports */
    s  = make_set_idle(hid.iface_num);
    rc = ctrl_xfer(new_addr, ep0_mps, is_ls, &s, NULL);
    if (rc < 0) kwarn("USB: addr %u SET_IDLE failed (non-fatal)\n", new_addr);

    /* 9. Record device */
    if (g_hid_count >= MAX_HID) { kwarn("USB: too many HID devices\n"); return; }

    int idx = g_hid_count++;
    usb_hid_dev_t *dev = &g_hid[idx];
    dev->active     = 1;
    dev->is_kbd     = is_kbd;
    dev->addr       = new_addr;
    dev->ep_num     = hid.ep_addr & 0x0Fu;
    dev->max_packet = hid.ep_mps < 8u ? 8u :
                      hid.ep_mps > 8u ? 8u :  /* cap at 8 for our buffers */
                      hid.ep_mps;
    dev->is_ls      = is_ls;
    km_set(dev->prev_kbd, 0, 8);

    kinfo("USB: HID[%d] = %s  addr=%u ep%u mps=%u\n",
          idx, is_kbd ? "kbd" : "tablet",
          (unsigned)dev->addr, (unsigned)dev->ep_num,
          (unsigned)dev->max_packet);
}

/* ── Interrupt ED setup ─────────────────────────────────────────────────── */

static void setup_int_ed(int idx)
{
    usb_hid_dev_t *dev   = &g_hid[idx];
    ohci_ed_t     *ed    = &g_int_ed[idx];
    ohci_td_t     *td    = &g_int_td[idx];
    ohci_td_t     *dummy = &g_int_td_dummy[idx];
    u8            *buf   = g_int_buf[idx];

    km_set(dummy, 0, sizeof(*dummy));
    dummy->flags = (u32)TD_CC_NOTACCESSED;

    km_set(td, 0, sizeof(*td));
    td->flags   = (u32)(TD_CC_NOTACCESSED | TD_T_DATA0 | TD_DP_IN
                       | TD_R | TD_DI_NONE);
    td->cbp     = PHYS(buf);
    td->be      = PHYS(buf) + (u32)(dev->max_packet - 1u);
    td->next_td = PHYS(dummy);

    km_set(ed, 0, sizeof(*ed));
    ed->flags   = ED_FA(dev->addr) | ED_EN(dev->ep_num) | ED_DIR_IN
                | (dev->is_ls ? ED_SPEED_LS : ED_SPEED_FS)
                | ED_MPS(dev->max_packet);
    ed->tail_td = PHYS(dummy);
    ed->head_td = PHYS(td);
    ed->next_ed = 0u;

    DSB();
}

static void link_int_eds_into_hcca(void)
{
    /* Build a simple linear chain: ed[0] → ed[1] → NULL */
    for (int i = 0; i < g_hid_count - 1; i++)
        g_int_ed[i].next_ed = PHYS(&g_int_ed[i + 1]);
    if (g_hid_count > 0)
        g_int_ed[g_hid_count - 1].next_ed = 0u;

    u32 head = (g_hid_count > 0) ? PHYS(&g_int_ed[0]) : 0u;
    for (int slot = 0; slot < 32; slot++)
        g_hcca.interrupt_table[slot] = head;

    DSB();
}

/* ── HID report parsing ─────────────────────────────────────────────────── */

/* USB HID keyboard usage ID → AetherOS keycode (index = usage ID) */
static const keycode_t hid_to_aether[0x53] = {
    [0x04] = KEY_A,  [0x05] = KEY_B,  [0x06] = KEY_C,  [0x07] = KEY_D,
    [0x08] = KEY_E,  [0x09] = KEY_F,  [0x0A] = KEY_G,  [0x0B] = KEY_H,
    [0x0C] = KEY_I,  [0x0D] = KEY_J,  [0x0E] = KEY_K,  [0x0F] = KEY_L,
    [0x10] = KEY_M,  [0x11] = KEY_N,  [0x12] = KEY_O,  [0x13] = KEY_P,
    [0x14] = KEY_Q,  [0x15] = KEY_R,  [0x16] = KEY_S,  [0x17] = KEY_T,
    [0x18] = KEY_U,  [0x19] = KEY_V,  [0x1A] = KEY_W,  [0x1B] = KEY_X,
    [0x1C] = KEY_Y,  [0x1D] = KEY_Z,
    [0x1E] = KEY_1,  [0x1F] = KEY_2,  [0x20] = KEY_3,  [0x21] = KEY_4,
    [0x22] = KEY_5,  [0x23] = KEY_6,  [0x24] = KEY_7,  [0x25] = KEY_8,
    [0x26] = KEY_9,  [0x27] = KEY_0,
    [0x28] = KEY_ENTER, [0x29] = KEY_ESC, [0x2A] = KEY_BACKSPACE, [0x2B] = KEY_TAB,
    [0x2C] = KEY_SPACE,
    [0x2D] = KEY_MINUS,     [0x2E] = KEY_EQUALS,
    [0x2F] = KEY_LBRACKET,  [0x30] = KEY_RBRACKET, [0x31] = KEY_BACKSLASH,
    [0x33] = KEY_SEMICOLON, [0x34] = KEY_APOSTROPHE,
    [0x35] = KEY_GRAVE,     [0x36] = KEY_COMMA,    [0x37] = KEY_DOT,
    [0x38] = KEY_SLASH,     [0x39] = KEY_CAPS_LOCK,
    [0x3A] = KEY_F1,  [0x3B] = KEY_F2,  [0x3C] = KEY_F3,  [0x3D] = KEY_F4,
    [0x3E] = KEY_F5,  [0x3F] = KEY_F6,  [0x40] = KEY_F7,  [0x41] = KEY_F8,
    [0x42] = KEY_F9,  [0x43] = KEY_F10, [0x44] = KEY_F11, [0x45] = KEY_F12,
    [0x49] = KEY_INSERT, [0x4A] = KEY_HOME, [0x4B] = KEY_PGUP,
    [0x4C] = KEY_DELETE, [0x4D] = KEY_END,  [0x4E] = KEY_PGDN,
    [0x4F] = KEY_RIGHT,  [0x50] = KEY_LEFT, [0x51] = KEY_DOWN, [0x52] = KEY_UP,
};

/* Modifier byte bits → {keycode, MOD_* bit} */
static const struct { keycode_t kc; unsigned int mod; } mod_map[8] = {
    { KEY_LCTRL,  MOD_CTRL  },   /* bit 0: L_Ctrl  */
    { KEY_LSHIFT, MOD_SHIFT },   /* bit 1: L_Shift */
    { KEY_LALT,   MOD_ALT   },   /* bit 2: L_Alt   */
    { KEY_NONE,   0         },   /* bit 3: L_Meta  */
    { KEY_RCTRL,  MOD_CTRL  },   /* bit 4: R_Ctrl  */
    { KEY_RSHIFT, MOD_SHIFT },   /* bit 5: R_Shift */
    { KEY_RALT,   MOD_ALT   },   /* bit 6: R_Alt   */
    { KEY_NONE,   0         },   /* bit 7: R_Meta  */
};

static void process_kbd_report(usb_hid_dev_t *dev, const u8 *rep)
{
    /* Modifier changes */
    u8 old_mod = dev->prev_kbd[0];
    u8 new_mod = rep[0];
    u8 delta   = old_mod ^ new_mod;

    for (int bit = 0; bit < 8; bit++) {
        if (!(delta & (1u << bit))) continue;
        if (mod_map[bit].kc == KEY_NONE) continue;
        kbd_push_key(mod_map[bit].kc, (new_mod >> bit) & 1);
    }

    /* Key presses: in new but not in old */
    for (int n = 2; n < 8; n++) {
        u8 u = rep[n];
        if (!u) break;
        int found = 0;
        for (int o = 2; o < 8; o++) {
            if (dev->prev_kbd[o] == u) { found = 1; break; }
        }
        if (!found && u < 0x53u && hid_to_aether[u])
            kbd_push_key(hid_to_aether[u], 1);
    }

    /* Key releases: in old but not in new */
    for (int o = 2; o < 8; o++) {
        u8 u = dev->prev_kbd[o];
        if (!u) break;
        int found = 0;
        for (int n = 2; n < 8; n++) {
            if (rep[n] == u) { found = 1; break; }
        }
        if (!found && u < 0x53u && hid_to_aether[u])
            kbd_push_key(hid_to_aether[u], 0);
    }

    km_copy(dev->prev_kbd, rep, 8);
}

static void process_tablet_report(const u8 *rep)
{
    /*
     * QEMU usb-tablet 7-byte report:
     *   [0]   buttons: bit0=left, bit1=right, bit2=middle
     *   [1:2] X: u16 LE  0-32767
     *   [3:4] Y: u16 LE  0-32767
     *   [5:6] wheel (ignored)
     */
    u8  btn_raw = rep[0];
    u16 x_raw   = (u16)(rep[1] | ((u16)rep[2] << 8));
    u16 y_raw   = (u16)(rep[3] | ((u16)rep[4] << 8));

    /* Scale: 32768 → 1024 (>> 5) and 32768 → 768 (× 3 >> 7) */
    unsigned int sx = (unsigned int)x_raw >> 5;
    unsigned int sy = ((unsigned int)y_raw * 3u) >> 7;

    /* Remap buttons: USB bit1=right → aether bit2, USB bit2=middle → aether bit1 */
    unsigned int buttons = (btn_raw & 1u)
                         | (((btn_raw >> 2) & 1u) << 1)
                         | (((btn_raw >> 1) & 1u) << 2);

    mouse_event_t me = { .x = sx, .y = sy, .buttons = buttons };
    mouse_post_event(mouse_event_pack(me));
}

/* ── usb_hid_poll (called at 100 Hz from timer IRQ) ────────────────────── */

static volatile u32 g_poll_n;   /* how many times poll has been called */

void usb_hid_poll(void)
{
    u32 n = ++g_poll_n;

    /* Diagnostic: log HC state every ~5 s for the first 60 s.
     * Key fields:
     *   IS  = HcInterruptStatus; bit1 (WBD) set when any TD retires.
     *   DH  = HCCA.done_head; non-zero when a TD was retired.
     *   cc  = TD Condition Code; stays 0xF while device NAKs (no data).
     * If IS/DH are never non-zero after hovering over the QEMU window,
     * QEMU's Cocoa display is not routing events to the USB devices. */
    if (n <= 600 && (n % 100) == 0) {
        DSB();
        u32 ctl  = ohci_rd(OHCI_HcControl);
        u32 fmn  = ohci_rd(OHCI_HcFmNumber);
        u32 ints = ohci_rd(OHCI_HcInterruptStatus);
        u32 dh   = *(volatile u32 *)&g_hcca.done_head;
        kinfo("USB poll %u: HcCtl=0x%x FmN=%u IS=0x%x DH=0x%lx\n",
              (unsigned)n, (unsigned)ctl, (unsigned)fmn,
              (unsigned)ints, (unsigned long)dh);
        for (int i = 0; i < g_hid_count; i++) {
            kinfo("  HID[%d] cc=%u ed_head=0x%lx\n", i,
                  (unsigned)td_cc(&g_int_td[i]),
                  (unsigned long)*(volatile u32 *)&g_int_ed[i].head_td);
        }
    }

    /* One-time alert: first time WritebackDoneHead (IS bit1) is set.
     * Fires when the HC retires a TD to the done queue for the first time,
     * i.e., when a USB device ACKs an IN token (key pressed / mouse moved). */
    {
        static volatile u32 s_wbd_seen;
        if (!s_wbd_seen) {
            DSB();
            u32 ints = ohci_rd(OHCI_HcInterruptStatus);
            if (ints & (1u << 1)) {
                s_wbd_seen = 1;
                kinfo("USB: FIRST TD RETIRED — IS=0x%x DH=0x%lx\n",
                      (unsigned)ints,
                      (unsigned long)*(volatile u32 *)&g_hcca.done_head);
                ohci_wr(OHCI_HcInterruptStatus, (1u << 1));  /* W1C clear */
            }
        }
    }

    for (int i = 0; i < g_hid_count; i++) {
        usb_hid_dev_t *dev   = &g_hid[i];
        if (!dev->active) continue;

        ohci_td_t *td    = &g_int_td[i];
        ohci_td_t *dummy = &g_int_td_dummy[i];
        ohci_ed_t *ed    = &g_int_ed[i];

        DSB();
        u32 cc = td_cc(td);
        if (cc == 0xFu) continue;   /* still pending */

        if (cc == TD_CC_OK) {
            static u32 s_first_ok;
            if (!s_first_ok++) {
                kinfo("USB: first TD completion HID[%d] cc=%u buf=0x%x%x%x%x%x%x%x%x\n",
                      i, (unsigned)cc,
                      g_int_buf[i][0], g_int_buf[i][1], g_int_buf[i][2], g_int_buf[i][3],
                      g_int_buf[i][4], g_int_buf[i][5], g_int_buf[i][6], g_int_buf[i][7]);
            }
            if (dev->is_kbd)
                process_kbd_report(dev, g_int_buf[i]);
            else
                process_tablet_report(g_int_buf[i]);
        }
        /* (errors: drop report, rearm anyway to keep device polling) */

        /* Rearm: reset TD, re-insert before dummy */
        td->flags   = (u32)(TD_CC_NOTACCESSED | TD_T_TOGGLE | TD_DP_IN
                           | TD_R | TD_DI_NONE);
        td->cbp     = PHYS(g_int_buf[i]);
        td->be      = PHYS(g_int_buf[i]) + (u32)(dev->max_packet - 1u);
        td->next_td = PHYS(dummy);

        dummy->flags   = (u32)TD_CC_NOTACCESSED;
        dummy->cbp     = 0u;
        dummy->next_td = 0u;
        dummy->be      = 0u;

        DSB();

        /* Preserve ToggleCarry (bit 1 of head_td) when re-inserting */
        u32 old_head = *(volatile u32 *)&ed->head_td;
        ed->head_td  = PHYS(td) | (old_head & 2u);
        DSB();
    }
}

/* ── usb_hid_init ────────────────────────────────────────────────────────── */

void usb_hid_init(void)
{
    g_hid_count = 0;
    km_set(g_hid,  0, sizeof(g_hid));
    km_set(&g_hcca, 0, sizeof(g_hcca));

    /* 1. Find OHCI controller on PCI bus */
    pci_dev_t pci;
    km_set(&pci, 0, sizeof(pci));
    if (!pci_scan_ohci(&pci)) {
        kwarn("USB: no OHCI controller found\n");
        return;
    }
    g_ohci_base = pci.bar[0];
    if (!g_ohci_base) {
        kwarn("USB: OHCI BAR0 not assigned\n");
        return;
    }

    kinfo("USB: OHCI rev=0x%x base=0x%lx\n",
          (unsigned)(ohci_rd(OHCI_HcRevision) & 0xFF),
          (unsigned long)g_ohci_base);

    /* 2. Disable all HC interrupts (we poll) */
    ohci_wr(OHCI_HcInterruptDisable, 0xFFFFFFFFu);
    ohci_wr(OHCI_HcInterruptStatus,  0xFFFFFFFFu);

    /* 3. Software reset — HcCommandStatus.HCR */
    ohci_wr(OHCI_HcCommandStatus, OHCI_CS_HCR);
    mdelay(15);   /* spec: max 10 ms; give 15 ms margin */

    /* 4. Set up HC registers */
    ohci_wr(OHCI_HcHCCA,          PHYS(&g_hcca));
    ohci_wr(OHCI_HcFmInterval,    OHCI_FMINTERVAL_DEFAULT);
    ohci_wr(OHCI_HcPeriodicStart, 0x2A2Fu);   /* 90% of FI=11999 */

    /* 5. Clear done head and control/bulk list pointers */
    ohci_wr(OHCI_HcControlHeadED,    0u);
    ohci_wr(OHCI_HcControlCurrentED, 0u);
    ohci_wr(OHCI_HcBulkHeadED,       0u);
    ohci_wr(OHCI_HcBulkCurrentED,    0u);

    /* 6. Transition to Operational state.
     *    OHCI spec requires going through Resume before Operational.
     *    Many implementations skip this, but QEMU needs it from USB Reset state. */
    ohci_wr(OHCI_HcControl, OHCI_CTRL_HCFS_RES);
    mdelay(20);   /* USB resume signaling: 20 ms */
    ohci_wr(OHCI_HcControl, OHCI_CTRL_HCFS_OPR);
    mdelay(5);
    kinfo("USB: HcControl after OPR=0x%x\n",
          (unsigned)ohci_rd(OHCI_HcControl));

    /* 7. Discover number of downstream ports and power them all on */
    u32 ndp = ohci_rd(OHCI_HcRhDescriptorA) & 0xFFu;
    kinfo("USB: root hub has %u port(s)\n", (unsigned)ndp);

    for (u32 p = 0; p < ndp; p++)
        ohci_wr(OHCI_HcRhPortStatus(p), OHCI_PORT_SET_POWER);

    mdelay(100);   /* USB power-on stabilization (spec: 100 ms) */

    /* 8. Enumerate each port that has a device */
    u8 next_addr = 1;
    for (u32 p = 0; p < ndp && g_hid_count < MAX_HID; p++) {
        u32 ps = ohci_rd(OHCI_HcRhPortStatus(p));
        if (!(ps & OHCI_PORT_CCS)) {
            kinfo("USB: port %u empty\n", (unsigned)p);
            continue;
        }

        kinfo("USB: port %u detected ps=0x%x\n", (unsigned)p, (unsigned)ps);

        /* Reset port — required before enumeration */
        ohci_wr(OHCI_HcRhPortStatus(p), OHCI_PORT_SET_RESET);
        mdelay(60);   /* USB spec: minimum 10 ms; give 60 ms */

        /* Wait for PRS (Port Reset Status) to clear */
        {
            u64 freq = arm_cntfrq(); if (!freq) freq = 62500000ULL;
            u64 start = arm_cntpct();
            u64 lim   = (freq / 1000ULL) * 200ULL;
            while ((ohci_rd(OHCI_HcRhPortStatus(p)) & OHCI_PORT_PRS) &&
                   (arm_cntpct() - start) < lim)
                __asm__ volatile("nop");
        }

        /* Clear reset status change bit */
        ohci_wr(OHCI_HcRhPortStatus(p), OHCI_PORT_CLR_RESET);
        mdelay(5);

        ps = ohci_rd(OHCI_HcRhPortStatus(p));
        if (!(ps & OHCI_PORT_PES)) {
            kwarn("USB: port %u reset failed ps=0x%x\n",
                  (unsigned)p, (unsigned)ps);
            continue;
        }

        int is_ls = (ps & OHCI_PORT_LSDA) ? 1 : 0;
        enumerate_port(p, next_addr, is_ls);

        if (g_hid[g_hid_count - 1].active)
            next_addr++;
    }

    if (!g_hid_count) {
        kwarn("USB: no HID devices found\n");
        return;
    }

    /* 9. Set up interrupt EDs for all found devices */
    for (int i = 0; i < g_hid_count; i++)
        setup_int_ed(i);

    /* 10. Link EDs into HCCA interrupt table and enable periodic list */
    link_int_eds_into_hcca();
    ohci_wr(OHCI_HcHCCA, PHYS(&g_hcca));
    ohci_wr(OHCI_HcControl, OHCI_CTRL_HCFS_OPR | OHCI_CTRL_PLE);

    kinfo("USB: HcCtl final=0x%x HCCA=0x%lx\n",
          (unsigned)ohci_rd(OHCI_HcControl),
          (unsigned long)PHYS(&g_hcca));
    for (int i = 0; i < g_hid_count; i++) {
        kinfo("  int ED[%d] fl=0x%lx hd=0x%lx tl=0x%lx\n", i,
              (unsigned long)g_int_ed[i].flags,
              (unsigned long)g_int_ed[i].head_td,
              (unsigned long)g_int_ed[i].tail_td);
        kinfo("  int TD[%d] fl=0x%lx cbp=0x%lx be=0x%lx\n", i,
              (unsigned long)g_int_td[i].flags,
              (unsigned long)g_int_td[i].cbp,
              (unsigned long)g_int_td[i].be);
    }
    kinfo("USB: HID subsystem ready (%d device(s))\n", g_hid_count);
}
