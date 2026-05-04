/*
 * AetherOS — VideoCore ARM→GPU Mailbox (Phase 6.1.2)
 * File: kernel/drivers/gpu/mailbox.c
 *
 * On BCM2711 (Pi 4):
 *   Property tags are written into a 16-byte-aligned buffer, its physical
 *   address is OR'd with channel 8 and written to the mailbox WRITE register.
 *   The VideoCore fills the buffer with responses and signals completion.
 *
 * On QEMU -M virt (no hardware):
 *   g_present stays false; every public function is a safe no-op / -1.
 *
 * Cache coherency note for real Pi 4:
 *   The VideoCore DMA bypasses the ARM L1/L2 cache.  Before mailbox_call(),
 *   the ARM must clean (flush) the buffer to RAM so the VC sees fresh data.
 *   After the call, the ARM must invalidate before reading the response.
 *   On QEMU this is irrelevant — no real DMA occurs.
 *   TODO (Phase 6 hardware): add DC CVAC / DC CIVAC cache ops around the call.
 */

#include "drivers/gpu/mailbox.h"
#include "aether/printk.h"
#include "aether/types.h"

/* ── Hardware presence ────────────────────────────────────────────────── */

#ifdef AETHER_TARGET_PI4
static volatile u32 * const g_mbox = (volatile u32 *)MBOX_BASE_PI4;
static bool g_present = true;
#else
/* Dummy pointer — never dereferenced (g_present stays false) */
static volatile u32 * const g_mbox = (volatile u32 *)0UL;
static bool g_present = false;
#endif

/* ── 16-byte-aligned property exchange buffer ─────────────────────────── */
/*
 * Shared with the VideoCore via physical DMA.  Must remain 16-byte aligned
 * so the lower nibble is free for the channel number in the WRITE register.
 * 64 u32s = 256 bytes — enough for all single-tag property calls we issue.
 */
static volatile u32 __attribute__((aligned(16))) g_buf[64];

/* ── Init ─────────────────────────────────────────────────────────────── */

void mailbox_init(void)
{
#ifdef AETHER_TARGET_PI4
    kinfo("[MBOX] BCM2711 mailbox at 0x%08x\n", (unsigned)MBOX_BASE_PI4);
#else
    kinfo("[MBOX] no Pi 4 target — mailbox stubbed (QEMU mode)\n");
#endif
}

bool mailbox_present(void) { return g_present; }

/* ── Low-level call ───────────────────────────────────────────────────── */

int mailbox_call(u32 channel, volatile u32 *buf)
{
    if (!g_present) return -1;

    uintptr_t phys = (uintptr_t)buf;
    if (phys & 0xFu) {
        kwarn("[MBOX] buffer not 16-byte aligned (0x%lx)\n",
              (unsigned long)phys);
        return -1;
    }

    /* Wait until WRITE register not full */
    u32 timeout = 0x100000u;
    while ((g_mbox[MBOX_REG_STATUS / 4] & MBOX_STATUS_FULL) && --timeout)
        __asm__ volatile("nop" ::: "memory");
    if (!timeout) { kwarn("[MBOX] write timeout\n"); return -1; }

    /* Submit: physical address with lower nibble = channel */
    g_mbox[MBOX_REG_WRITE / 4] = (u32)(phys & ~0xFu) | (channel & 0xFu);

    /* Wait for response on our channel */
    timeout = 0x100000u;
    for (;;) {
        while ((g_mbox[MBOX_REG_STATUS / 4] & MBOX_STATUS_EMPTY) && --timeout)
            __asm__ volatile("nop" ::: "memory");
        if (!timeout) { kwarn("[MBOX] read timeout\n"); return -1; }

        u32 resp = g_mbox[MBOX_REG_READ / 4];
        if ((resp & 0xFu) == channel)
            return (buf[1] == MBOX_CODE_RESP_OK) ? 0 : -1;
    }
}

/* ── Property interface helpers ───────────────────────────────────────── */

u32 mailbox_get_firmware_rev(void)
{
    if (!g_present) return 0;

    /* Buffer layout:
     *   [0] total size (7 words = 28 bytes)
     *   [1] request code
     *   [2] tag id
     *   [3] value buffer size (4 bytes)
     *   [4] request indicator (0)
     *   [5] value (filled by VC)
     *   [6] end tag
     */
    g_buf[0] = 7 * 4;
    g_buf[1] = MBOX_CODE_REQUEST;
    g_buf[2] = MBOX_TAG_GET_FW_REV;
    g_buf[3] = 4;
    g_buf[4] = 0;
    g_buf[5] = 0;
    g_buf[6] = MBOX_TAG_END;

    if (mailbox_call(MBOX_CH_PROP, g_buf) < 0) return 0;
    return g_buf[5];
}

int mailbox_set_power_state(u32 device_id, u32 flags)
{
    if (!g_present) return -1;

    g_buf[0] = 9 * 4;
    g_buf[1] = MBOX_CODE_REQUEST;
    g_buf[2] = MBOX_TAG_SET_POWER_STATE;
    g_buf[3] = 8;        /* two u32 values */
    g_buf[4] = 0;
    g_buf[5] = device_id;
    g_buf[6] = flags;
    g_buf[7] = 0;        /* VC fills with result state */
    g_buf[8] = MBOX_TAG_END;

    return mailbox_call(MBOX_CH_PROP, g_buf);
}

u32 mailbox_get_power_state(u32 device_id)
{
    if (!g_present) return 0;

    g_buf[0] = 8 * 4;
    g_buf[1] = MBOX_CODE_REQUEST;
    g_buf[2] = MBOX_TAG_GET_POWER_STATE;
    g_buf[3] = 8;
    g_buf[4] = 0;
    g_buf[5] = device_id;
    g_buf[6] = 0;
    g_buf[7] = MBOX_TAG_END;

    if (mailbox_call(MBOX_CH_PROP, g_buf) < 0) return 0;
    return g_buf[6];   /* bits[0]=powered, bits[1]=device_exists */
}

u32 mailbox_get_clock_rate(u32 clock_id)
{
    if (!g_present) return 0;

    g_buf[0] = 9 * 4;
    g_buf[1] = MBOX_CODE_REQUEST;
    g_buf[2] = MBOX_TAG_GET_CLOCK_RATE;
    g_buf[3] = 8;
    g_buf[4] = 0;
    g_buf[5] = clock_id;
    g_buf[6] = 0;      /* VC fills with rate in Hz */
    g_buf[7] = 0;
    g_buf[8] = MBOX_TAG_END;

    if (mailbox_call(MBOX_CH_PROP, g_buf) < 0) return 0;
    return g_buf[6];
}
