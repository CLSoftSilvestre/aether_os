/*
 * AetherOS — USB Audio Class 2.0 Driver (Phase 8.1)
 * File: kernel/drivers/usb/audio/uac2.c
 *
 * Supports any UAC2-compliant USB audio interface.  Primary target:
 * Behringer UMC202HD (VID 0x1397 PID 0x0507).
 *
 * UAC2 signal path:
 *   Host (xHCI) ←── Isochronous IN ←── UMC202HD (capture: guitar input)
 *   Host (xHCI) ──► Isochronous OUT ──► UMC202HD (playback: monitor/headphone)
 *   Host (xHCI) ←── Feedback endpoint ←── UMC202HD (async USB clock sync)
 *
 * Enumeration sequence:
 *   1. xhci_init() already ran; device slot allocated by USB MSC probe code.
 *   2. We scan all enumerated slots looking for Audio class interfaces.
 *   3. For each slot with USB_CLASS_AUDIO we:
 *        a) Parse the AudioControl interface (find clock source, terminals)
 *        b) Parse the AudioStreaming interfaces (find IN/OUT endpoints)
 *        c) Issue SET_CUR sample rate = 48 kHz via control EP0
 *        d) Register an audio_dev_t
 *
 * Streaming (after audio_dev_start):
 *   xHCI isochronous transfers are currently simulated as bulk transfers
 *   on the polling path.  Full isochronous TRB scheduling is deferred to
 *   Phase 8.1 completion; this version sets up the control path and format
 *   negotiation so the device is correctly initialised.
 *
 * PCM conversion:
 *   UAC2 delivers 24-bit packed integers (3 bytes per sample).
 *   We convert to float32 in the DMA completion path before calling the
 *   audio callback.  Output float32 is converted back to 24-bit for TX.
 */

#include "drivers/usb/audio/uac2.h"
#include "aether/audio_dev.h"
#include "aether/sched.h"
#include "aether/printk.h"
#include "aether/types.h"
#include "drivers/usb/xhci.h"

/* ── USB descriptor walking helpers ─────────────────────────────────── */

/* Standard USB descriptor header */
typedef struct __attribute__((packed)) {
    u8 bLength;
    u8 bDescriptorType;
} usb_desc_hdr_t;

/* Interface descriptor */
typedef struct __attribute__((packed)) {
    u8 bLength;
    u8 bDescriptorType;    /* 0x04 */
    u8 bInterfaceNumber;
    u8 bAlternateSetting;
    u8 bNumEndpoints;
    u8 bInterfaceClass;
    u8 bInterfaceSubClass;
    u8 bInterfaceProtocol;
    u8 iInterface;
} usb_intf_desc_t;

/* Endpoint descriptor */
typedef struct __attribute__((packed)) {
    u8 bLength;
    u8 bDescriptorType;    /* 0x05 */
    u8 bEndpointAddress;   /* bit7: 1=IN, 0=OUT; bits3-0: endpoint number */
    u8 bmAttributes;       /* bits1-0: 01=isochronous */
    u16 wMaxPacketSize;
    u8 bInterval;
} usb_ep_desc_t;

/* UAC2 AudioStreaming class-specific interface descriptor */
typedef struct __attribute__((packed)) {
    u8 bLength;
    u8 bDescriptorType;    /* 0x24 = CS_INTERFACE */
    u8 bDescriptorSubtype; /* UAC2_AS_GENERAL */
    u8 bTerminalLink;
    u8 bmControls;
    u8 bFormatType;
    u32 bmFormats;
    u8 bNrChannels;
    u32 bmChannelConfig;
    u8 iChannelNames;
} __attribute__((packed)) uac2_as_general_t;

/* ── DMA buffers ─────────────────────────────────────────────────────── */

/* 8-packet ring: 1 ms per USB HS microframe × 8 = 8 ms max latency.
 * 48 kHz × 2 ch × 3 bytes = 288 bytes/ms (one full-speed packet).
 * HS = 8 packets per 1 ms → 36 bytes per 125 µs packet.              */
#define UAC2_RING_PACKETS   8
#define UAC2_PACKET_BYTES   192    /* 48000 Hz × 2ch × 24-bit / 8000 pkt/s */
#define UAC2_RING_BYTES     (UAC2_RING_PACKETS * UAC2_PACKET_BYTES)

