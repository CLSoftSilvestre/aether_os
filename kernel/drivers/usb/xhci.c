/*
 * AetherOS — xHCI USB 3.0 Host Controller Driver (Phase 5.2.12)
 * File: kernel/drivers/usb/xhci.c
 *
 * Initialises the xHCI host controller found on PCI, enumerates one
 * root-hub port, and performs USB device enumeration (GET_DESCRIPTOR,
 * SET_ADDRESS, SET_CONFIGURATION).  On a Mass Storage device, calls
 * usb_msc_probe() so the MSC layer can set up bulk endpoints.
 *
 * Design constraints:
 *   - Polling only — no IRQ wiring required.
 *   - Static BSS allocation — zero dynamic allocation.
 *   - One device slot (slot 1).  One port at a time.
 *   - 32-bit physical addresses (kernel < 4 GB).
 */

#include "drivers/usb/xhci.h"
#include "drivers/usb/msc.h"
#include "drivers/pci/pci_ecam.h"
#include "aether/printk.h"
#include "aether/types.h"

/* ── Timing helpers ─────────────────────────────────────────────────────── */

static inline u64 cntpct(void)
{
    u64 v; __asm__ volatile("mrs %0, CNTPCT_EL0" : "=r"(v)); return v;
}
static inline u64 cntfrq(void)
{
    u64 v; __asm__ volatile("mrs %0, CNTFRQ_EL0" : "=r"(v)); return v;
}
static void mdelay(u32 ms)
{
    u64 freq = cntfrq(); if (!freq) freq = 62500000ULL;
    u64 start = cntpct();
    u64 ticks = (freq / 1000ULL) * ms;
    while ((cntpct() - start) < ticks) __asm__ volatile("nop");
}

#define DSB() __asm__ volatile("dsb sy" ::: "memory")

/* ── Freestanding helpers ────────────────────────────────────────────────── */

static void xm_zero(void *dst, u32 n)
{
    u8 *p = (u8 *)dst; while (n--) *p++ = 0;
}
static void xm_copy(void *dst, const void *src, u32 n)
{
    u8 *d = (u8 *)dst; const u8 *s = (const u8 *)src;
    while (n--) *d++ = *s++;
}

/* ── Physical address helper (identity-mapped kernel) ────────────────────── */

#define PHYS(p) ((u32)(uintptr_t)(p))

/* ── Static xHCI data structures ────────────────────────────────────────── */

/*
 * DCBAA: Device Context Base Address Array.
 * Entry 0 = scratchpad pointer (we leave it 0).
 * Entry 1..2 = Output Device Contexts for slots 1 and 2.
 */
static u64 g_dcbaa[XHCI_MAX_SLOTS + 1]
    __attribute__((aligned(64)));

/* Output Device Contexts — one per slot (supports up to 2 devices) */
#define XHCI_MAX_DEV_SLOTS 2
static xhci_dev_ctx_t g_dev_ctx[XHCI_MAX_DEV_SLOTS]
    __attribute__((aligned(64)));

/* Input Context used for Address Device and Configure Endpoint commands */
static xhci_input_ctx_t g_input_ctx
    __attribute__((aligned(64)));

/* Command Ring: XHCI_CMD_RING_SIZE TRBs, last is Link TRB back to start */
static xhci_trb_t g_cmd_ring[XHCI_CMD_RING_SIZE]
    __attribute__((aligned(64)));

/* Event Ring: XHCI_EVT_RING_SIZE TRBs */
static xhci_trb_t g_evt_ring[XHCI_EVT_RING_SIZE]
    __attribute__((aligned(64)));

/* Event Ring Segment Table: 1 entry */
static xhci_erst_entry_t g_erst[1]
    __attribute__((aligned(64)));

/* Transfer Rings for EP0 — one per slot (indexed 0=slot1, 1=slot2) */
static xhci_trb_t g_ep0_ring[XHCI_MAX_DEV_SLOTS][XHCI_XFER_RING_SIZE]
    __attribute__((aligned(64)));

/* Transfer Rings for bulk endpoints */
static xhci_trb_t g_ep_out_ring[XHCI_XFER_RING_SIZE]
    __attribute__((aligned(64)));

static xhci_trb_t g_ep_in_ring[XHCI_XFER_RING_SIZE]
    __attribute__((aligned(64)));

/* Data buffer used for control transfers (descriptors, etc.) */
static u8 g_ctrl_buf[512] __attribute__((aligned(64)));

/* ── Driver state ────────────────────────────────────────────────────────── */

static uintptr_t g_cap_base;   /* BAR0 */
static uintptr_t g_op_base;    /* BAR0 + CAPLENGTH */
static uintptr_t g_rt_base;    /* BAR0 + RTSOFF */
static uintptr_t g_db_base;    /* BAR0 + DBOFF */

static u8  g_max_ports;
static int g_ctx64;            /* 1 if Context Size = 64 bytes */
static int g_ready;

/* Command Ring producer state */
static u32 g_cmd_enq;    /* enqueue index into g_cmd_ring[] */
static u8  g_cmd_pcs;    /* producer cycle state (starts 1) */

/* Event Ring consumer state */
static u32 g_evt_deq;    /* dequeue index into g_evt_ring[] */
static u8  g_evt_ccs;    /* consumer cycle state (starts 1) */

/* EP0 transfer ring producer state — per slot (0=slot1, 1=slot2) */
static u32 g_ep0_enq[XHCI_MAX_DEV_SLOTS];
static u8  g_ep0_pcs[XHCI_MAX_DEV_SLOTS];

