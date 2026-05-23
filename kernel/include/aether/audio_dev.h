/*
 * AetherOS — Kernel Audio Device API (Phase 8.1)
 * File: kernel/include/aether/audio_dev.h
 *
 * Unified interface for all audio backends:
 *   - USB Audio Class 2.0 (UAC2) — UMC202HD, etc.
 *   - I2S BCM2712 (Pi 5 onboard)
 *   - PWM software renderer (QEMU -M virt fallback)
 *
 * Usage:
 *   1. audio_core_init() at boot — registers all detected devices
 *   2. audio_dev_open("default") to get a handle
 *   3. audio_dev_configure() to set sample rate / format
 *   4. audio_dev_set_callback() to install the RT audio callback
 *   5. audio_dev_start() — begins streaming, callback fires every period_frames
 *
 * The callback is called from IRQ/DMA context and MUST be lock-free.
 * No kmalloc, no printk, no task_yield inside the callback.
 */

#ifndef AETHER_AUDIO_DEV_H
#define AETHER_AUDIO_DEV_H

#include "aether/types.h"

/* ── Audio device types ─────────────────────────────────────────────── */

typedef enum {
    AUDIO_DEV_UAC2   = 0,   /* USB Audio Class 2.0 (xHCI isochronous)  */
    AUDIO_DEV_I2S    = 1,   /* BCM2712 I2S peripheral                  */
    AUDIO_DEV_PWM    = 2,   /* PWM software renderer (QEMU fallback)   */
} audio_dev_type_t;

/* ── PCM ring buffer — shared between kernel driver and audio server ─── */

/*
 * Raw 16-bit interleaved stereo PCM.  The kernel driver writes captured
 * samples here; the AetherSound server reads them, converts to float32,
 * runs the DSP chain, converts back to s16, and writes the output ring.
 *
 * No floats in the kernel: float32 DSP lives entirely in userspace
 * (AetherSound server, Phase 8.3), compiled without -mgeneral-regs-only.
 */
#define AUDIO_PCM_RING_FRAMES  1024    /* 1024 frames = 21 ms at 48 kHz */
#define AUDIO_PCM_MAX_CH       2

typedef struct {
    volatile u32  write_idx;           /* producer (kernel driver)       */
    volatile u32  read_idx;            /* consumer (audio server)        */
    u32           capacity;            /* ring capacity in frames        */
    u8            channels;
    u8            _pad[3];
    s16           pcm[AUDIO_PCM_RING_FRAMES * AUDIO_PCM_MAX_CH];
} audio_pcm_ring_t;

/* ── Callback — executed in the AetherSound server (userspace) ────────
 *
 * This typedef is shared between kernel headers (which store the pointer)
 * and userspace libs (which implement it).  The kernel NEVER calls this
 * pointer directly — it only stores it.  The AetherSound RT thread calls
 * it after reading raw PCM from the ring and converting to float32.
 *
 * in/out: deinterleaved float32 [channels × period_frames]
 */
typedef void (*audio_callback_t)(float *in, float *out,
                                 u32 frames, void *userdata);

/* ── Device info (for SYS_AUDIO_ENUM) ──────────────────────────────── */

#define AUDIO_NAME_MAX 32

typedef struct {
    char              name[AUDIO_NAME_MAX]; /* e.g. "UMC202HD", "BCM2712-I2S" */
    audio_dev_type_t  type;
    u8                inputs;               /* capture channels                */
    u8                outputs;              /* playback channels               */
    u32               max_sample_rate;
} audio_dev_info_t;

/* ── Opaque device handle ────────────────────────────────────────────── */

typedef struct audio_dev audio_dev_t;

/* ── Kernel-internal audio device vtable ────────────────────────────── */

typedef struct {
    int  (*configure)(audio_dev_t *dev, u32 sample_rate,
                      u8 bit_depth, u8 channels);
    int  (*start)(audio_dev_t *dev);
    int  (*stop)(audio_dev_t *dev);
    void (*close)(audio_dev_t *dev);
} audio_dev_ops_t;

struct audio_dev {
    audio_dev_info_t   info;
    audio_dev_ops_t   *ops;

    /* Configured parameters */
    u32                sample_rate;     /* Hz: 44100, 48000, 96000        */
    u8                 bit_depth;       /* 16, 24, or 32                  */
    u8                 channels;        /* 1 (mono) or 2 (stereo)         */
    u32                period_frames;   /* callback block size in frames  */

    /* Raw PCM rings — kernel writes capture, server writes playback.
     * NOTE: kernel code must NOT use the float callback pointer below.  */
    audio_pcm_ring_t   capture_ring;    /* kernel → server (ADC data)     */
    audio_pcm_ring_t   playback_ring;   /* server → kernel (DAC data)     */

    /* RT callback — stored here for the AetherSound server's reference.
     * The kernel NEVER calls this.  The server calls it after float conv. */
    audio_callback_t   callback;
    void              *callback_data;

    /* Backend-private state (UAC2 / I2S / PWM) */
    void              *priv;
};

/* ── audio_core.c public API ────────────────────────────────────────── */

/* Called from kernel_main after USB enumeration and I2S init.          */
void audio_core_init(void);

/* Register a device (called by each backend driver).                  */
int  audio_core_register(audio_dev_t *dev);

/* Open device by name ("default" = first available). Returns device.  */
audio_dev_t *audio_dev_open(const char *name);

/* Close handle (does not destroy the device; it stays registered).    */
void audio_dev_close(audio_dev_t *dev);

/* Configure sample rate, bit depth, channels. Must be called before start. */
int  audio_dev_configure(audio_dev_t *dev, u32 sample_rate,
                         u8 bit_depth, u8 channels);

/* Install RT callback. period_frames = block size (e.g. 64).          */
int  audio_dev_set_callback(audio_dev_t *dev, audio_callback_t cb,
                            void *userdata, u32 period_frames);

/* Begin streaming. Callback fires every period_frames samples.         */
int  audio_dev_start(audio_dev_t *dev);

/* Stop streaming. Callback will not be called after this returns.      */
int  audio_dev_stop(audio_dev_t *dev);

/* Enumerate registered devices into caller's array.                    */
int  audio_dev_enumerate(audio_dev_info_t *out, int max);

#endif /* AETHER_AUDIO_DEV_H */
