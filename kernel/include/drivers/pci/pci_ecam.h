#ifndef AETHER_PCI_ECAM_H
#define AETHER_PCI_ECAM_H

#include "aether/types.h"

/*
 * Minimal PCIe ECAM driver for QEMU "-M virt,highmem=off".
 *
 * ECAM base: 0x3f000000 (256 MB window, bus 0 only)
 * PCIe MMIO: 0x10000000–0x3effffff (device BARs land here)
 *
 * Only bus 0, functions 0, is scanned — sufficient for QEMU virt.
 */

#define PCI_ECAM_BASE     0x3f000000UL
#define PCI_MMIO_BASE     0x10000000UL

/* PCI config-space register offsets */
#define PCI_VENDOR_ID     0x00
#define PCI_DEVICE_ID     0x02
#define PCI_COMMAND       0x04
#define PCI_STATUS        0x06
#define PCI_CAP_PTR       0x34
#define PCI_INT_LINE      0x3C
#define PCI_INT_PIN       0x3D
#define PCI_BAR0          0x10

#define PCI_CMD_MEM       (1u << 1)   /* Memory Space Enable */
#define PCI_CMD_MASTER    (1u << 2)   /* Bus Master Enable */
#define PCI_STS_CAP_LIST  (1u << 4)   /* Capabilities List present */

/* PCI capability IDs */
#define PCI_CAP_VNDR      0x09        /* Vendor-specific */

/* VirtIO PCI IDs */
#define VIRTIO_VENDOR     0x1AF4u
#define VIRTIO_DEV_INPUT  0x1052u     /* 0x1040 + device-id 18 */

/* Resolved PCI device descriptor */
typedef struct {
    u8        bus, dev, fn;
    uintptr_t bar[6];       /* resolved MMIO base per BAR (0 = unused) */
} pci_dev_t;

/* PCI config-space accessors */
u32  pci_read32 (u8 bus, u8 dev, u8 fn, u16 off);
u16  pci_read16 (u8 bus, u8 dev, u8 fn, u16 off);
u8   pci_read8  (u8 bus, u8 dev, u8 fn, u16 off);
void pci_write32(u8 bus, u8 dev, u8 fn, u16 off, u32 val);
void pci_write16(u8 bus, u8 dev, u8 fn, u16 off, u16 val);

/* Scan bus 0 for VirtIO input devices (vendor 0x1AF4, device 0x1052).
 * Enables Memory Space + Bus Master on each found device.
 * Returns number found (at most max_devs). */
int pci_scan_virtio_input(pci_dev_t *out, int max_devs);

/* Scan bus 0 for OHCI USB host controller (class=0x0C, sub=0x03, progIF=0x10).
 * Assigns BARs, enables Memory Space + Bus Master.
 * Returns 1 if found (r->bar[0] is the OHCI MMIO base), 0 if not found. */
int pci_scan_ohci(pci_dev_t *r);

#endif /* AETHER_PCI_ECAM_H */
