/*
 * AetherOS — Audio Device Registry (Phase 8.1)
 * File: kernel/drivers/audio/audio_core.c
 *
 * Central registry for all audio devices.  Each backend (UAC2, I2S, PWM)
 * calls audio_core_register() to publish its device.  The audio server
 * then calls audio_dev_open() to claim a device.
 *
 * Device priority order (highest first):
 *   1. AUDIO_DEV_UAC2 — USB audio interface (best quality, lowest jitter)
 *   2. AUDIO_DEV_I2S  — onboard Pi 5 I2S (direct hardware, no USB overhead)
 *   3. AUDIO_DEV_PWM  — software PWM (QEMU, always available as fallback)
 */

#include "aether/audio_dev.h"
#include "aether/printk.h"
#include "aether/types.h"

/* ── Device table ────────────────────────────────────────────────────── */

#define AUDIO_MAX_DEVS 8

static audio_dev_t *g_devs[AUDIO_MAX_DEVS];
static int          g_ndevs = 0;

/* ── Registration ────────────────────────────────────────────────────── */

int audio_core_register(audio_dev_t *dev)
{
    if (!dev || g_ndevs >= AUDIO_MAX_DEVS) return -1;

    g_devs[g_ndevs++] = dev;
    kinfo("audio: registered '%s' (%s, %u ch in, %u ch out, max %u Hz)\n",
          dev->info.name,
          dev->info.type == AUDIO_DEV_UAC2 ? "UAC2" :
          dev->info.type == AUDIO_DEV_I2S  ? "I2S"  : "PWM",
          dev->info.inputs, dev->info.outputs, dev->info.max_sample_rate);
    return 0;
}

/* ── Open / close ────────────────────────────────────────────────────── */

audio_dev_t *audio_dev_open(const char *name)
{
    if (!name) return NULL;

    /* Exact name match */
    for (int i = 0; i < g_ndevs; i++) {
        const char *n = g_devs[i]->info.name;
        int j = 0;
        while (n[j] && name[j] && n[j] == name[j]) j++;
        if (!n[j] && !name[j])
            return g_devs[i];
    }

    /* "default" = highest-priority device (UAC2 > I2S > PWM) */
    const char *dflt = "default";
    int j = 0;
    while (dflt[j] && name[j] && dflt[j] == name[j]) j++;
    if (!dflt[j] && !name[j]) {
        /* Return device in priority order */
        for (audio_dev_type_t t = AUDIO_DEV_UAC2; t <= AUDIO_DEV_PWM; t++) {
            for (int i = 0; i < g_ndevs; i++) {
                if (g_devs[i]->info.type == t)
                    return g_devs[i];
            }
        }
    }

    return NULL;
}

void audio_dev_close(audio_dev_t *dev)
{
    /* Does not unregister — device stays available for next open */
    (void)dev;
}

/* ── Configuration ───────────────────────────────────────────────────── */

int audio_dev_configure(audio_dev_t *dev, u32 sample_rate,
                        u8 bit_depth, u8 channels)
{
    if (!dev || !dev->ops || !dev->ops->configure) return -1;
    if (!sample_rate || !channels) return -1;

    dev->sample_rate = sample_rate;
    dev->bit_depth   = bit_depth ? bit_depth : 32;
    dev->channels    = channels;

    return dev->ops->configure(dev, sample_rate, dev->bit_depth, channels);
}

int audio_dev_set_callback(audio_dev_t *dev, audio_callback_t cb,
                           void *userdata, u32 period_frames)
{
    if (!dev || !cb || !period_frames) return -1;
    dev->callback      = cb;
    dev->callback_data = userdata;
    dev->period_frames = period_frames;
    return 0;
}

int audio_dev_start(audio_dev_t *dev)
{
    if (!dev || !dev->ops || !dev->ops->start) return -1;
    if (!dev->callback) {
        kwarn("audio: start called with no callback on '%s'\n",
              dev->info.name);
        return -1;
    }
    return dev->ops->start(dev);
}

int audio_dev_stop(audio_dev_t *dev)
{
    if (!dev || !dev->ops || !dev->ops->stop) return -1;
    return dev->ops->stop(dev);
}

int audio_dev_enumerate(audio_dev_info_t *out, int max)
{
    int n = g_ndevs < max ? g_ndevs : max;
    for (int i = 0; i < n; i++)
        out[i] = g_devs[i]->info;
    return n;
}

/* ── Init ────────────────────────────────────────────────────────────── */

/* Forward declarations from backend drivers */
void uac2_init(void);
void pwm_audio_init(void);
void i2s_bcm2712_init(void);

void audio_core_init(void)
{
    g_ndevs = 0;
    kinfo("audio: core initialised\n");

    /* Register backends in priority order */
    uac2_init();         /* Phase 8.1: USB UAC2 (may register 0 or 1 device) */
    i2s_bcm2712_init();  /* Phase 8.2: I2S onboard (Pi 5 only)               */
    pwm_audio_init();    /* Phase 8.2: PWM fallback (always available)        */

    kinfo("audio: %d device(s) registered\n", g_ndevs);
}