/* Bulk endpoint transfer ring producer states */
static u32 g_ep_out_enq;
static u8  g_ep_out_pcs;
static u32 g_ep_in_enq;
static u8  g_ep_in_pcs;

/* ── MMIO helpers ─────────────────────────────────────────────────────────  */

static inline u32 cap_rd(u32 off)
{
    return *(volatile u32 *)(g_cap_base + off);
}
static inline u32 op_rd(u32 off)
{
    return *(volatile u32 *)(g_op_base + off);
}
static inline void op_wr(u32 off, u32 val)
{
    *(volatile u32 *)(g_op_base + off) = val;
    DSB();
}
static inline u32 rt_intr_rd(u32 off)   /* interrupter 0 */
{
    return *(volatile u32 *)(g_rt_base + XHCI_INTR_BASE + off);
}
static inline void rt_intr_wr(u32 off, u32 val)
{
    *(volatile u32 *)(g_rt_base + XHCI_INTR_BASE + off) = val;
    DSB();
}
static inline void db_wr(u8 slot, u32 val)   /* ring doorbell */
{
    *(volatile u32 *)(g_db_base + (u32)slot * 4u) = val;
    DSB();
}

/* ── Event Ring polling ──────────────────────────────────────────────────── */

/*
 * evt_wait — busy-poll the event ring for one event matching trb_type.
 * Returns a copy of the matching event TRB, or fills .dw[2] CC with 0xFF
 * on timeout (500 ms).
 */
static xhci_trb_t evt_wait(u32 trb_type)
{
    u64 freq  = cntfrq(); if (!freq) freq = 62500000ULL;
    u64 limit = (freq / 1000ULL) * 500ULL;
    u64 start = cntpct();

    while ((cntpct() - start) < limit) {
        DSB();
        xhci_trb_t *ev = &g_evt_ring[g_evt_deq];
        u32 ctrl = *(volatile u32 *)&ev->dw[3];
        u8  cycle = (u8)(ctrl & 1u);

        if (cycle != g_evt_ccs) continue;  /* HC hasn't written this yet */

        u32 type = (ctrl >> 10) & 0x3Fu;

        /* Advance consumer pointer */
        g_evt_deq++;
        if (g_evt_deq >= XHCI_EVT_RING_SIZE) {
            g_evt_deq = 0;
            g_evt_ccs ^= 1;
        }

        /* Update ERDP */
        u32 erdp = PHYS(&g_evt_ring[g_evt_deq]) | XHCI_ERDP_EHB;
        rt_intr_wr(XHCI_INTR_ERDP_LO, erdp);
        rt_intr_wr(XHCI_INTR_ERDP_HI, 0);

        if (type == trb_type) {
            xhci_trb_t copy;
            xm_copy(&copy, ev, sizeof(copy));
            return copy;
        }
        /* Discard uninteresting events (port status change etc.) */
    }

    xhci_trb_t timeout;
    xm_zero(&timeout, sizeof(timeout));
    timeout.dw[2] = 0xFFu << 24;   /* completion code 0xFF = internal timeout */
    return timeout;
}

/* ── Command Ring submission ─────────────────────────────────────────────── */

/*
 * cmd_post — post one command TRB to the command ring and ring doorbell 0.
 * Does NOT wait for the completion event.
 */
static void cmd_post(u32 dw0, u32 dw1, u32 dw2, u32 dw3)
{
    xhci_trb_t *trb = &g_cmd_ring[g_cmd_enq];
    trb->dw[0] = dw0;
    trb->dw[1] = dw1;
    trb->dw[2] = dw2;
    trb->dw[3] = dw3 | (u32)g_cmd_pcs;   /* set cycle bit */
    DSB();
    db_wr(0, 0);   /* doorbell 0 = command ring */

    g_cmd_enq++;
    if (g_cmd_enq >= XHCI_CMD_RING_SIZE - 1u) {
        /* Reached the Link TRB — already set up at init, just wrap */
        g_cmd_enq = 0;
        g_cmd_pcs ^= 1;
    }
}

/*
 * cmd_issue — post command and wait for Command Completion Event.
 * Returns the event TRB (check dw[2] CC field).
 */
static xhci_trb_t cmd_issue(u32 dw0, u32 dw1, u32 dw2, u32 dw3)
{
    cmd_post(dw0, dw1, dw2, dw3);
    return evt_wait(TRB_EV_CMD_COMPLETE);
}

/* ── Transfer Ring helpers ───────────────────────────────────────────────── */

static void trb_enqueue(xhci_trb_t *ring, u32 *enq_ptr, u8 *pcs_ptr,
                        u32 size, u32 d0, u32 d1, u32 d2, u32 d3)
{
    u32 enq = *enq_ptr;
    u8  pcs = *pcs_ptr;
    xhci_trb_t *trb = &ring[enq];
    trb->dw[0] = d0;
    trb->dw[1] = d1;
    trb->dw[2] = d2;
    trb->dw[3] = d3 | (u32)pcs;
    DSB();

    enq++;
    if (enq >= size - 1u) {
        /* Reached Link TRB — ring it (cycle bit already in place at init) */
        /* Link TRB cycle must match current PCS so hardware can follow it,
         * then hardware toggles its own cycle when it sees TC=1. */
        xhci_trb_t *link = &ring[size - 1u];
        u32 lctl = link->dw[3];
        lctl = (lctl & ~1u) | (u32)pcs;
        link->dw[3] = lctl;
        DSB();
        enq = 0;
        *pcs_ptr ^= 1;
    }
    *enq_ptr = enq;
}

/* ── Port management ─────────────────────────────────────────────────────── */

