/*
 * PL031 Real-Time Clock Driver — AetherOS
 * File: kernel/drivers/rtc/pl031.c
 *
 * QEMU virt machine maps the PL031 RTC at 0x09010000.
 * QEMU backs the RTCDR register with the host clock, so reading it gives
 * real wall-clock time (in seconds since the Unix epoch when launched with
 * -rtc base=localtime, local time is returned directly).
 *
 * Register map (only RTCDR is needed for a read-only clock):
 *   +0x000  RTCDR  — Data Register: current time in seconds (read-only)
 *   +0x008  RTCLR  — Load Register: set time (write-only)
 *   +0x00C  RTCCR  — Control Register: bit 0 = enable
 */

#include "drivers/rtc/pl031.h"

#define PL031_BASE  0x09010000UL
#define RTCDR  (*(volatile u32 *)(PL031_BASE + 0x000))
#define RTCCR  (*(volatile u32 *)(PL031_BASE + 0x00C))

u32 pl031_read(void)
{
    RTCCR = 1;   /* ensure RTC is enabled (idempotent) */
    return RTCDR;
}