static u8 g_rx_ring[UAC2_RING_BYTES];   /* capture ring (IN endpoint)    */
static u8 g_tx_ring[UAC2_RING_BYTES];   /* playback ring (OUT endpoint)  */

/* ── PCM conversion helpers (integer only — no floats in kernel) ─────── */

/* 24-bit packed LE signed → s16 (truncate lower 8 bits).
 * The AetherSound server does the full float conversion in userspace.   */
static s16 pcm24_to_s16(const u8 *p)
{
    s32 s = (s32)p[0] | ((s32)p[1] << 8) | ((s32)p[2] << 16);
    if (s & 0x800000) s |= (s32)0xFF000000;   /* sign extend to 32-bit  */
    return (s16)(s >> 8);                      /* take top 16 bits       */
}

/* s16 → 24-bit packed LE signed (pad with zero LSB).                   */
static void s16_to_pcm24(s16 v, u8 *p)
{
    s32 s = (s32)v << 8;
    p[0] = (u8)(s & 0xFF);
    p[1] = (u8)((s >> 8)  & 0xFF);
    p[2] = (u8)((s >> 16) & 0xFF);
}

/* ── Device state ────────────────────────────────────────────────────── */

typedef struct {
    u8  slot_id;        /* xHCI slot for this USB device                */
    u8  as_out_intf;    /* AudioStreaming OUT interface number           */
    u8  as_in_intf;     /* AudioStreaming IN interface number            */
    u8  ep_out;         /* OUT isochronous endpoint address             */
    u8  ep_in;          /* IN isochronous endpoint address              */
    u8  ep_out_dbi;     /* doorbell index for OUT                       */
    u8  ep_in_dbi;      /* doorbell index for IN                        */
    int running;
    u32 sample_rate;
} uac2_priv_t;

static uac2_priv_t  g_uac2_priv;
static audio_dev_t  g_uac2_dev;
static audio_dev_ops_t g_uac2_ops;

/* ── Class-specific control requests ─────────────────────────────────── */

static int uac2_set_sample_rate(u8 slot_id, u8 clock_id, u32 rate)
{
    usb_setup_t setup = {
        .bmRequestType = 0x21,   /* Class, Interface, Host-to-Device */
        .bRequest      = UAC2_SET_CUR,
        .wValue        = (u16)(UAC2_CS_SAM_FREQ << 8),
        .wIndex        = (u16)((clock_id << 8) | 0x00),  /* clock entity | intf 0 */
        .wLength       = 4,
    };
    u32 rate_le = rate;   /* little-endian 32-bit */
    return xhci_ctrl_xfer(slot_id, &setup, &rate_le);
}

/* ── SET_INTERFACE to enable non-zero alternate setting ─────────────── */

static int uac2_set_intf(u8 slot_id, u8 intf_num, u8 alt_setting)
{
    usb_setup_t setup = {
        .bmRequestType = 0x01,   /* Standard, Interface, Host-to-Device */
        .bRequest      = 0x0B,   /* SET_INTERFACE */
        .wValue        = alt_setting,
        .wIndex        = intf_num,
        .wLength       = 0,
    };
    return xhci_ctrl_xfer(slot_id, &setup, NULL);
}

/* ── Ops ─────────────────────────────────────────────────────────────── */

static int uac2_configure(audio_dev_t *dev, u32 sample_rate,
                          u8 bit_depth, u8 channels)
{
    uac2_priv_t *p = (uac2_priv_t *)dev->priv;
    (void)bit_depth; (void)channels;

    /* Activate alternate setting 1 (zero-bandwidth off → streaming on) */
    if (p->as_out_intf)
        uac2_set_intf(p->slot_id, p->as_out_intf, 1);
    if (p->as_in_intf)
        uac2_set_intf(p->slot_id, p->as_in_intf, 1);

    /* Set sample rate on clock source entity 1 (standard UAC2 device) */
    uac2_set_sample_rate(p->slot_id, 1, sample_rate);
    p->sample_rate = sample_rate;

    kinfo("uac2: slot %u configured: %u Hz, 24-bit, 2ch\n",
          p->slot_id, sample_rate);
    return 0;
}

static int uac2_start(audio_dev_t *dev)
{
    uac2_priv_t *p = (uac2_priv_t *)dev->priv;
    p->running = 1;
    kinfo("uac2: streaming started on slot %u\n", p->slot_id);
    return 0;
}

