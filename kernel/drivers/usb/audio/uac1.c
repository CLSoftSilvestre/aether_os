/*
 * AetherOS — USB Audio Class 1.0 Driver (Phase 8.3)
 * File: kernel/drivers/usb/audio/uac1.c
 *
 * Provides playback output for QEMU's virtual usb-audio device
 * (VID 0x46f4, PID 0x0002, UAC1 protocol=0x00).
 *
 * This driver does NOT register with audio_core.  Instead it acts as an
 * output tap: SYS_AUDIO_WRITE calls uac1_write_pcm() after writing to
 * the PWM playback ring, forwarding each period's processed audio
 * directly to the USB OUT endpoint.  PWM remains the default capture
 * device (440 Hz test tone).
 *
 * Enumeration sequence:
 *   1. Scan xHCI slots for Audio class, subclass AudioStreaming, protocol=0x00
 *      and alternate setting > 0 (active streaming interface).
 *   2. Find the isochronous OUT endpoint within that interface.
 *   3. Issue SET_INTERFACE(intf, alt=1) to enable streaming.
 *   4. Issue SET_CUR sampling-freq on the OUT endpoint (48000 Hz).
 *
 * Playback (per-period):
 *   uac1_write_pcm(buf, frames, ch) copies the interleaved s16 buffer to
 *   a static DMA buffer and issues xhci_bulk_xfer (treated as iso-bulk
 *   by the xHCI shim) to the OUT endpoint.
 */

#include "drivers/usb/audio/uac1.h"
#include "drivers/usb/xhci.h"
#include "aether/printk.h"
#include "aether/types.h"

/* ── Descriptor types shared with USB stack ──────────────────────────── */

#define USB_CLASS_AUDIO    0x01
#define USB_SUBCLASS_AS    0x02   /* AudioStreaming */
#define USB_EP_ATTR_ISOCH  0x01

/* Interface descriptor (copied from uac2.c shared layout) */
typedef struct __attribute__((packed)) {
    u8 bLength;
    u8 bDescriptorType;
    u8 bInterfaceNumber;
    u8 bAlternateSetting;
    u8 bNumEndpoints;
    u8 bInterfaceClass;
    u8 bInterfaceSubClass;
    u8 bInterfaceProtocol;
    u8 iInterface;
} uac1_intf_desc_t;

/* Endpoint descriptor */
typedef struct __attribute__((packed)) {
    u8  bLength;
    u8  bDescriptorType;
    u8  bEndpointAddress;
    u8  bmAttributes;
    u16 wMaxPacketSize;
    u8  bInterval;
} uac1_ep_desc_t;

/* ── Device state ────────────────────────────────────────────────────── */

typedef struct {
    u8  slot_id;
    u8  as_out_intf;    /* AudioStreaming interface number for OUT     */
    u8  ep_out_addr;    /* full endpoint address (for control request) */
    u8  ep_out;         /* endpoint number (bits 3:0)                  */
    u8  ep_out_dbi;     /* xHCI doorbell index                         */
    u16 ep_out_mps;     /* max packet size from endpoint descriptor     */
    int running;
} uac1_priv_t;

static uac1_priv_t g_uac1_priv;

/* ── DMA output buffer ───────────────────────────────────────────────── */
/* 256 frames × 2 ch × 2 bytes = 1024 bytes max per period */
static s16 g_uac1_tx[256 * 2];

/* ── Control helpers ─────────────────────────────────────────────────── */

static int uac1_set_intf(u8 slot_id, u8 intf_num, u8 alt)
{
    usb_setup_t setup = {
        .bmRequestType = 0x01,   /* Standard, Interface, Host-to-Device */
        .bRequest      = 0x0B,   /* SET_INTERFACE                        */
        .wValue        = alt,
        .wIndex        = intf_num,
        .wLength       = 0,
    };
    return xhci_ctrl_xfer(slot_id, &setup, NULL);
}

static void uac1_set_sample_rate(u8 slot_id, u8 ep_addr, u32 rate)
{
    /* UAC1: SET_CUR on endpoint, 3-byte LE frequency value */
    usb_setup_t setup = {
        .bmRequestType = 0x22,   /* Class, Endpoint, Host-to-Device */
        .bRequest      = UAC1_SET_CUR,
        .wValue        = (u16)(UAC1_SAMPLING_FREQ_CTRL << 8),
        .wIndex        = ep_addr,
        .wLength       = 3,
    };
    u8 rate_buf[3] = {
        (u8)(rate & 0xFF),
        (u8)((rate >> 8)  & 0xFF),
        (u8)((rate >> 16) & 0xFF),
    };
    xhci_ctrl_xfer(slot_id, &setup, rate_buf);
}

