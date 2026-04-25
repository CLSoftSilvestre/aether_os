/*
 * AetherOS — OHCI Host Controller + USB HID Boot Driver
 * File: kernel/include/drivers/usb/ohci.h
 *
 * Covers:
 *   - OHCI operational register offsets (relative to BAR0)
 *   - HCCA, Endpoint Descriptor, Transfer Descriptor structures
 *   - USB standard descriptor structs (device, config, interface, endpoint)
 *   - Public API: usb_hid_init(), usb_hid_poll()
 *
 * Reference: OHCI spec 1.0a; USB HID spec 1.11; QEMU hw/usb/hcd-ohci.c
 */

#ifndef AETHER_USB_OHCI_H
#define AETHER_USB_OHCI_H

#include "aether/types.h"

/* ── OHCI operational register offsets (from BAR0) ─────────────────────── */

#define OHCI_HcRevision          0x00
#define OHCI_HcControl           0x04
#define OHCI_HcCommandStatus     0x08
#define OHCI_HcInterruptStatus   0x0C
#define OHCI_HcInterruptEnable   0x10
#define OHCI_HcInterruptDisable  0x14
#define OHCI_HcHCCA              0x18   /* physical address of HCCA */
#define OHCI_HcPeriodCurrentED   0x1C
#define OHCI_HcControlHeadED     0x20
#define OHCI_HcControlCurrentED  0x24
#define OHCI_HcBulkHeadED        0x28
#define OHCI_HcBulkCurrentED     0x2C
#define OHCI_HcDoneHead          0x30
#define OHCI_HcFmInterval        0x34
#define OHCI_HcFmRemaining       0x38
#define OHCI_HcFmNumber          0x3C
#define OHCI_HcPeriodicStart     0x40
#define OHCI_HcLSThreshold       0x44
#define OHCI_HcRhDescriptorA     0x48
#define OHCI_HcRhDescriptorB     0x4C
#define OHCI_HcRhStatus          0x50
#define OHCI_HcRhPortStatus(n)   (0x54 + (n)*4)  /* n = 0-based port index */

/* HcControl bits */
#define OHCI_CTRL_CBSR     (3u <<  0)   /* Control Bulk Service Ratio */
#define OHCI_CTRL_PLE      (1u <<  2)   /* Periodic List Enable */
#define OHCI_CTRL_IE       (1u <<  3)   /* Isochronous Enable */
#define OHCI_CTRL_CLE      (1u <<  4)   /* Control List Enable */
#define OHCI_CTRL_BLE      (1u <<  5)   /* Bulk List Enable */
#define OHCI_CTRL_HCFS     (3u <<  6)   /* Host Controller Functional State */
#define OHCI_CTRL_HCFS_RST (0u <<  6)   /*   USB Reset */
#define OHCI_CTRL_HCFS_RES (1u <<  6)   /*   USB Resume */
#define OHCI_CTRL_HCFS_OPR (2u <<  6)   /*   USB Operational */
#define OHCI_CTRL_HCFS_SUS (3u <<  6)   /*   USB Suspend */

/* HcCommandStatus bits */
#define OHCI_CS_HCR   (1u <<  0)   /* Host Controller Reset */
#define OHCI_CS_CLF   (1u <<  1)   /* Control List Filled */
#define OHCI_CS_BLF   (1u <<  2)   /* Bulk List Filled */

/* HcRhPortStatus bits */
#define OHCI_PORT_CCS   (1u <<  0)   /* Current Connect Status */
#define OHCI_PORT_PES   (1u <<  1)   /* Port Enable Status */
#define OHCI_PORT_PSS   (1u <<  2)   /* Port Suspend Status */
#define OHCI_PORT_POCI  (1u <<  3)   /* Port Over Current Indicator */
#define OHCI_PORT_PRS   (1u <<  4)   /* Port Reset Status */
#define OHCI_PORT_PPS   (1u <<  8)   /* Port Power Status */
#define OHCI_PORT_LSDA  (1u <<  9)   /* Low Speed Device Attached */
#define OHCI_PORT_CSC   (1u << 16)   /* Connect Status Change */
#define OHCI_PORT_PESC  (1u << 17)   /* Port Enable Status Change */
#define OHCI_PORT_PRSC  (1u << 20)   /* Port Reset Status Change */

