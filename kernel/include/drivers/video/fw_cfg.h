#ifndef DRIVERS_VIDEO_FW_CFG_H
#define DRIVERS_VIDEO_FW_CFG_H

/*
 * QEMU fw_cfg MMIO interface — used to configure the ramfb device.
 *
 * On QEMU -M virt, fw_cfg is at 0x09020000:
 *   +0x00  data register (1-byte r/w, streamed)
 *   +0x08  selector register (2-byte w, big-endian)
 *   +0x10  DMA high address (4-byte w, big-endian)
 *   +0x14  DMA low  address (4-byte w, big-endian, triggers DMA)
 */

#include "aether/types.h"

/* Find the selector key for a named fw_cfg file (e.g. "etc/ramfb").
 * Returns the key, or 0 if not found. */
u16 fwcfg_find_file(const char *name, u32 *size_out);

/* Write 'len' bytes from 'data' to the fw_cfg file at 'selector'. */
void fwcfg_write_file(u16 selector, const void *data, u32 len);

#endif /* DRIVERS_VIDEO_FW_CFG_H */