static int uac2_stop(audio_dev_t *dev)
{
    uac2_priv_t *p = (uac2_priv_t *)dev->priv;
    p->running = 0;
    /* Return to alt setting 0 (zero bandwidth) */
    if (p->as_out_intf) uac2_set_intf(p->slot_id, p->as_out_intf, 0);
    if (p->as_in_intf)  uac2_set_intf(p->slot_id, p->as_in_intf,  0);
    return 0;
}

static void uac2_close(audio_dev_t *dev)
{
    uac2_stop(dev);
}

/* ── Periodic polling — DMA transfers, PCM ring update ──────────────── */

/*
 * uac2_audio_poll — called periodically from the audio server poll loop.
 * Transfers raw 24-bit PCM between USB device and the shared PCM rings.
 * No float operations here — conversion happens in the AetherSound server.
 */
void uac2_audio_poll(void)
{
    if (!g_uac2_priv.running) return;

    u32 frames = g_uac2_dev.period_frames;

    /* Receive (IN): read captured audio from USB device → capture_ring */
    if (g_uac2_priv.ep_in) {
        u32 bytes = frames * 2 * 3;   /* stereo 24-bit */
        xhci_bulk_xfer(g_uac2_priv.slot_id, g_uac2_priv.ep_in,
                       g_uac2_priv.ep_in_dbi, g_rx_ring, bytes, 1 /*IN*/);

        /* Convert 24-bit → s16 and push to capture ring */
        audio_pcm_ring_t *cap = &g_uac2_dev.capture_ring;
        for (u32 f = 0; f < frames; f++) {
            const u8 *l = &g_rx_ring[f * 6];
            const u8 *r = l + 3;
            u32 wi_l = (cap->write_idx * 2)     % (AUDIO_PCM_RING_FRAMES * 2);
            u32 wi_r = (cap->write_idx * 2 + 1) % (AUDIO_PCM_RING_FRAMES * 2);
            cap->pcm[wi_l] = pcm24_to_s16(l);
            cap->pcm[wi_r] = pcm24_to_s16(r);
            cap->write_idx = (cap->write_idx + 1) % AUDIO_PCM_RING_FRAMES;
        }
    }

    /* Transmit (OUT): read from playback_ring → convert to 24-bit → USB OUT */
    if (g_uac2_priv.ep_out) {
        audio_pcm_ring_t *play = &g_uac2_dev.playback_ring;
        u32 available = (play->write_idx - play->read_idx
                         + AUDIO_PCM_RING_FRAMES) % AUDIO_PCM_RING_FRAMES;
        u32 consume = available < frames ? available : frames;

        for (u32 f = 0; f < consume; f++) {
            u32 ri_l = (play->read_idx * 2)     % (AUDIO_PCM_RING_FRAMES * 2);
            u32 ri_r = (play->read_idx * 2 + 1) % (AUDIO_PCM_RING_FRAMES * 2);
            u8 *out_l = &g_tx_ring[f * 6];
            u8 *out_r = out_l + 3;
            s16_to_pcm24(play->pcm[ri_l], out_l);
            s16_to_pcm24(play->pcm[ri_r], out_r);
            play->read_idx = (play->read_idx + 1) % AUDIO_PCM_RING_FRAMES;
        }
        /* Pad with silence if server hasn't written enough */
        for (u32 f = consume; f < frames; f++) {
            u8 *out_l = &g_tx_ring[f * 6];
            out_l[0] = out_l[1] = out_l[2] = 0;
            out_l[3] = out_l[4] = out_l[5] = 0;
        }

        u32 bytes = frames * 2 * 3;
        xhci_bulk_xfer(g_uac2_priv.slot_id, g_uac2_priv.ep_out,
                       g_uac2_priv.ep_out_dbi, g_tx_ring, bytes, 0 /*OUT*/);
    }

    sched_rt_record_callback(sched_rt_timestamp_ns(), 0);
}

/* ── Init: scan xHCI slots for Audio class devices ───────────────────── */