static u32 port_rd(u8 port)   /* port = 0-based */
{
    return op_rd(XHCI_OP_PORTSC(port));
}

static void port_wr(u8 port, u32 val)
{
    op_wr(XHCI_OP_PORTSC(port), val);
}

/*
 * Wait for PORTSC bit to reach expected state.
 * mask: bits to check; expected: value those bits should have.
 * Returns 0 on success, -1 on timeout.
 */
static int port_wait(u8 port, u32 mask, u32 expected, u32 ms)
{
    u64 freq  = cntfrq(); if (!freq) freq = 62500000ULL;
    u64 limit = (freq / 1000ULL) * ms;
    u64 start = cntpct();
    while ((cntpct() - start) < limit) {
        DSB();
        if ((port_rd(port) & mask) == expected) return 0;
    }
    return -1;
}

/* ── USB setup packet helpers ────────────────────────────────────────────── */

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

/* ── xhci_ctrl_xfer ──────────────────────────────────────────────────────── */
/*
 * Issue one control transfer (SETUP + optional DATA + STATUS) on EP0
 * of slot slot_id.
 *
 * Uses the per-slot EP0 transfer ring (g_ep0_ring[slot_id-1]).
 * Returns 0 on success, -1 on error.
 */
int xhci_ctrl_xfer(u8 slot_id, const usb_setup_t *setup, void *data)
{
    /* Route to the per-slot EP0 ring so concurrent scans don't cross-contaminate */
    u8 si = (slot_id > 0u && slot_id <= XHCI_MAX_DEV_SLOTS) ? slot_id - 1u : 0u;
    xhci_trb_t *ring = g_ep0_ring[si];

    int data_in  = (setup->bmRequestType & 0x80u) ? 1 : 0;
    u16 data_len = setup->wLength;

    /* Copy SETUP packet to buffer */
    xm_copy(g_ctrl_buf, setup, 8);

    /* ── SETUP Stage TRB ── */
    u32 trt = data_len ? (data_in ? TRB_SETUP_TRT_IN : TRB_SETUP_TRT_OUT)
                       : TRB_SETUP_TRT_NONE;
    trb_enqueue(ring, &g_ep0_enq[si], &g_ep0_pcs[si], XHCI_XFER_RING_SIZE,
        /* dw0 */ *(u32 *)&g_ctrl_buf[0],
        /* dw1 */ *(u32 *)&g_ctrl_buf[4],
        /* dw2 */ 8u,                               /* always 8-byte SETUP */
        /* dw3 */ TRB_CTRL_TYPE(TRB_SETUP_STAGE) | TRB_CTRL_IDT | trt);

    /* ── DATA Stage TRB (optional) ── */
    if (data_len) {
        u8 *buf = (u8 *)g_ctrl_buf + 8;
        if (!data_in && data)
            xm_copy(buf, data, data_len);

        trb_enqueue(ring, &g_ep0_enq[si], &g_ep0_pcs[si], XHCI_XFER_RING_SIZE,
            PHYS(buf), 0,
            data_len,
            TRB_CTRL_TYPE(TRB_DATA_STAGE) | TRB_CTRL_CHAIN
                | (data_in ? TRB_DATA_DIR_IN : TRB_DATA_DIR_OUT));
    }

    /* ── STATUS Stage TRB ── */
    u32 status_dir = data_len ? (data_in ? 0u : TRB_DATA_DIR_IN)
                               : TRB_DATA_DIR_IN;
    trb_enqueue(ring, &g_ep0_enq[si], &g_ep0_pcs[si], XHCI_XFER_RING_SIZE,
        0, 0, 0,
        TRB_CTRL_TYPE(TRB_STATUS_STAGE) | TRB_CTRL_IOC | status_dir);

    /* Ring EP0 doorbell */
    db_wr(slot_id, XHCI_DBI_EP0);

    /* Wait for Transfer Event (status stage completes last) */
    xhci_trb_t ev = evt_wait(TRB_EV_TRANSFER);
    u32 cc = TRB_CC(ev.dw[2]);
    if (cc != TRB_CC_SUCCESS && cc != TRB_CC_SHORT_PKT) {
        kwarn("xHCI: ctrl_xfer CC=%u\n", (unsigned)cc);
        return -1;
    }

    if (data_len && data_in && data)
        xm_copy(data, g_ctrl_buf + 8, data_len);

    return 0;
}

/* ── xhci_bulk_xfer ──────────────────────────────────────────────────────── */

int xhci_bulk_xfer(u8 slot_id, u8 ep_id, u8 dbi, void *buf,
                   u32 len, int dir_in)
{
    xhci_trb_t *ring;
    u32 *enq;
    u8  *pcs;

    if (dir_in) {
        ring = g_ep_in_ring; enq = &g_ep_in_enq; pcs = &g_ep_in_pcs;
    } else {
        ring = g_ep_out_ring; enq = &g_ep_out_enq; pcs = &g_ep_out_pcs;
    }
    (void)ep_id;

    trb_enqueue(ring, enq, pcs, XHCI_XFER_RING_SIZE,
        PHYS(buf), 0,
        len,
        TRB_CTRL_TYPE(TRB_NORMAL) | TRB_CTRL_IOC | TRB_CTRL_ISP);

    db_wr(slot_id, dbi);

    xhci_trb_t ev = evt_wait(TRB_EV_TRANSFER);
    u32 cc = TRB_CC(ev.dw[2]);
    if (cc != TRB_CC_SUCCESS && cc != TRB_CC_SHORT_PKT) {
        kwarn("xHCI: bulk_xfer ep_id=%u CC=%u\n",
              (unsigned)ep_id, (unsigned)cc);
        return -1;
    }

    /* Bytes transferred = requested - remaining */
    u32 remaining = ev.dw[2] & 0x00FFFFFFu;
    return (int)(len - remaining);
}

