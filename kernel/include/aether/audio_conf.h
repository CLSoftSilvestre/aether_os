/*
 * AetherOS — Audio Configuration Subsystem
 * File: kernel/include/aether/audio_conf.h
 *
 * Persistent audio settings stored in /config/audio.conf on FAT32.
 * audio_conf_init() is called at boot (after fat32_mount).
 * audio_conf_set() validates, updates the live state, and persists.
 *
 * Live state (g_audio_conf) is read directly by SYS_AUDIO_READ and
 * SYS_AUDIO_WRITE to apply volume / mute / balance / input-gain in-kernel.
 */

#ifndef AETHER_AUDIO_CONF_H
#define AETHER_AUDIO_CONF_H

#include "aether/types.h"

#define AUDIO_CONF_NAME_MAX 32

typedef struct {
    char  output_dev[AUDIO_CONF_NAME_MAX]; /* preferred output device, "" = default */
    char  input_dev[AUDIO_CONF_NAME_MAX];  /* preferred input  device, "" = default */
    u32   sample_rate;     /* 44100 / 48000 / 96000 Hz                      */
    u16   period_frames;   /* DMA period: 64 / 128 / 256 / 512 frames       */
    u8    output_mute;     /* 0 = unmuted, 1 = muted                        */
    u8    output_volume;   /* 0-100 (applied as s16 scale in SYS_AUDIO_WRITE) */
    s8    output_balance;  /* -100 (full left) .. 0 (center) .. +100 (right) */
    u8    input_gain;      /* 0-100 (applied as s16 scale in SYS_AUDIO_READ) */
    u8    alert_volume;    /* 0-100 (UI / alert sounds, future use)          */
    u8    bit_depth;       /* 16 / 24 / 32                                  */
} audio_conf_t;

/* Live state — read by syscall.c READ/WRITE handlers; always valid. */
extern audio_conf_t g_audio_conf;

/* Called from kernel_main after audio_core_init() and fat32_mount. */
void audio_conf_init(void);

/* Fill *out with current live state. */
void audio_conf_get(audio_conf_t *out);

/*
 * Validate, apply, and persist cfg to /config/audio.conf.
 * Returns 0 on success, -1 if cfg is NULL or contains invalid values.
 */
int  audio_conf_set(const audio_conf_t *cfg);

#endif /* AETHER_AUDIO_CONF_H */