/* SetPortFeature selectors (written to HcRhPortStatus) */
#define OHCI_PORT_SET_POWER   (1u <<  8)
#define OHCI_PORT_SET_RESET   (1u <<  4)
#define OHCI_PORT_SET_ENABLE  (1u <<  1)
#define OHCI_PORT_CLR_RESET   (1u << 20)   /* ClearPortFeature C_PORT_RESET */

/* HcFmInterval reset value: FI=11999 (12000 bit-times per frame), FSMPS */
#define OHCI_FMINTERVAL_DEFAULT  0xA7782EDF

/* ── OHCI data structures ───────────────────────────────────────────────── */

/*
 * Host Controller Communications Area — 256-byte aligned.
 * OHCI reads HccaInterruptTable[] each frame to find ED chains for
 * periodic (interrupt) endpoints.  We use a single ED for both HID
 * devices and link it into all 32 interrupt-table slots.
 */
typedef struct {
    u32 interrupt_table[32];   /* physical addresses of period-1 ED chain */
    u16 frame_number;          /* current frame, written by HC */
    u16 pad1;
    u32 done_head;             /* physical address of last completed TD */
    u8  reserved[116];
} __attribute__((packed)) hcca_t;

/*
 * Endpoint Descriptor — 16-byte aligned.
 * HC reads this to find the TD ring for a given endpoint.
 */
typedef struct {
    u32 flags;    /* FA, EN, D, S, K, F, MPS */
    u32 tail_td;  /* physical address of dummy TD (tail sentinel) */
    u32 head_td;  /* physical address of first real TD | halted bit */
    u32 next_ed;  /* physical address of next ED in list (0 = end) */
} __attribute__((packed)) ohci_ed_t;

/* ED flags field helpers */
#define ED_FA(addr)  ((addr) & 0x7F)         /* Function Address (bits 0-6) */
#define ED_EN(n)     (((n) & 0xF) << 7)      /* Endpoint Number */
#define ED_DIR_TD    (0u << 11)              /* Direction from TD */
#define ED_DIR_OUT   (1u << 11)
#define ED_DIR_IN    (2u << 11)
#define ED_SPEED_FS  (0u << 13)              /* Full-speed */
#define ED_SPEED_LS  (1u << 13)              /* Low-speed */
#define ED_SKIP      (1u << 14)              /* HC skips this ED */
#define ED_FORMAT_GEN (0u << 15)             /* General (non-iso) */
#define ED_MPS(n)    (((n) & 0x7FF) << 16)  /* Max Packet Size */

/* head_td status bits */
#define ED_HEAD_HALTED  (1u << 0)
#define ED_HEAD_TOGGLE  (1u << 1)

/*
 * General Transfer Descriptor — 16-byte aligned.
 * Describes one USB transaction (SETUP, DATA IN/OUT, or STATUS).
 */
typedef struct {
    u32 flags;    /* CC, EC, T, DI, DP, R */
    u32 cbp;      /* Current Buffer Pointer (physical, 0 when done) */
    u32 next_td;  /* physical address of next TD */
    u32 be;       /* Buffer End (physical address of last byte) */
} __attribute__((packed)) ohci_td_t;

/* TD flags field helpers */
#define TD_CC_NOTACCESSED  (0xFu << 28)   /* Condition Code = NotAccessed */
#define TD_CC_MASK         (0xFu << 28)
#define TD_CC(flags)       (((flags) >> 28) & 0xF)
#define TD_CC_OK           0u

#define TD_T_DATA0   (2u << 24)   /* Force DATA0 */
#define TD_T_DATA1   (3u << 24)   /* Force DATA1 */
#define TD_T_TOGGLE  (0u << 24)   /* Use ED toggle carry */

