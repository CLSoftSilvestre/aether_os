/*
 * AetherOS — xHCI USB 3.0 Host Controller Driver
 * File: kernel/include/drivers/usb/xhci.h
 *
 * Supports:
 *   - USB 2.0 High-Speed (480 Mbit/s) and USB 3.0 SuperSpeed devices
 *   - Bulk, Control, Interrupt endpoints
 *   - USB Mass Storage (via msc.c) and USB HID (future)
 *
 * Reference: xHCI spec 1.2 (Intel); QEMU hw/usb/hcd-xhci.c
 * Strategy:  polling only (no IRQ wiring needed); synchronous transfers.
 */

#ifndef AETHER_USB_XHCI_H
#define AETHER_USB_XHCI_H

#include "aether/types.h"

/* ── PCI class codes ─────────────────────────────────────────────────────── */

#define PCI_CLASS_SERIAL_BUS  0x0Cu
#define PCI_SUBCLASS_USB      0x03u
#define PCI_PROGIF_XHCI       0x30u   /* xHCI programming interface */
#define PCI_PROGIF_OHCI       0x10u
#define PCI_PROGIF_EHCI       0x20u

/* ── xHCI Capability Register offsets (from BAR0) ───────────────────────── */

#define XHCI_CAP_CAPLENGTH    0x00   /* byte: length of capability regs */
#define XHCI_CAP_HCIVERSION   0x02   /* word: BCD version */
#define XHCI_CAP_HCSPARAMS1   0x04   /* max_slots[7:0], max_intrs[18:8], max_ports[31:24] */
#define XHCI_CAP_HCSPARAMS2   0x08   /* IST, ERST max, scratchpad bufs */
#define XHCI_CAP_HCSPARAMS3   0x0C
#define XHCI_CAP_HCCPARAMS1   0x10   /* AC64[0], CSZ[2], ... */
#define XHCI_CAP_DBOFF        0x14   /* doorbell array offset from cap base */
#define XHCI_CAP_RTSOFF       0x18   /* runtime registers offset from cap base */
#define XHCI_CAP_HCCPARAMS2   0x1C

/* HCCPARAMS1 bits */
#define XHCI_HCC1_AC64   (1u << 0)   /* 64-bit addressing capable */
#define XHCI_HCC1_CSZ    (1u << 2)   /* Context Size: 0=32B, 1=64B */

/* ── xHCI Operational Register offsets (from BAR0 + CAPLENGTH) ──────────── */

#define XHCI_OP_USBCMD    0x00
#define XHCI_OP_USBSTS    0x04
#define XHCI_OP_PAGESIZE  0x08
#define XHCI_OP_DNCTRL    0x14
#define XHCI_OP_CRCR_LO  0x18   /* Command Ring Control — low 32 bits  */
#define XHCI_OP_CRCR_HI  0x1C   /* Command Ring Control — high 32 bits */
#define XHCI_OP_DCBAAP_LO 0x30  /* Device Context Base Address Array Pointer lo */
#define XHCI_OP_DCBAAP_HI 0x34
#define XHCI_OP_CONFIG    0x38

/* Port registers base (from op base): 0x400 + port_idx * 0x10 */
#define XHCI_OP_PORTSC(n)    (0x400u + (u32)(n) * 0x10u)   /* n = 0-based */

/* USBCMD bits */
#define XHCI_CMD_RS    (1u << 0)   /* Run/Stop */
#define XHCI_CMD_HCRST (1u << 1)   /* Host Controller Reset */
#define XHCI_CMD_INTE  (1u << 2)   /* Interrupter Enable (not used — we poll) */
#define XHCI_CMD_HSEE  (1u << 3)   /* Host System Error Enable */
#define XHCI_CMD_EWE   (1u << 10)  /* Enable Wrap Event */

/* USBSTS bits */
#define XHCI_STS_HCH  (1u << 0)   /* HC Halted */
#define XHCI_STS_HSE  (1u << 2)   /* Host System Error */
#define XHCI_STS_EINT (1u << 3)   /* Event Interrupt */
#define XHCI_STS_PCD  (1u << 4)   /* Port Change Detect */
#define XHCI_STS_CNR  (1u << 11)  /* Controller Not Ready */