/* ── Slot allocation & device addressing ─────────────────────────────────── */

/*
 * setup_ep0_context — populate g_input_ctx for a control-only device
 * (Address Device BSR=1 or BSR=0).
 */
static void setup_ep0_context(u8 slot_id, u8 port1, usb_speed_t speed, u16 ep0_mps)
{
    u8 si = (slot_id > 0u && slot_id <= XHCI_MAX_DEV_SLOTS)
            ? slot_id - 1u : 0u;
    xhci_trb_t *ring = g_ep0_ring[si];

    xm_zero(&g_input_ctx, sizeof(g_input_ctx));

    /* Add Slot (A0) and EP0 (A1) contexts */
    g_input_ctx.ctrl_dw[1] = ICTX_A0 | ICTX_A1;

    /* Slot Context: port, speed, route=0, context_entries=1 */
    g_input_ctx.slot.dw[0] = SLOT_CTX_SPEED(speed)
                            | SLOT_CTX_ENTRIES(1);
    g_input_ctx.slot.dw[1] = SLOT_CTX_PORT(port1);

    /* EP0 Context: control endpoint, CErr=3, MPS from device */
    g_ep0_enq[si] = 0;
    g_ep0_pcs[si] = 1;
    /* Link TRB at end of this slot's EP0 ring */
    ring[XHCI_XFER_RING_SIZE - 1u].dw[0] = PHYS(ring);
    ring[XHCI_XFER_RING_SIZE - 1u].dw[1] = 0;
    ring[XHCI_XFER_RING_SIZE - 1u].dw[2] = 0;
    ring[XHCI_XFER_RING_SIZE - 1u].dw[3] =
        TRB_CTRL_TYPE(TRB_LINK) | TRB_LINK_TC | g_ep0_pcs[si];

    xhci_ep_ctx_t *ep0 = &g_input_ctx.ep[0];
    ep0->dw[1] = EP_CTX_EP_TYPE(EP_TYPE_CTRL)
               | EP_CTX_CERR(3)
               | EP_CTX_MAX_PKT(ep0_mps);
    ep0->dw[2] = PHYS(ring) | EP_CTX_DCS;
    ep0->dw[3] = 0;
    ep0->dw[4] = 8u;   /* Average TRB length = 8 for control */
}

/*
 * setup_bulk_endpoints — add bulk IN/OUT to g_input_ctx after we know
 * the endpoint descriptors.
 * ep_out_addr / ep_in_addr: USB endpoint addresses (4-bit number part)
 * ep_mps: max packet size (512 for HS bulk)
 */
void xhci_configure_bulk_eps(u8 slot_id, u8 ep_out_num, u8 ep_in_num,
                              u16 ep_mps)
{
    /* Reuse input ctx — keep slot and EP0 contexts, add bulk EPs */
    /* A0=slot, A1=EP0, A2=EP1_OUT (ep_out_num→DBI2), A3=EP1_IN (ep_in_num→DBI3) */
    g_input_ctx.ctrl_dw[0] = 0;
    g_input_ctx.ctrl_dw[1] = ICTX_A0 | ICTX_A1
                            | ICTX_ADD(2) | ICTX_ADD(3);

    /* Slot context: update Context Entries to 3 */
    g_input_ctx.slot.dw[0] = (g_input_ctx.slot.dw[0] & ~(0x1Fu << 27))
                            | SLOT_CTX_ENTRIES(3);

    /* Init bulk OUT ring (EP context index 1 = DBI 2) */
    g_ep_out_enq = 0;
    g_ep_out_pcs = 1;
    g_ep_out_ring[XHCI_XFER_RING_SIZE - 1u].dw[0] = PHYS(g_ep_out_ring);
    g_ep_out_ring[XHCI_XFER_RING_SIZE - 1u].dw[1] = 0;
    g_ep_out_ring[XHCI_XFER_RING_SIZE - 1u].dw[2] = 0;
    g_ep_out_ring[XHCI_XFER_RING_SIZE - 1u].dw[3] =
        TRB_CTRL_TYPE(TRB_LINK) | TRB_LINK_TC | g_ep_out_pcs;

    xhci_ep_ctx_t *ep_out = &g_input_ctx.ep[1];
    xm_zero(ep_out, sizeof(*ep_out));
    ep_out->dw[1] = EP_CTX_EP_TYPE(EP_TYPE_BULK_OUT)
                  | EP_CTX_CERR(3)
                  | EP_CTX_MAX_PKT(ep_mps);
    ep_out->dw[2] = PHYS(g_ep_out_ring) | EP_CTX_DCS;
    ep_out->dw[4] = (u32)ep_mps;   /* Average TRB length = MPS */

    /* Init bulk IN ring (EP context index 2 = DBI 3) */
    g_ep_in_enq = 0;
    g_ep_in_pcs = 1;
    g_ep_in_ring[XHCI_XFER_RING_SIZE - 1u].dw[0] = PHYS(g_ep_in_ring);
    g_ep_in_ring[XHCI_XFER_RING_SIZE - 1u].dw[1] = 0;
    g_ep_in_ring[XHCI_XFER_RING_SIZE - 1u].dw[2] = 0;
    g_ep_in_ring[XHCI_XFER_RING_SIZE - 1u].dw[3] =
        TRB_CTRL_TYPE(TRB_LINK) | TRB_LINK_TC | g_ep_in_pcs;

    xhci_ep_ctx_t *ep_in = &g_input_ctx.ep[2];
    xm_zero(ep_in, sizeof(*ep_in));
    ep_in->dw[1] = EP_CTX_EP_TYPE(EP_TYPE_BULK_IN)
                 | EP_CTX_CERR(3)
                 | EP_CTX_MAX_PKT(ep_mps);
    ep_in->dw[2] = PHYS(g_ep_in_ring) | EP_CTX_DCS;
    ep_in->dw[4] = (u32)ep_mps;

    /* Issue Configure Endpoint command */
    xhci_trb_t ev = cmd_issue(
        PHYS(&g_input_ctx), 0, 0,
        TRB_CTRL_TYPE(TRB_CONFIG_EP) | TRB_CTRL_SLOT(slot_id));
    u32 cc = TRB_CC(ev.dw[2]);
    if (cc != TRB_CC_SUCCESS)
        kwarn("xHCI: Configure Endpoint CC=%u\n", (unsigned)cc);
    else
        kinfo("xHCI: bulk EPs configured (out_num=%u in_num=%u mps=%u)\n",
              (unsigned)ep_out_num, (unsigned)ep_in_num, (unsigned)ep_mps);
    (void)ep_out_num; (void)ep_in_num;
}

