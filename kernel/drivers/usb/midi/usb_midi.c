/*
 * AetherOS — USB MIDI 1.0 Class Driver (Phase 8.1)
 * File: kernel/drivers/usb/midi/usb_midi.c
 *
 * USB MIDI uses bulk endpoints with 4-byte USB-MIDI Event Packets (UMPs).
 * Each UMP contains: Cable Number (4-bit) | Code Index Number (4-bit) |
 * MIDI_0 | MIDI_1 | MIDI_2
 *
 * UMC202HD exposes a MIDIStreaming interface on the same USB device as
 * the AudioStreaming interfaces.  We scan for USB_SUBCLASS_MIDI interfaces
 * and set up polling on the bulk IN endpoint.
 *
 * MIDI events are stored in a lock-free single-producer / single-consumer
 * ring buffer.  The USB polling task is the producer; userspace reads via
 * SYS_MIDI_READ.
 */

#include "drivers/usb/midi/usb_midi.h"
#include "aether/printk.h"
#include "aether/types.h"
#include "drivers/usb/xhci.h"

/* ── USB MIDI constants ──────────────────────────────────────────────── */

#define USB_CLASS_AUDIO     0x01
#define USB_SUBCLASS_MIDI   0x03

/* Code Index Numbers (CIN) — first nibble of UMP byte 0 */
#define CIN_NOTE_OFF        0x08
#define CIN_NOTE_ON         0x09
#define CIN_POLY_PRESSURE   0x0A
#define CIN_CONTROL_CHANGE  0x0B
#define CIN_PROGRAM_CHANGE  0x0C
#define CIN_CHANNEL_PRESSURE 0x0D
#define CIN_PITCH_BEND      0x0E
#define CIN_SYSEX_START     0x04
#define CIN_SYSEX_END_3     0x07

/* ── Ring buffer ─────────────────────────────────────────────────────── */

static midi_event_t g_ring[MIDI_RING_SIZE];
static volatile u32 g_write = 0;
static volatile u32 g_read  = 0;

static void ring_push(u8 status, u8 d1, u8 d2)
{
    u32 next = (g_write + 1) % MIDI_RING_SIZE;
    if (next == g_read) return;   /* overflow: drop event */
    g_ring[g_write].status = status;
    g_ring[g_write].data1  = d1;
    g_ring[g_write].data2  = d2;
    g_ring[g_write]._pad   = 0;
    g_write = next;
}

/* ── USB MIDI bulk IN polling buffer ─────────────────────────────────── */

#define MIDI_BUF_SIZE 64

static u8    g_midi_buf[MIDI_BUF_SIZE];
static u8    g_midi_slot    = 0;
static u8    g_midi_ep_in   = 0;
static u8    g_midi_ep_dbi  = 0;
static int   g_midi_ready   = 0;

/* ── Parse 4-byte USB-MIDI Event Packets ─────────────────────────────── */

static void parse_ump(const u8 *p)
{
    u8 cin    = p[0] & 0x0F;
    u8 status = p[1];
    u8 d1     = p[2];
    u8 d2     = p[3];

    switch (cin) {
    case CIN_NOTE_OFF:
    case CIN_NOTE_ON:
    case CIN_POLY_PRESSURE:
    case CIN_CONTROL_CHANGE:
    case CIN_PITCH_BEND:
        ring_push(status, d1, d2);
        break;
    case CIN_PROGRAM_CHANGE:
    case CIN_CHANNEL_PRESSURE:
        ring_push(status, d1, 0);
        break;
    default:
        break;   /* SysEx, real-time, etc. — ignore for now */
    }
}

/* ── Poll ────────────────────────────────────────────────────────────── */

void usb_midi_poll(void)
{
    if (!g_midi_ready) return;

    int n = xhci_bulk_xfer(g_midi_slot, g_midi_ep_in, g_midi_ep_dbi,
                           g_midi_buf, MIDI_BUF_SIZE, 1 /*IN*/);
    if (n <= 0) return;

    for (int i = 0; i + 3 < n; i += 4)
        parse_ump(&g_midi_buf[i]);
}

/* ── Userspace read / write ──────────────────────────────────────────── */

int usb_midi_read(midi_event_t *out, int max_events)
{
    int count = 0;
    while (count < max_events && g_read != g_write) {
        out[count++] = g_ring[g_read];
        g_read = (g_read + 1) % MIDI_RING_SIZE;
    }
    return count;
}

int usb_midi_write(const midi_event_t *events, int count)
{
    if (!g_midi_ready || !g_midi_slot) return -1;
    /* USB MIDI OUT: pack each event into a 4-byte UMP and bulk-out */
    static u8 tx_buf[MIDI_BUF_SIZE];
    int bytes = 0;
    for (int i = 0; i < count && bytes + 4 <= (int)sizeof(tx_buf); i++) {
        u8 cin = (events[i].status >> 4) & 0x0F;
        tx_buf[bytes+0] = cin;
        tx_buf[bytes+1] = events[i].status;
        tx_buf[bytes+2] = events[i].data1;
        tx_buf[bytes+3] = events[i].data2;
        bytes += 4;
    }
    if (!bytes) return 0;
    /* Use endpoint OUT (not IN) — we repurpose ep_in-1 as ep_out heuristic */
    u8 ep_out = (g_midi_ep_in > 0) ? g_midi_ep_in - 1 : 1;
    u8 ep_dbi = ep_out * 2;
    xhci_bulk_xfer(g_midi_slot, ep_out, ep_dbi, tx_buf, bytes, 0 /*OUT*/);
    return count;
}

/* ── Init ────────────────────────────────────────────────────────────── */

void usb_midi_init(void)
{
    if (!xhci_ready()) {
        kinfo("usb_midi: xHCI not ready, MIDI skipped\n");
        return;
    }

    static u8 cfg_buf[512];

    for (u8 slot = 1; slot <= 32; slot++) {
        usb_setup_t get_cfg = {
            .bmRequestType = 0x80,
            .bRequest      = 0x06,
            .wValue        = 0x0200,
            .wIndex        = 0,
            .wLength       = sizeof(cfg_buf),
        };
        if (xhci_ctrl_xfer(slot, &get_cfg, cfg_buf) < 0) continue;

        u8 *p   = cfg_buf;
        u8 *end = cfg_buf + sizeof(cfg_buf);

        while (p + 2 <= end && p[0] >= 2) {
            u8 len = p[0], type = p[1];
            if (type == 0x04 && len >= 9) {
                u8 cls  = p[5], sub = p[6];
                if (cls == USB_CLASS_AUDIO && sub == USB_SUBCLASS_MIDI) {
                    /* Next descriptor should be endpoint */
                    u8 *np = p + len;
                    if (np + 2 <= end && np[1] == 0x05 && (np[2] & 0x80)) {
                        /* IN endpoint found */
                        g_midi_slot   = slot;
                        g_midi_ep_in  = np[2] & 0x0F;
                        g_midi_ep_dbi = 2 * g_midi_ep_in + 1;
                        g_midi_ready  = 1;
                        kinfo("usb_midi: MIDI IN on slot %u ep %u\n",
                              slot, g_midi_ep_in);
                        return;
                    }
                }
            }
            p += len;
        }
    }

    kinfo("usb_midi: no USB MIDI device found\n");
}