/* PORTSC bits */
#define XHCI_PORT_CCS  (1u << 0)   /* Current Connect Status */
#define XHCI_PORT_PED  (1u << 1)   /* Port Enabled/Disabled */
#define XHCI_PORT_PR   (1u << 4)   /* Port Reset */
#define XHCI_PORT_PLS  (0xFu << 5) /* Port Link State [8:5] */
#define XHCI_PORT_PP   (1u << 9)   /* Port Power */
#define XHCI_PORT_SPD  (0xFu << 10) /* Port Speed [13:10]: 1=FS 2=LS 3=HS 4=SS */
#define XHCI_PORT_CSC  (1u << 17)  /* Connect Status Change — W1C */
#define XHCI_PORT_PRC  (1u << 21)  /* Port Reset Change — W1C */
#define XHCI_PORT_WRC  (1u << 19)  /* Warm Reset Change — W1C */
#define XHCI_PORT_PEC  (1u << 18)  /* Port Enable/Disable Change — W1C */

#define XHCI_PORT_SPD_SHIFT  10
#define XHCI_PORT_SPD_FS  1u
#define XHCI_PORT_SPD_LS  2u
#define XHCI_PORT_SPD_HS  3u
#define XHCI_PORT_SPD_SS  4u

/* CRCR register: bit 0 = RCS (Ring Cycle State), bit 1 = CS, bit 2 = CA, bit 3 = CRR */
#define XHCI_CRCR_RCS (1u << 0)
#define XHCI_CRCR_CS  (1u << 1)   /* Command Stop */
#define XHCI_CRCR_CA  (1u << 2)   /* Command Abort */
#define XHCI_CRCR_CRR (1u << 3)   /* Command Ring Running (RO) */

/* ── Runtime Register offsets (from BAR0 + RTSOFF) ─────────────────────── */

#define XHCI_RT_MFINDEX  0x00
/* Interrupter n at RTSOFF + 0x20 + n * 0x20 */
#define XHCI_INTR_BASE    0x20
#define XHCI_INTR_STRIDE  0x20
#define XHCI_INTR_IMAN    0x00   /* Interrupt Management */
#define XHCI_INTR_IMOD    0x04   /* Interrupt Moderation */
#define XHCI_INTR_ERSTSZ  0x08   /* Event Ring Segment Table Size */
#define XHCI_INTR_ERSTBA_LO 0x10 /* ERST Base Address lo */
#define XHCI_INTR_ERSTBA_HI 0x14
#define XHCI_INTR_ERDP_LO   0x18 /* Event Ring Dequeue Pointer lo */
#define XHCI_INTR_ERDP_HI   0x1C

/* ERDP bit 3: Event Handler Busy — write 1 to clear EHB */
#define XHCI_ERDP_EHB  (1u << 3)

/* ── TRB (Transfer Request Block) — 16 bytes ────────────────────────────── */

typedef struct {
    u32 dw[4];   /* dw[0..1]=parameter, dw[2]=status, dw[3]=control */
} __attribute__((packed, aligned(16))) xhci_trb_t;

/* TRB type codes (bits[15:10] of control dword) */
#define TRB_NORMAL          1u
#define TRB_SETUP_STAGE     2u
#define TRB_DATA_STAGE      3u
#define TRB_STATUS_STAGE    4u
#define TRB_LINK            6u
#define TRB_ENABLE_SLOT     9u
#define TRB_DISABLE_SLOT    10u
#define TRB_ADDRESS_DEVICE  11u
#define TRB_CONFIG_EP       12u
#define TRB_EVAL_CONTEXT    13u
#define TRB_NOOP_CMD        23u
#define TRB_EV_TRANSFER     32u
#define TRB_EV_CMD_COMPLETE 33u
#define TRB_EV_PORT_CHANGE  34u

/* TRB control word helpers */
#define TRB_CTRL_CYCLE      (1u << 0)          /* Cycle Bit */
#define TRB_CTRL_ENT        (1u << 1)          /* Evaluate Next TRB */
#define TRB_CTRL_ISP        (1u << 2)          /* Interrupt on Short Packet */
#define TRB_CTRL_NS         (1u << 3)          /* No Snoop */
#define TRB_CTRL_CHAIN      (1u << 4)          /* Chain Bit */
#define TRB_CTRL_IOC        (1u << 5)          /* Interrupt on Completion */
#define TRB_CTRL_IDT        (1u << 6)          /* Immediate Data */
#define TRB_CTRL_TYPE(t)    ((u32)(t) << 10)   /* TRB Type field */
#define TRB_CTRL_SLOT(s)    ((u32)(s) << 24)   /* Slot ID [31:24] */
#define TRB_CTRL_EP(ep)     ((u32)(ep) << 16)  /* Endpoint ID [20:16] */

/* Setup Stage TRB: Transfer Type (TRT) bits [7:6] */
#define TRB_SETUP_TRT_NONE  (0u << 6)
#define TRB_SETUP_TRT_OUT   (2u << 6)
#define TRB_SETUP_TRT_IN    (3u << 6)

