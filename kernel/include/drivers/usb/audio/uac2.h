/*
 * AetherOS — USB Audio Class 2.0 (UAC2) driver header (Phase 8.1)
 * File: kernel/include/drivers/usb/audio/uac2.h
 */

#ifndef AETHER_USB_UAC2_H
#define AETHER_USB_UAC2_H

#include "aether/types.h"

/* ── UAC2 USB Class / Subclass / Protocol ───────────────────────────── */
#define USB_CLASS_AUDIO         0x01
#define USB_SUBCLASS_AC         0x01   /* AudioControl interface         */
#define USB_SUBCLASS_AS         0x02   /* AudioStreaming interface        */
#define USB_SUBCLASS_MIDI       0x03   /* MIDIStreaming interface         */
#define USB_PROTOCOL_UAC2       0x20   /* UAC version 2                  */

/* ── AudioControl class-specific descriptor subtypes ────────────────── */
#define UAC2_CS_HEADER          0x01
#define UAC2_CS_INPUT_TERMINAL  0x02
#define UAC2_CS_OUTPUT_TERMINAL 0x03
#define UAC2_CS_FEATURE_UNIT    0x06
#define UAC2_CS_CLOCK_SOURCE    0x0A
#define UAC2_CS_CLOCK_SELECTOR  0x0B

/* ── AudioStreaming class-specific descriptor subtypes ───────────────── */
#define UAC2_AS_GENERAL         0x01
#define UAC2_AS_FORMAT_TYPE     0x02

/* ── Audio Format Type I ─────────────────────────────────────────────── */
#define UAC2_FORMAT_TYPE_I      0x01
#define UAC2_FORMAT_PCM         (1u << 0)   /* bmFormats bit 0 */

/* ── Class-specific request codes ───────────────────────────────────── */
#define UAC2_SET_CUR            0x01
#define UAC2_GET_CUR            0x81
#define UAC2_GET_MIN            0x82
#define UAC2_GET_MAX            0x83
#define UAC2_GET_RES            0x84

/* Selector codes for SET_CUR */
#define UAC2_CS_SAM_FREQ        0x01   /* Sampling Frequency Control     */
#define UAC2_CS_PITCH           0x02
#define UAC2_CX_CLOCK_VALID     0x02   /* Clock Valid control            */

/* ── Endpoint attributes ─────────────────────────────────────────────── */
#define USB_EP_ATTR_ISOCH       0x01
#define USB_EP_ATTR_ASYNC       0x04   /* async isochronous              */
#define USB_EP_ATTR_FEEDBACK    0x11   /* feedback endpoint              */

/* ── UMC202HD vendor/product IDs ─────────────────────────────────────── */
#define UMC202HD_VID            0x1397
#define UMC202HD_PID            0x0507

/* ── Public init (called from audio_core.c) ──────────────────────────── */
void uac2_init(void);

#endif /* AETHER_USB_UAC2_H */