#define TD_DI_NONE   (7u << 21)   /* No interrupt after completion */
#define TD_DI_IMM    (0u << 21)   /* Interrupt immediately */

#define TD_DP_SETUP  (0u << 19)
#define TD_DP_OUT    (1u << 19)
#define TD_DP_IN     (2u << 19)

#define TD_R         (1u << 18)   /* Buffer Rounding: allow short packet */

/* ── USB standard descriptors ──────────────────────────────────────────── */

typedef struct {
    u8  bLength;
    u8  bDescriptorType;
    u16 bcdUSB;
    u8  bDeviceClass;
    u8  bDeviceSubClass;
    u8  bDeviceProtocol;
    u8  bMaxPacketSize0;
    u16 idVendor;
    u16 idProduct;
    u16 bcdDevice;
    u8  iManufacturer;
    u8  iProduct;
    u8  iSerialNumber;
    u8  bNumConfigurations;
} __attribute__((packed)) usb_dev_desc_t;

typedef struct {
    u8  bLength;
    u8  bDescriptorType;
    u16 wTotalLength;
    u8  bNumInterfaces;
    u8  bConfigurationValue;
    u8  iConfiguration;
    u8  bmAttributes;
    u8  bMaxPower;
} __attribute__((packed)) usb_cfg_desc_t;

typedef struct {
    u8  bLength;
    u8  bDescriptorType;
    u8  bInterfaceNumber;
    u8  bAlternateSetting;
    u8  bNumEndpoints;
    u8  bInterfaceClass;
    u8  bInterfaceSubClass;
    u8  bInterfaceProtocol;
    u8  iInterface;
} __attribute__((packed)) usb_iface_desc_t;

typedef struct {
    u8  bLength;
    u8  bDescriptorType;
    u8  bEndpointAddress;   /* bit 7: 1=IN, 0=OUT; bits 3-0: EP number */
    u8  bmAttributes;       /* bits 1-0: 0=Control 1=Iso 2=Bulk 3=Interrupt */
    u16 wMaxPacketSize;
    u8  bInterval;          /* Polling interval in frames (1 = every frame) */
} __attribute__((packed)) usb_ep_desc_t;

/* USB descriptor type codes */
#define USB_DT_DEVICE     1
#define USB_DT_CONFIG     2
#define USB_DT_STRING     3
#define USB_DT_INTERFACE  4
#define USB_DT_ENDPOINT   5
#define USB_DT_HID        0x21
#define USB_DT_REPORT     0x22

/* USB standard request codes */
#define USB_REQ_GET_DESCRIPTOR   6
#define USB_REQ_SET_ADDRESS      5
#define USB_REQ_SET_CONFIGURATION 9
#define USB_REQ_SET_PROTOCOL     0x0B   /* HID class request */
#define USB_REQ_SET_IDLE         0x0A   /* HID class request */

/* bmRequestType */
#define USB_RT_HOST_TO_DEV   0x00
#define USB_RT_DEV_TO_HOST   0x80
#define USB_RT_CLASS_IF      0x21      /* Class, Interface recipient */

/* USB SETUP packet (8 bytes, always DATA0) */
typedef struct {
    u8  bmRequestType;
    u8  bRequest;
    u16 wValue;
    u16 wIndex;
    u16 wLength;
} __attribute__((packed)) usb_setup_t;

/* ── Public API ─────────────────────────────────────────────────────────── */

/*
 * usb_hid_init — called once from kernel_main() before IRQs are enabled.
 * Scans PCI for OHCI controller, resets it, enumerates root hub ports,
 * finds usb-kbd and usb-tablet, and starts polling interrupt EDs.
 */
void usb_hid_init(void);

/*
 * usb_hid_poll — called from timer_irq_handler() at 100 Hz.
 * Checks the interrupt TD completion flag for each HID device,
 * parses the report, posts events to kbd_ring / mouse_ring, and
 * rearms the TD for the next poll.
 */
void usb_hid_poll(void);

#endif /* AETHER_USB_OHCI_H */