/* Data Stage: DIR bit [16] */
#define TRB_DATA_DIR_IN     (1u << 16)
#define TRB_DATA_DIR_OUT    (0u << 16)

/* BSR bit [9] in Address Device command */
#define TRB_ADDR_BSR        (1u << 9)

/* LINK TRB: Toggle Cycle bit [1] */
#define TRB_LINK_TC         (1u << 1)

/* Completion Code in Event TRBs: bits[31:24] of status dword */
#define TRB_CC(status)      (((status) >> 24) & 0xFFu)
#define TRB_CC_SUCCESS      1u
#define TRB_CC_DATA_ERR     2u
#define TRB_CC_BABBLE       3u
#define TRB_CC_XACT_ERR     4u
#define TRB_CC_TRB_ERR      5u
#define TRB_CC_STALL        6u
#define TRB_CC_SHORT_PKT    13u

/* Slot ID from Command Completion Event: bits[31:24] of control dword */
#define TRB_EV_SLOT(ctrl)   (((ctrl) >> 24) & 0xFFu)

/* Endpoint ID from Transfer Event: bits[20:16] of control dword */
#define TRB_EV_EPID(ctrl)   (((ctrl) >> 16) & 0x1Fu)

/* ── ERST (Event Ring Segment Table) entry — 16 bytes ───────────────────── */

typedef struct {
    u32 seg_base_lo;
    u32 seg_base_hi;
    u32 seg_size;      /* number of TRBs in this segment */
    u32 reserved;
} __attribute__((packed, aligned(64))) xhci_erst_entry_t;

/* ── Slot Context (32 bytes, CSZ=0) ────────────────────────────────────────*/

typedef struct {
    u32 dw[8];
} __attribute__((packed, aligned(32))) xhci_slot_ctx_t;

/* Slot context DW0 helpers */
#define SLOT_CTX_SPEED(s)    ((u32)(s) << 20)   /* bits[23:20]: USB Speed */
#define SLOT_CTX_ENTRIES(e)  ((u32)(e) << 27)   /* bits[31:27]: Context Entries */
#define SLOT_CTX_ROUTE(r)    ((u32)(r) & 0xFFFFFu) /* bits[19:0]: Route String */

/* Slot context DW1 helpers */
#define SLOT_CTX_PORT(p)     ((u32)(p) << 16)   /* bits[23:16]: Root Hub Port */

/* ── Endpoint Context (32 bytes, CSZ=0) ───────────────────────────────────*/

typedef struct {
    u32 dw[8];
} __attribute__((packed, aligned(32))) xhci_ep_ctx_t;

/* Endpoint type (bits[5:3] of DW1) */
#define EP_TYPE_CTRL      4u
#define EP_TYPE_ISOCH_OUT 1u
#define EP_TYPE_BULK_OUT  2u
#define EP_TYPE_INTR_OUT  3u
#define EP_TYPE_ISOCH_IN  5u
#define EP_TYPE_BULK_IN   6u
#define EP_TYPE_INTR_IN   7u

/* EP context DW1 */
#define EP_CTX_EP_TYPE(t)   ((u32)(t) << 3)     /* bits[5:3] */
#define EP_CTX_MAX_PKT(m)   ((u32)(m) << 16)    /* bits[31:16] Max Packet Size */
#define EP_CTX_CERR(c)      ((u32)(c) << 1)     /* bits[2:1] CErr (error count) */

/* EP context DW2: Transfer Ring dequeue pointer lo + DCS bit */
#define EP_CTX_DCS          (1u << 0)           /* Dequeue Cycle State */

/* EP context DW4: Average TRB Length bits[15:0] */

/* ── Input Context (1056 bytes: 32B control + 32B slot + 30×32B ep) ──────── */

typedef struct {
    u32            ctrl_dw[8];    /* Input Control Context: drop/add flags */
    xhci_slot_ctx_t slot;
    xhci_ep_ctx_t  ep[15];       /* ep[0]=EP0, ep[1]=EP1 OUT, ep[2]=EP1 IN... */
} __attribute__((aligned(64))) xhci_input_ctx_t;

/* Input Control Context: DW0=drop flags, DW1=add flags (bit N = slot/EP N) */
#define ICTX_ADD(n)    (1u << (n))   /* bit n in DW1: add context n */
#define ICTX_DROP(n)   (1u << (n))   /* bit n in DW0: drop context n */
#define ICTX_A0        ICTX_ADD(0)   /* add Slot Context */
#define ICTX_A1        ICTX_ADD(1)   /* add EP0 context */

/* ── Device Context: slot + up to 31 EP contexts ────────────────────────── */

