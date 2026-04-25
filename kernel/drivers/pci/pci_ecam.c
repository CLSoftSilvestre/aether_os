/*
 * AetherOS — PCIe ECAM driver (bus 0 only)
 * File: kernel/drivers/pci/pci_ecam.c
 *
 * QEMU "-M virt,highmem=off" places the ECAM window at 0x3f000000.
 * Config space address: ECAM_BASE | (bus<<20) | (dev<<15) | (fn<<12) | off
 *
 * On ARM bare metal there is no PCI BIOS to assign BAR addresses, so we
 * probe each BAR for its required size and assign addresses ourselves from
 * the PCIe MMIO window (0x10000000 – 0x3effffff).
 */

#include "drivers/pci/pci_ecam.h"
#include "aether/printk.h"

/* ── Config-space accessors ─────────────────────────────────────────────── */

static volatile u32 *ecam_word(u8 bus, u8 dev, u8 fn, u16 off)
{
    uintptr_t a = PCI_ECAM_BASE
                | ((u32)bus << 20)
                | ((u32)dev << 15)
                | ((u32)fn  << 12)
                | (off & 0xFFCu);
    return (volatile u32 *)a;
}

u32 pci_read32(u8 bus, u8 dev, u8 fn, u16 off)
{
    return *ecam_word(bus, dev, fn, off);
}

u16 pci_read16(u8 bus, u8 dev, u8 fn, u16 off)
{
    u32 v = pci_read32(bus, dev, fn, off & ~3u);
    return (u16)(v >> ((off & 2u) * 8u));
}

u8 pci_read8(u8 bus, u8 dev, u8 fn, u16 off)
{
    u32 v = pci_read32(bus, dev, fn, off & ~3u);
    return (u8)(v >> ((off & 3u) * 8u));
}

void pci_write32(u8 bus, u8 dev, u8 fn, u16 off, u32 val)
{
    *ecam_word(bus, dev, fn, off) = val;
}

void pci_write16(u8 bus, u8 dev, u8 fn, u16 off, u16 val)
{
    volatile u32 *p = ecam_word(bus, dev, fn, off & ~3u);
    u32 sh = (off & 2u) * 8u;
    *p = (*p & ~(0xFFFFu << sh)) | ((u32)val << sh);
}

/* ── BAR assignment ──────────────────────────────────────────────────────── */
/*
 * We allocate BAR addresses sequentially from the PCIe MMIO window.
 * Each BAR is aligned to its own size (PCI requirement).
 * QEMU's PCIe MMIO window starts at 0x10000000, safe up to 0x3f000000.
 */
static uintptr_t g_mmio_next = PCI_MMIO_BASE;

/*
 * Probe and assign a single BAR.  Returns the assigned MMIO base address,
 * or 0 if the BAR is unused (returns 0 from size probe).
 * Advances g_mmio_next past the allocated region.
 * For 64-bit BARs, *skip_next is set to 1 so the caller skips BAR idx+1.
 */
static uintptr_t assign_bar(u8 bus, u8 dev, u8 fn, int idx, int *skip_next)
{
    u16 reg = (u16)(PCI_BAR0 + idx * 4);
    u32 orig = pci_read32(bus, dev, fn, reg);

    if (orig & 1u) return 0;   /* I/O space BAR — ignore on ARM */

    int is64 = (((orig >> 1) & 3u) == 2u);

    /* For 64-bit BARs, mark the next slot as the upper half NOW — before the
     * size probe can return early.  If we wait until after the probe and the
     * BAR reports size=0 (not implemented), we'd return without ever setting
     * skip_next, leaving BAR[idx+1] to be scanned as an independent BAR even
     * though it is just the upper 32 bits of this one. */
    if (is64 && idx < 5) {
        pci_write32(bus, dev, fn, (u16)(reg + 4), 0u);
        *skip_next = 1;
    }

    /* Size probe: write all-ones, read back, restore */
    pci_write32(bus, dev, fn, reg, 0xFFFFFFFFu);
    u32 sz_lo = pci_read32(bus, dev, fn, reg) & ~0xFu;
    pci_write32(bus, dev, fn, reg, orig);

    if (sz_lo == 0u || sz_lo == 0xFFFFFFF0u) return 0;  /* BAR not implemented */

    /* Size = lowest set bit (PCI BAR size encoding) */
    u32 bar_size = ~sz_lo + 1u;

    /* Align g_mmio_next to bar_size */
    uintptr_t addr = (g_mmio_next + (uintptr_t)bar_size - 1u)
                   & ~((uintptr_t)bar_size - 1u);
    g_mmio_next = addr + (uintptr_t)bar_size;

    /* Write assigned address */
    pci_write32(bus, dev, fn, reg, (u32)addr);

    return addr;
}

/* ── Public scan ─────────────────────────────────────────────────────────── */

int pci_scan_virtio_input(pci_dev_t *out, int max_devs)
{
    int found = 0;

    for (u8 d = 0; d < 32u && found < max_devs; d++) {
        u16 vendor = pci_read16(0, d, 0, PCI_VENDOR_ID);
        if (vendor == 0xFFFFu) continue;

        u16 device = pci_read16(0, d, 0, PCI_DEVICE_ID);
        if (vendor != VIRTIO_VENDOR || device != VIRTIO_DEV_INPUT)
            continue;

        pci_dev_t *r = &out[found++];
        r->bus = 0; r->dev = d; r->fn = 0;

        /* Assign BARs */
        for (int b = 0; b < 6; ) {
            int skip = 0;
            r->bar[b] = assign_bar(0, d, 0, b, &skip);
            b += skip ? 2 : 1;
        }

        /* Enable Memory Space + Bus Master after BAR assignment */
        u16 cmd = pci_read16(0, d, 0, PCI_COMMAND);
        pci_write16(0, d, 0, PCI_COMMAND,
                    (u16)(cmd | PCI_CMD_MEM | PCI_CMD_MASTER));

        kinfo("PCI: VirtIO input at 00:%x.0  bars:", d);
        for (int b = 0; b < 6; b++)
            if (r->bar[b])
                kinfo(" [%d]=0x%lx", b, (unsigned long)r->bar[b]);
        kinfo("\n");
    }

    return found;
}

int pci_scan_ohci(pci_dev_t *r)
{
    for (u8 d = 0; d < 32u; d++) {
        u16 vendor = pci_read16(0, d, 0, PCI_VENDOR_ID);
        if (vendor == 0xFFFFu) continue;

        u8 class_code = pci_read8(0, d, 0, 0x0Bu);
        u8 subclass   = pci_read8(0, d, 0, 0x0Au);
        u8 prog_if    = pci_read8(0, d, 0, 0x09u);

        if (class_code != 0x0Cu || subclass != 0x03u || prog_if != 0x10u)
            continue;

        r->bus = 0; r->dev = d; r->fn = 0;

        for (int b = 0; b < 6; ) {
            int skip = 0;
            r->bar[b] = assign_bar(0, d, 0, b, &skip);
            b += skip ? 2 : 1;
        }

        u16 cmd = pci_read16(0, d, 0, PCI_COMMAND);
        pci_write16(0, d, 0, PCI_COMMAND,
                    (u16)(cmd | PCI_CMD_MEM | PCI_CMD_MASTER));

        kinfo("PCI: OHCI USB at 00:%x.0 bar[0]=0x%lx\n",
              (unsigned)d, (unsigned long)r->bar[0]);

        return 1;
    }
    return 0;
}