/*
 * xhci_configure_bulk_out_ep — configure a single OUT endpoint on a slot.
 * Used by the UAC1 audio driver to activate the isochronous OUT endpoint
 * (treated as bulk by our xHCI shim) after SET_INTERFACE alt=1.
 */
void xhci_configure_bulk_out_ep(u8 slot_id, u8 ep_out_num, u16 ep_mps)
{
    u8 dbi_out = 2u * ep_out_num;   /* OUT DBI = ep_num * 2 (even = OUT) */

    g_input_ctx.ctrl_dw[0] = 0;
    g_input_ctx.ctrl_dw[1] = ICTX_A0 | ICTX_ADD(dbi_out); /* no A1: don't overwrite EP0 with stale context */

    g_input_ctx.slot.dw[0] = (g_input_ctx.slot.dw[0] & ~(0x1Fu << 27))
                            | SLOT_CTX_ENTRIES(dbi_out);

    /* Init / reset the shared bulk OUT ring for this new owner */
    g_ep_out_enq = 0;
    g_ep_out_pcs = 1;
    g_ep_out_ring[XHCI_XFER_RING_SIZE - 1u].dw[0] = PHYS(g_ep_out_ring);
    g_ep_out_ring[XHCI_XFER_RING_SIZE - 1u].dw[1] = 0;
    g_ep_out_ring[XHCI_XFER_RING_SIZE - 1u].dw[2] = 0;
    g_ep_out_ring[XHCI_XFER_RING_SIZE - 1u].dw[3] =
        TRB_CTRL_TYPE(TRB_LINK) | TRB_LINK_TC | g_ep_out_pcs;

    xhci_ep_ctx_t *ep_out = &g_input_ctx.ep[dbi_out - 1u];
    xm_zero(ep_out, sizeof(*ep_out));
    ep_out->dw[1] = EP_CTX_EP_TYPE(EP_TYPE_BULK_OUT)
                  | EP_CTX_CERR(3)
                  | EP_CTX_MAX_PKT(ep_mps ? ep_mps : 192u);
    ep_out->dw[2] = PHYS(g_ep_out_ring) | EP_CTX_DCS;
    ep_out->dw[4] = ep_mps ? (u32)ep_mps : 192u;

    xhci_trb_t ev = cmd_issue(
        PHYS(&g_input_ctx), 0, 0,
        TRB_CTRL_TYPE(TRB_CONFIG_EP) | TRB_CTRL_SLOT(slot_id));
    u32 cc = TRB_CC(ev.dw[2]);
    if (cc != TRB_CC_SUCCESS)
        kwarn("xHCI: Configure OUT EP CC=%u slot=%u\n",
              (unsigned)cc, (unsigned)slot_id);
    else
        kinfo("xHCI: OUT endpoint configured ep=%u mps=%u slot=%u\n",
              (unsigned)ep_out_num, (unsigned)ep_mps, (unsigned)slot_id);
}

/* ── Config descriptor parser ────────────────────────────────────────────── */

typedef struct {
    u8  cfg_value;
    u8  iface_num;
    u8  iface_class;
    u8  iface_subclass;
    u8  iface_protocol;
    u8  ep_out_addr;   /* bulk OUT endpoint address (lower 4 bits) */
    u8  ep_in_addr;    /* bulk IN endpoint address (lower 4 bits) */
    u16 ep_mps;
    int found;
} cfg_info_t;

static void parse_config(const u8 *blob, u16 total, cfg_info_t *out)
{
    const u8 *p = blob, *end = blob + total;
    xm_zero(out, sizeof(*out));

    if (p >= end || p[1] != USB_DT_CONFIG) return;
    out->cfg_value = p[5];
    p += p[0];

    int in_iface = 0;
    while (p < end && p[0] >= 2u) {
        u8 dlen = p[0], dtype = p[1];
        if (dtype == USB_DT_INTERFACE) {
            out->iface_class    = p[5];
            out->iface_subclass = p[6];
            out->iface_protocol = p[7];
            out->iface_num      = p[2];
            in_iface = 1;
        } else if (dtype == USB_DT_ENDPOINT && in_iface) {
            u8 addr = p[2], attr = p[3];
            u16 mps = (u16)(p[4] | ((u16)p[5] << 8));
            if ((attr & 3u) == 2u) {   /* bulk */
                if (addr & 0x80u) {
                    out->ep_in_addr = addr & 0x0Fu;
                    out->ep_mps     = mps;
                } else {
                    out->ep_out_addr = addr & 0x0Fu;
                }
                if (out->ep_out_addr && out->ep_in_addr)
                    out->found = 1;
            }
        }
        p += dlen;
    }
}

