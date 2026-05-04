#ifndef DRIVERS_GPU_MAILBOX_H
#define DRIVERS_GPU_MAILBOX_H
/*
 * AetherOS — VideoCore ARM→GPU Mailbox (Phase 6.1.2)
 * File: kernel/include/drivers/gpu/mailbox.h
 *
 * The VideoCore mailbox property interface is used to:
 *   - Power on/off hardware blocks (V3D, display, etc.)
 *   - Query firmware version and board info
 *   - Get/set clock rates
 *
 * Supported hardware: BCM2711 (Raspberry Pi 4).
 * On QEMU -M virt there is no mailbox; all calls are no-ops returning -1.
 * Define AETHER_TARGET_PI4 in CMakeLists.txt for real-hardware builds.
 *
 * Protocol (channel 8, property interface):
 *   1. Fill a 16-byte-aligned buffer with tags + MBOX_TAG_END.
 *   2. Write (phys_addr | channel) to MBOX_REG_WRITE.
 *   3. Poll MBOX_REG_READ until response arrives on channel 8.
 *   4. Check buf[1] == MBOX_CODE_RESP_OK.
 */

#include "aether/types.h"

/* ── BCM2711 mailbox base ─────────────────────────────────────────────── */
#define MBOX_BASE_PI4     0xFE00B880UL   /* peripheral_base + 0xB880 */

/* ── Register offsets (byte offsets; divide by 4 for u32 array index) ─── */
#define MBOX_REG_READ     0x00
#define MBOX_REG_POLL     0x10
#define MBOX_REG_SENDER   0x14
#define MBOX_REG_STATUS   0x18
#define MBOX_REG_CONFIG   0x1C
#define MBOX_REG_WRITE    0x20

/* Status register flags */
#define MBOX_STATUS_FULL  (1u << 31)
#define MBOX_STATUS_EMPTY (1u << 30)

/* Channel 8 = ARM→VC property interface */
#define MBOX_CH_PROP  8u

/* ── Property buffer codes ────────────────────────────────────────────── */
#define MBOX_CODE_REQUEST   0x00000000u
#define MBOX_CODE_RESP_OK   0x80000000u
#define MBOX_CODE_RESP_ERR  0x80000001u

/* ── Tag IDs ──────────────────────────────────────────────────────────── */
#define MBOX_TAG_END              0x00000000u
#define MBOX_TAG_GET_FW_REV       0x00000001u
#define MBOX_TAG_GET_BOARD_MODEL  0x00010001u
#define MBOX_TAG_GET_BOARD_REV    0x00010002u
#define MBOX_TAG_GET_POWER_STATE  0x00028001u
#define MBOX_TAG_SET_POWER_STATE  0x00038001u
#define MBOX_TAG_GET_CLOCK_RATE   0x00030002u
#define MBOX_TAG_SET_CLOCK_RATE   0x00038002u

/* ── Device IDs (for SET/GET_POWER_STATE) ─────────────────────────────── */
#define MBOX_DEV_V3D  10u   /* V3D GPU on BCM2711 */

/* ── Clock IDs (for GET/SET_CLOCK_RATE) ──────────────────────────────── */
#define MBOX_CLK_V3D  5u

/* ── Power state flags ────────────────────────────────────────────────── */
#define MBOX_POWER_ON    (1u << 0)   /* set = on, clear = off */
#define MBOX_POWER_WAIT  (1u << 1)   /* block until transition completes */

/* ── Public API ───────────────────────────────────────────────────────── */

/* Call once before any other mailbox function. */
void mailbox_init(void);

/* Returns true if mailbox hardware was detected. */
bool mailbox_present(void);

/*
 * mailbox_call — submit a property request buffer on the given channel.
 * buf must be 16-byte aligned (the VideoCore accesses it via DMA).
 * Returns 0 on success, -1 on timeout or NACK.
 *
 * On real Pi hardware, flush the D-cache to buf before calling and
 * invalidate after — the VideoCore bypasses the CPU cache.  On QEMU
 * this is not needed (no real DMA).
 */
int mailbox_call(u32 channel, volatile u32 *buf);

/* Convenience wrappers built on mailbox_call(MBOX_CH_PROP, ...) */
u32 mailbox_get_firmware_rev(void);
int mailbox_set_power_state(u32 device_id, u32 flags);
u32 mailbox_get_power_state(u32 device_id);
u32 mailbox_get_clock_rate(u32 clock_id);

#endif /* DRIVERS_GPU_MAILBOX_H */