void uac2_init(void)
{
    if (!xhci_ready()) {
        kinfo("uac2: xHCI not ready, UAC2 skipped\n");
        return;
    }

    /* Scan enumerated USB devices for Audio class.
     * xhci_ctrl_xfer with GET_DESCRIPTOR lets us read the configuration
     * descriptor to find audio interfaces.  We try slots 1-MAX_SLOTS.    */

    /* GET_DESCRIPTOR for Configuration descriptor (type=0x02, index=0) */
    static u8 cfg_buf[512];

    for (u8 slot = 1; slot <= 32; slot++) {
        usb_setup_t get_cfg = {
            .bmRequestType = 0x80,  /* Standard, Device, Device-to-Host */
            .bRequest      = 0x06,  /* GET_DESCRIPTOR                   */
            .wValue        = 0x0200,/* Configuration descriptor, index 0*/
            .wIndex        = 0,
            .wLength       = sizeof(cfg_buf),
        };

        int r = xhci_ctrl_xfer(slot, &get_cfg, cfg_buf);
        if (r < 0) continue;   /* slot not active */

        /* Walk descriptors looking for Audio interfaces */
        u8 *p   = cfg_buf;
        u8 *end = cfg_buf + sizeof(cfg_buf);

        uac2_priv_t probe = {0};
        probe.slot_id = slot;
        int found_audio = 0;

        while (p + 2 <= end && p[0] >= 2) {
            u8 len  = p[0];
            u8 type = p[1];

            if (type == 0x04 && len >= 9) {  /* Interface descriptor */
                usb_intf_desc_t *intf = (usb_intf_desc_t *)p;
                if (intf->bInterfaceClass    == USB_CLASS_AUDIO &&
                    intf->bInterfaceProtocol == USB_PROTOCOL_UAC2) {
                    found_audio = 1;
                    if (intf->bInterfaceSubClass == USB_SUBCLASS_AS) {
                        /* Look at next descriptor for direction */
                        u8 *np = p + len;
                        if (np + 2 <= end && np[1] == 0x05) {  /* endpoint */
                            usb_ep_desc_t *ep = (usb_ep_desc_t *)np;
                            if ((ep->bmAttributes & 0x03) == USB_EP_ATTR_ISOCH) {
                                if (ep->bEndpointAddress & 0x80) {
                                    /* IN endpoint (capture) */
                                    probe.as_in_intf = intf->bInterfaceNumber;
                                    probe.ep_in      = ep->bEndpointAddress & 0x0F;
                                    probe.ep_in_dbi  = 2 * probe.ep_in + 1;
                                } else {
                                    /* OUT endpoint (playback) */
                                    probe.as_out_intf = intf->bInterfaceNumber;
                                    probe.ep_out      = ep->bEndpointAddress & 0x0F;
                                    probe.ep_out_dbi  = 2 * probe.ep_out;
                                }
                            }
                        }
                    }
                }
            }

            p += len;
            if (p >= end) break;
        }

        if (!found_audio) continue;

        kinfo("uac2: found Audio device on slot %u (ep_in=%u ep_out=%u)\n",
              slot, probe.ep_in, probe.ep_out);

        /* Populate device */
        g_uac2_priv = probe;

        g_uac2_ops.configure = uac2_configure;
        g_uac2_ops.start     = uac2_start;
        g_uac2_ops.stop      = uac2_stop;
        g_uac2_ops.close     = uac2_close;

        const char *nm = "UAC2-audio";
        int i = 0;
        while (nm[i] && i < AUDIO_NAME_MAX - 1)
            { g_uac2_dev.info.name[i] = nm[i]; i++; }
        g_uac2_dev.info.name[i]         = '\0';
        g_uac2_dev.info.type            = AUDIO_DEV_UAC2;
        g_uac2_dev.info.inputs          = 2;
        g_uac2_dev.info.outputs         = 2;
        g_uac2_dev.info.max_sample_rate = 96000;

        g_uac2_dev.ops          = &g_uac2_ops;
        g_uac2_dev.sample_rate  = 48000;
        g_uac2_dev.bit_depth    = 24;
        g_uac2_dev.channels     = 2;
        g_uac2_dev.period_frames= 64;
        g_uac2_dev.callback     = NULL;
        g_uac2_dev.callback_data= NULL;
        g_uac2_dev.priv         = &g_uac2_priv;

        audio_core_register(&g_uac2_dev);
        return;   /* register first audio device found */
    }

    kinfo("uac2: no UAC2 audio device found\n");
}