/* ── Port enumeration & USB device setup ─────────────────────────────────── */

static int enumerate_port(u8 port0)
{
    u32 ps;

    /* Power on and reset the port */
    ps = port_rd(port0);
    if (!(ps & XHCI_PORT_PP)) {
        port_wr(port0, ps | XHCI_PORT_PP);
        mdelay(20);
    }

    /* Assert port reset */
    ps = port_rd(port0);
    port_wr(port0, (ps | XHCI_PORT_PR) & ~(XHCI_PORT_CSC | XHCI_PORT_PRC));
    mdelay(50);

    /* Wait for PR=0 and PRC=1 */
    if (port_wait(port0, XHCI_PORT_PR, 0, 200) < 0) {
        kwarn("xHCI: port %u reset timeout\n", (unsigned)port0);
        return -1;
    }

    /* Clear PRC W1C */
    ps = port_rd(port0);
    port_wr(port0, ps | XHCI_PORT_PRC);
    mdelay(2);

    ps = port_rd(port0);
    usb_speed_t speed = (usb_speed_t)((ps >> XHCI_PORT_SPD_SHIFT) & 0xFu);
    kinfo("xHCI: port %u enabled speed=%u ps=0x%x\n",
          (unsigned)port0, (unsigned)speed, (unsigned)ps);

    if (!(ps & XHCI_PORT_PED)) {
        kwarn("xHCI: port %u not enabled after reset\n", (unsigned)port0);
        return -1;
    }

    /* ── Enable Slot ── */
    xhci_trb_t ev = cmd_issue(0, 0, 0,
        TRB_CTRL_TYPE(TRB_ENABLE_SLOT));
    if (TRB_CC(ev.dw[2]) != TRB_CC_SUCCESS) {
        kwarn("xHCI: Enable Slot failed CC=%u\n",
              (unsigned)TRB_CC(ev.dw[2]));
        return -1;
    }
    u8 slot_id = (u8)TRB_EV_SLOT(ev.dw[3]);
    kinfo("xHCI: slot %u assigned\n", (unsigned)slot_id);

    /* Point DCBAA[slot_id] at this slot's output device context */
    u8 ctx_idx = (slot_id > 0u && slot_id <= XHCI_MAX_DEV_SLOTS)
                 ? slot_id - 1u : 0u;
    g_dcbaa[slot_id] = (u64)(u32)(uintptr_t)&g_dev_ctx[ctx_idx];
    DSB();

    /* ── Address Device (BSR=1: init slot+EP0 without assigning USB addr) ── */
    setup_ep0_context(slot_id, (u8)(port0 + 1u), speed, 8u);   /* assume MPS=8 initially */
    ev = cmd_issue(
        PHYS(&g_input_ctx), 0, 0,
        TRB_CTRL_TYPE(TRB_ADDRESS_DEVICE) | TRB_ADDR_BSR | TRB_CTRL_SLOT(slot_id));
    if (TRB_CC(ev.dw[2]) != TRB_CC_SUCCESS) {
        kwarn("xHCI: Address Device (BSR=1) CC=%u\n",
              (unsigned)TRB_CC(ev.dw[2]));
        return -1;
    }

    /* ── GET_DESCRIPTOR(Device, 8) to read bMaxPacketSize0 ── */
    usb_setup_t s = make_get_desc(USB_DT_DEVICE, 0, 8);
    if (xhci_ctrl_xfer(slot_id, &s, NULL) < 0) {
        kwarn("xHCI: GET_DESCRIPTOR(Dev/8) failed\n");
        return -1;
    }
    u8 ep0_mps = g_ctrl_buf[8 + 7];   /* bMaxPacketSize0 in ctrl_buf+8 (DATA phase) */
    if (!ep0_mps) ep0_mps = 8;

    u16 vid = (u16)(g_ctrl_buf[8 + 8]  | ((u16)g_ctrl_buf[8 + 9] << 8));
    u16 pid = (u16)(g_ctrl_buf[8 + 10] | ((u16)g_ctrl_buf[8 + 11] << 8));
    kinfo("xHCI: VID=0x%04x PID=0x%04x ep0_mps=%u\n",
          (unsigned)vid, (unsigned)pid, (unsigned)ep0_mps);

    /* ── Evaluate Context (update EP0 MPS if it changed) ── */
    if (ep0_mps != 8u) {
        xm_zero(&g_input_ctx, sizeof(g_input_ctx));
        g_input_ctx.ctrl_dw[1] = ICTX_A1;   /* only EP0 context */
        g_input_ctx.ep[0].dw[1] = EP_CTX_EP_TYPE(EP_TYPE_CTRL)
                                 | EP_CTX_CERR(3)
                                 | EP_CTX_MAX_PKT(ep0_mps);
        g_input_ctx.ep[0].dw[2] = PHYS(g_ep0_ring[ctx_idx]) | EP_CTX_DCS;
        g_input_ctx.ep[0].dw[4] = 8u;
        ev = cmd_issue(
            PHYS(&g_input_ctx), 0, 0,
            TRB_CTRL_TYPE(TRB_EVAL_CONTEXT) | TRB_CTRL_SLOT(slot_id));
        /* Non-fatal if it fails */
        (void)ev;
    }

    /* ── Address Device (BSR=0: actually assign USB address) ── */
    setup_ep0_context(slot_id, (u8)(port0 + 1u), speed, ep0_mps);
    ev = cmd_issue(
        PHYS(&g_input_ctx), 0, 0,
        TRB_CTRL_TYPE(TRB_ADDRESS_DEVICE) | TRB_CTRL_SLOT(slot_id));
    if (TRB_CC(ev.dw[2]) != TRB_CC_SUCCESS) {
        kwarn("xHCI: Address Device (BSR=0) CC=%u\n",
              (unsigned)TRB_CC(ev.dw[2]));
        return -1;
    }
    u8 usb_addr = (u8)(g_dev_ctx[ctx_idx].slot.dw[3] & 0xFFu);
    kinfo("xHCI: USB address = %u\n", (unsigned)usb_addr);

    /* ── GET_DESCRIPTOR(Config, 9) ── */
    s = make_get_desc(USB_DT_CONFIG, 0, 9);
    if (xhci_ctrl_xfer(slot_id, &s, NULL) < 0) {
        kwarn("xHCI: GET_DESCRIPTOR(Cfg/9) failed\n");
        return -1;
    }
    u16 total_len = (u16)(g_ctrl_buf[8 + 2] | ((u16)g_ctrl_buf[8 + 3] << 8));
    if (total_len < 9u || total_len > 240u) total_len = 240u;

    /* ── GET_DESCRIPTOR(Config, full) ── */
    s = make_get_desc(USB_DT_CONFIG, 0, total_len);
    if (xhci_ctrl_xfer(slot_id, &s, NULL) < 0) {
        kwarn("xHCI: GET_DESCRIPTOR(Cfg/full) failed\n");
        return -1;
    }

    /* Parse for bulk endpoints */
    cfg_info_t cfg;
    parse_config(g_ctrl_buf + 8, total_len, &cfg);
    if (!cfg.found) {
        /* No bulk EPs in default alt-setting (e.g. USB Audio class 1 with
         * isochronous endpoints in alt=1 only).  Issue SET_CONFIGURATION so
         * the device enters Configured state before class drivers send
         * SET_INTERFACE requests later.  cfg_value is at byte 5 of the
         * configuration descriptor. */
        u8 cfg_val = (g_ctrl_buf + 8)[5];
        if (!cfg_val) cfg_val = 1u;
        s = make_set_cfg(cfg_val);
        xhci_ctrl_xfer(slot_id, &s, NULL);   /* non-fatal if it fails */
        kinfo("xHCI: device has no bulk endpoints — not mass storage\n");
        return 0;  /* device enumerated but no MSC */
    }

    kinfo("xHCI: iface class=0x%02x sub=0x%02x proto=0x%02x ep_out=%u ep_in=%u mps=%u\n",
          (unsigned)cfg.iface_class, (unsigned)cfg.iface_subclass,
          (unsigned)cfg.iface_protocol,
          (unsigned)cfg.ep_out_addr, (unsigned)cfg.ep_in_addr,
          (unsigned)cfg.ep_mps);

    /* ── SET_CONFIGURATION ── */
    s = make_set_cfg(cfg.cfg_value);
    if (xhci_ctrl_xfer(slot_id, &s, NULL) < 0)
        kwarn("xHCI: SET_CONFIGURATION failed (non-fatal)\n");

    /* ── Configure bulk endpoints ── */
    xhci_configure_bulk_eps(slot_id, cfg.ep_out_addr, cfg.ep_in_addr,
                            cfg.ep_mps ? cfg.ep_mps : 512u);

    /* ── Probe for USB Mass Storage ── */
    if (cfg.iface_class == 0x08u &&
        cfg.iface_subclass == 0x06u &&
        cfg.iface_protocol == 0x50u) {
        usb_msc_probe(slot_id, cfg.ep_out_addr, cfg.ep_in_addr,
                      cfg.ep_mps ? cfg.ep_mps : 512u);
    } else {
        kinfo("xHCI: device is not USB MSC (class=0x%02x), ignored\n",
              (unsigned)cfg.iface_class);
    }

    return 0;
}

