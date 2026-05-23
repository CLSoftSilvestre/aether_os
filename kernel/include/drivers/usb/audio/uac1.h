/*
 * AetherOS — USB Audio Class 1.0 (UAC1) driver header (Phase 8.3)
 * File: kernel/include/drivers/usb/audio/uac1.h
 *
 * Targets QEMU's virtual usb-audio device (VID 0x46f4, PID 0x0002).
 * Provides playback-only output tapped from the PWM playback ring.
 */

#ifndef AETHER_USB_UAC1_H
#define AETHER_USB_UAC1_H

#include "aether/types.h"

/* ── UAC1 protocol identifier ────────────────────────────────────────── */
#define USB_PROTOCOL_UAC1       0x00   /* UAC version 1 (no protocol byte) */

/* ── UAC1 sampling frequency endpoint control ─────────────────────────── */
#define UAC1_SET_CUR            0x01
#define UAC1_SAMPLING_FREQ_CTRL 0x01   /* wValue high byte                 */

/* ── Public API ──────────────────────────────────────────────────────── */

/* uac1_init — scan xHCI slots for a UAC1 device and activate streaming.
 * Called from audio_core_init() after uac2_init(). */
void uac1_init(void);

/* uac1_write_pcm — forward interleaved s16 PCM to the UAC1 OUT endpoint.
 * buf:    interleaved s16 [L0,R0,L1,R1,...], frames×ch samples
 * frames: number of audio frames
 * ch:     channel count (2 for stereo)
 * No-op if no UAC1 device was found during uac1_init(). */
void uac1_write_pcm(const s16 *buf, u32 frames, u8 ch);

#endif /* AETHER_USB_UAC1_H */