typedef struct {
    xhci_slot_ctx_t slot;
    xhci_ep_ctx_t   ep[15];   /* ep[0]=EP0, ep[1..]=data endpoints */
} __attribute__((aligned(64))) xhci_dev_ctx_t;

/* ── xHCI endpoint DBI (Doorbell Index) — used when ringing doorbell ──────── */
/* EP0 Control = DBI 1, EP1 OUT = DBI 2, EP1 IN = DBI 3, EP2 OUT = DBI 4... */
#define XHCI_DBI_EP0       1u
#define XHCI_DBI_EP_OUT(n) (2u + (u32)(n) * 2u - 2u)   /* n=1..15, OUT=even */
#define XHCI_DBI_EP_IN(n)  (2u + (u32)(n) * 2u - 1u)   /* n=1..15, IN=odd  */

/* ── Ring sizes ─────────────────────────────────────────────────────────── */

#define XHCI_CMD_RING_SIZE   16u   /* command ring TRBs (last is Link) */
#define XHCI_EVT_RING_SIZE   32u   /* event ring TRBs  */
#define XHCI_XFER_RING_SIZE  32u   /* per-endpoint transfer ring TRBs */
#define XHCI_MAX_SLOTS        8u   /* max device slots we enable */
#define XHCI_MAX_PORTS        8u   /* max root hub ports to scan */

/* ── Endpoint IDs used by our driver (1-indexed as per xHCI spec) ─────────── */
/*
 * Control EP (EP0): ep_id = 1 (DBI 1)
 * Bulk OUT EP1:     ep_id = 2 (DBI 2)
 * Bulk IN  EP1:     ep_id = 3 (DBI 3)
 */

/* USB Speed (from PORTSC) */
typedef enum {
    USB_SPEED_FS = 1,   /* Full-Speed  12 Mbit/s */
    USB_SPEED_LS = 2,   /* Low-Speed   1.5 Mbit/s */
    USB_SPEED_HS = 3,   /* High-Speed  480 Mbit/s */
    USB_SPEED_SS = 4,   /* SuperSpeed  5 Gbit/s */
} usb_speed_t;

/* USB standard descriptor and request types (shared with ohci.h).
 * Guard against double-definition when ohci.h is included first. */
#ifndef USB_DT_DEVICE
#define USB_DT_DEVICE     1
#define USB_DT_CONFIG     2
#define USB_DT_STRING     3
#define USB_DT_INTERFACE  4
#define USB_DT_ENDPOINT   5

#define USB_REQ_GET_DESCRIPTOR    6
#define USB_REQ_SET_ADDRESS       5
#define USB_REQ_SET_CONFIGURATION 9
#define USB_REQ_SET_INTERFACE     11

#define USB_RT_HOST_TO_DEV  0x00
#define USB_RT_DEV_TO_HOST  0x80
#define USB_RT_CLASS_IF     0x21

typedef struct {
    u8  bmRequestType;
    u8  bRequest;
    u16 wValue;
    u16 wIndex;
    u16 wLength;
} __attribute__((packed)) usb_setup_t;
#endif /* USB_DT_DEVICE */

/* ── Public API ─────────────────────────────────────────────────────────── */

/*
 * xhci_init — scan PCI for xHCI controller, initialize it, enumerate
 * any connected USB device.  Calls usb_msc_probe() if a mass-storage
 * device is found.
 */
void xhci_init(void);

/*
 * xhci_ready — returns 1 after xhci_init() succeeded.
 */
int  xhci_ready(void);

/*
 * xhci_ctrl_xfer — issue one USB control transfer on the given slot's EP0.
 * Returns 0 on success, -1 on error/timeout.
 */
int  xhci_ctrl_xfer(u8 slot_id, const usb_setup_t *setup, void *data);

/*
 * xhci_bulk_xfer — issue a bulk IN or OUT transfer on the given slot/ep.
 * ep_id: endpoint context index (2=bulk-OUT, 3=bulk-IN per convention)
 * dbi:   doorbell index (same numbering as ep_id for bulk)
 * Returns bytes transferred on success, -1 on error.
 */
int  xhci_bulk_xfer(u8 slot_id, u8 ep_id, u8 dbi, void *buf,
                    u32 len, int dir_in);

/*
 * xhci_configure_bulk_out_ep — configure a single OUT endpoint on a slot.
 * Used by UAC1 to activate the isochronous OUT endpoint (treated as bulk).
 * ep_out_num: USB endpoint number (bits 3:0 of bEndpointAddress)
 * ep_mps:     max packet size in bytes (0 → default 192 for FS audio)
 */
void xhci_configure_bulk_out_ep(u8 slot_id, u8 ep_out_num, u16 ep_mps);

#endif /* AETHER_USB_XHCI_H */