/* ── Init: scan xHCI slots for UAC1 audio streaming device ──────────── */

void uac1_init(void)
{
    if (!xhci_ready()) {
        kinfo("uac1: xHCI not ready, UAC1 skipped\n");
        return;
    }

    static u8 cfg_buf[512];

    /* Scan only assigned slots — probing empty slots burns a 500 ms timeout each. */
    u8 nslots = xhci_num_slots();
    for (u8 slot = 1; slot <= nslots; slot++) {
        usb_setup_t get_cfg = {
            .bmRequestType = 0x80,
            .bRequest      = 0x06,   /* GET_DESCRIPTOR */
            .wValue        = 0x0200, /* Configuration, index 0 */
            .wIndex        = 0,
            .wLength       = sizeof(cfg_buf),
        };

        int r = xhci_ctrl_xfer(slot, &get_cfg, cfg_buf);
        if (r < 0) continue;

        u8 *p   = cfg_buf;
        u8 *end = cfg_buf + sizeof(cfg_buf);

        int in_uac1_as = 0;
        u8  cur_intf   = 0;
        u8  found      = 0;
        uac1_priv_t probe = {0};
        probe.slot_id = slot;

        while (p + 2 <= end && p[0] >= 2) {
            u8 len  = p[0];
            u8 type = p[1];

            if (type == 0x04 && len >= 9) {   /* Interface descriptor */
                uac1_intf_desc_t *intf = (uac1_intf_desc_t *)p;
                cur_intf = intf->bInterfaceNumber;
                in_uac1_as = (intf->bInterfaceClass    == USB_CLASS_AUDIO  &&
                              intf->bInterfaceSubClass == USB_SUBCLASS_AS  &&
                              intf->bInterfaceProtocol == USB_PROTOCOL_UAC1 &&
                              intf->bAlternateSetting  >  0);
            }

            if (type == 0x05 && len >= 7 && in_uac1_as) {
                uac1_ep_desc_t *ep = (uac1_ep_desc_t *)p;
                if ((ep->bmAttributes & 0x03) == USB_EP_ATTR_ISOCH &&
                    !(ep->bEndpointAddress & 0x80)) {
                    /* Isochronous OUT endpoint — playback */
                    probe.as_out_intf = cur_intf;
                    probe.ep_out_addr = ep->bEndpointAddress;
                    probe.ep_out      = ep->bEndpointAddress & 0x0F;
                    probe.ep_out_dbi  = 2u * probe.ep_out;
                    probe.ep_out_mps  = ep->wMaxPacketSize & 0x07FFu;
                    found = 1;
                    break;
                }
            }

            p += len;
            if (p >= end) break;
        }

        if (!found) continue;

        kinfo("uac1: slot %u — OUT ep=0x%02x intf=%u\n",
              slot, probe.ep_out_addr, probe.as_out_intf);

        /* Activate streaming alternate setting */
        if (uac1_set_intf(slot, probe.as_out_intf, 1) < 0) {
            kinfo("uac1: SET_INTERFACE failed\n");
            continue;
        }

        /* Set sample rate (best-effort — some devices don't support it) */
        uac1_set_sample_rate(slot, probe.ep_out_addr, 48000);

        /* Configure the isochronous OUT endpoint in xHCI (treated as bulk OUT) */
        extern void xhci_configure_bulk_out_ep(u8 sid, u8 ep_num, u16 mps);
        xhci_configure_bulk_out_ep(slot, probe.ep_out,
                                   probe.ep_out_mps ? probe.ep_out_mps : 192u);

        probe.running = 1;
        g_uac1_priv   = probe;

        kinfo("uac1: USB audio output active at 48000 Hz stereo\n");
        return;
    }

    kinfo("uac1: no UAC1 audio device found\n");
}

/* ── Playback output ─────────────────────────────────────────────────── */

void uac1_write_pcm(const s16 *buf, u32 frames, u8 ch)
{
    if (!g_uac1_priv.running) return;

    /* Cap to static buffer size */
    u32 max_frames = (u32)(sizeof(g_uac1_tx) / (sizeof(s16) * (ch ? ch : 2u)));
    if (frames > max_frames) frames = max_frames;

    u32 n = frames * ch;
    for (u32 i = 0; i < n; i++)
        g_uac1_tx[i] = buf[i];

    xhci_bulk_xfer(g_uac1_priv.slot_id,
                   g_uac1_priv.ep_out,
                   g_uac1_priv.ep_out_dbi,
                   g_uac1_tx,
                   frames * ch * (u32)sizeof(s16),
                   0 /*OUT*/);
}