/* ── xhci_init ───────────────────────────────────────────────────────────── */

void xhci_init(void)
{
    /* 1. Find xHCI on PCI */
    pci_dev_t pci;
    xm_zero(&pci, sizeof(pci));
    if (!pci_scan_xhci(&pci)) {
        kwarn("xHCI: no controller found\n");
        return;
    }
    if (!pci.bar[0]) {
        kwarn("xHCI: BAR0 not assigned\n");
        return;
    }

    g_cap_base = pci.bar[0];

    /* 2. Read capability registers */
    u8  caplength  = (u8)(cap_rd(XHCI_CAP_CAPLENGTH) & 0xFF);
    u32 hccparams1 = cap_rd(XHCI_CAP_HCCPARAMS1);
    u32 hcsparams1 = cap_rd(XHCI_CAP_HCSPARAMS1);
    u32 dboff      = cap_rd(XHCI_CAP_DBOFF) & ~3u;
    u32 rtsoff     = cap_rd(XHCI_CAP_RTSOFF) & ~0x1Fu;

    g_op_base = g_cap_base + caplength;
    g_db_base = g_cap_base + dboff;
    g_rt_base = g_cap_base + rtsoff;
    g_max_ports = (u8)((hcsparams1 >> 24) & 0xFFu);
    g_ctx64     = (hccparams1 & XHCI_HCC1_CSZ) ? 1 : 0;

    kinfo("xHCI: cap=0x%lx op=0x%lx db=0x%lx rt=0x%lx ports=%u ctx64=%d\n",
          (unsigned long)g_cap_base, (unsigned long)g_op_base,
          (unsigned long)g_db_base,  (unsigned long)g_rt_base,
          (unsigned)g_max_ports, g_ctx64);

    /* 3. Wait for Controller Not Ready to clear */
    {
        u64 freq = cntfrq(); if (!freq) freq = 62500000ULL;
        u64 lim  = (freq / 1000ULL) * 1000ULL;
        u64 t0   = cntpct();
        while ((op_rd(XHCI_OP_USBSTS) & XHCI_STS_CNR) &&
               (cntpct() - t0) < lim)
            __asm__ volatile("nop");
    }
    if (op_rd(XHCI_OP_USBSTS) & XHCI_STS_CNR) {
        kwarn("xHCI: Controller Not Ready timeout\n");
        return;
    }

    /* 4. Stop HC if running */
    if (!(op_rd(XHCI_OP_USBSTS) & XHCI_STS_HCH)) {
        op_wr(XHCI_OP_USBCMD, op_rd(XHCI_OP_USBCMD) & ~XHCI_CMD_RS);
        mdelay(20);
    }

    /* 5. Host Controller Reset */
    op_wr(XHCI_OP_USBCMD, XHCI_CMD_HCRST);
    mdelay(50);
    {
        u64 freq = cntfrq(); if (!freq) freq = 62500000ULL;
        u64 lim  = (freq / 1000ULL) * 500ULL;
        u64 t0   = cntpct();
        while ((op_rd(XHCI_OP_USBCMD) & XHCI_CMD_HCRST) &&
               (cntpct() - t0) < lim)
            __asm__ volatile("nop");
    }
    if (op_rd(XHCI_OP_USBCMD) & XHCI_CMD_HCRST) {
        kwarn("xHCI: HCReset timeout\n");
        return;
    }
    mdelay(10);

    /* 6. Enable MaxSlotsEn */
    op_wr(XHCI_OP_CONFIG, XHCI_MAX_SLOTS);

    /* 7. Init DCBAA (all zeros, scratchpad entry 0 = null) */
    xm_zero(g_dcbaa, sizeof(g_dcbaa));
    op_wr(XHCI_OP_DCBAAP_LO, PHYS(g_dcbaa));
    op_wr(XHCI_OP_DCBAAP_HI, 0);

    /* 8. Command Ring: set up Link TRB at end, then write CRCR */
    xm_zero(g_cmd_ring, sizeof(g_cmd_ring));
    g_cmd_enq = 0;
    g_cmd_pcs = 1;
    /* Link TRB: points back to start, TC=1 so cycle flips on wrap */
    xhci_trb_t *link = &g_cmd_ring[XHCI_CMD_RING_SIZE - 1u];
    link->dw[0] = PHYS(g_cmd_ring);
    link->dw[1] = 0;
    link->dw[2] = 0;
    link->dw[3] = TRB_CTRL_TYPE(TRB_LINK) | TRB_LINK_TC | (u32)g_cmd_pcs;
    DSB();

    /* CRCR: ring address + RCS = initial producer cycle state (1) */
    op_wr(XHCI_OP_CRCR_LO, PHYS(g_cmd_ring) | XHCI_CRCR_RCS);
    op_wr(XHCI_OP_CRCR_HI, 0);

    /* 9. Event Ring */
    xm_zero(g_evt_ring, sizeof(g_evt_ring));
    g_evt_deq = 0;
    g_evt_ccs = 1;

    /* ERST: one segment */
    g_erst[0].seg_base_lo = PHYS(g_evt_ring);
    g_erst[0].seg_base_hi = 0;
    g_erst[0].seg_size    = XHCI_EVT_RING_SIZE;
    g_erst[0].reserved    = 0;
    DSB();

    rt_intr_wr(XHCI_INTR_ERSTSZ, 1u);
    rt_intr_wr(XHCI_INTR_ERDP_LO, PHYS(g_evt_ring) | XHCI_ERDP_EHB);
    rt_intr_wr(XHCI_INTR_ERDP_HI, 0);
    rt_intr_wr(XHCI_INTR_ERSTBA_LO, PHYS(g_erst));
    rt_intr_wr(XHCI_INTR_ERSTBA_HI, 0);
    /* IMAN: leave interrupts disabled (we poll) */

    /* 10. Start HC */
    op_wr(XHCI_OP_USBCMD, XHCI_CMD_RS | XHCI_CMD_HSEE);
    mdelay(20);
    if (op_rd(XHCI_OP_USBSTS) & XHCI_STS_HCH) {
        kwarn("xHCI: HC failed to start (still halted)\n");
        return;
    }
    kinfo("xHCI: controller running (USBSTS=0x%x)\n",
          (unsigned)op_rd(XHCI_OP_USBSTS));

    /* 11. Scan all ports — enumerate every connected device (up to XHCI_MAX_DEV_SLOTS) */
    u8 n = g_max_ports < XHCI_MAX_PORTS ? g_max_ports : XHCI_MAX_PORTS;
    u8 slots_used = 0;
    for (u8 i = 0; i < n; i++) {
        u32 ps = port_rd(i);
        if (!(ps & XHCI_PORT_CCS)) continue;
        kinfo("xHCI: device on port %u (ps=0x%x) — enumerating...\n",
              (unsigned)i, (unsigned)ps);
        if (enumerate_port(i) == 0) {
            g_ready = 1;
            if (++slots_used >= XHCI_MAX_DEV_SLOTS)
                break;   /* device context array full */
        }
    }

    if (!g_ready)
        kinfo("xHCI: no device enumerated\n");
}

int xhci_ready(void) { return g_ready; }
