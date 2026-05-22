/*
 * AetherOS — PWM / Software Audio Fallback (Phase 8.2)
 * File: kernel/drivers/audio/pwm_audio.c
 *
 * QEMU -M virt has no I2S or USB audio hardware.  This driver provides a
 * software-rendered audio device so the audio server can be tested without
 * real hardware.
 *
 * Architecture:
 *   A timer-driven polling function fills the capture_ring with silence
 *   (no ADC on QEMU) and advances write_idx at the configured sample rate.
 *   The AetherSound server (Phase 8.3) reads the capture ring, runs the
 *   DSP chain in userspace (float32), and writes processed PCM to the
 *   playback_ring.  The kernel reads the playback_ring and "plays" it
 *   (currently: drains the ring to maintain timing; real PWM output is a
 *   future enhancement using the BCM2712 PWM peripheral on Pi 5).
 *
 * No float operations in this file — all PCM is s16 integer.
 */

#include "aether/audio_dev.h"
#include "aether/sched.h"
#include "aether/printk.h"
#include "aether/types.h"
#include "drivers/timer/arm_timer.h"

/* ── Constants ───────────────────────────────────────────────────────── */

#define PWM_DEFAULT_SR     48000   /* default sample rate Hz             */
#define PWM_DEFAULT_CH     2       /* stereo                             */
#define PWM_PERIOD_FRAMES  64      /* default block size                 */

/* ── Backend private state ────────────────────────────────────────────── */

typedef struct {
    int   running;
    u64   next_tick;
    u32   ticks_per_period;
} pwm_priv_t;

static pwm_priv_t  g_priv;
static audio_dev_t g_pwm_dev;
static audio_dev_ops_t g_pwm_ops;

/* ── Ops ─────────────────────────────────────────────────────────────── */

static int pwm_configure(audio_dev_t *dev, u32 sample_rate,
                         u8 bit_depth, u8 channels)
{
    pwm_priv_t *p = (pwm_priv_t *)dev->priv;
    (void)bit_depth; (void)channels;

    u64 freq = timer_get_freq();
    if (!freq) freq = 62500000ULL;
    p->ticks_per_period = (u32)(freq * dev->period_frames / sample_rate);

    /* Init PCM rings */
    dev->capture_ring.write_idx  = 0;
    dev->capture_ring.read_idx   = 0;
    dev->capture_ring.capacity   = AUDIO_PCM_RING_FRAMES;
    dev->capture_ring.channels   = (u8)channels;
    dev->playback_ring.write_idx = 0;
    dev->playback_ring.read_idx  = 0;
    dev->playback_ring.capacity  = AUDIO_PCM_RING_FRAMES;
    dev->playback_ring.channels  = (u8)channels;

    kinfo("pwm_audio: configured %u Hz %u ch, period=%u frames\n",
          sample_rate, channels, dev->period_frames);
    return 0;
}

static int pwm_start(audio_dev_t *dev)
{
    pwm_priv_t *p = (pwm_priv_t *)dev->priv;
    p->running    = 1;
    p->next_tick  = timer_get_ticks() + p->ticks_per_period;
    kinfo("pwm_audio: started\n");
    return 0;
}

static int pwm_stop(audio_dev_t *dev)
{
    pwm_priv_t *p = (pwm_priv_t *)dev->priv;
    p->running    = 0;
    kinfo("pwm_audio: stopped\n");
    return 0;
}

static void pwm_close(audio_dev_t *dev) { (void)dev; }

/* ── Polling tick — called from audio_server poll loop ──────────────── */

void pwm_audio_poll(void)
{
    pwm_priv_t *p = (pwm_priv_t *)g_pwm_dev.priv;
    if (!p->running) return;

    u64 now = timer_get_ticks();
    if (now < p->next_tick) return;

    p->next_tick = now + p->ticks_per_period;
    u32 frames   = g_pwm_dev.period_frames;
    u32 ch       = g_pwm_dev.channels;

    /* Write silence (s16 zeros) to capture ring — no ADC on QEMU */
    audio_pcm_ring_t *cap = &g_pwm_dev.capture_ring;
    for (u32 f = 0; f < frames; f++) {
        for (u32 c = 0; c < ch; c++) {
            u32 wi = (cap->write_idx * ch + c) % (AUDIO_PCM_RING_FRAMES * ch);
            cap->pcm[wi] = 0;
        }
        cap->write_idx = (cap->write_idx + 1) % AUDIO_PCM_RING_FRAMES;
    }

    /* Drain playback ring (advancing read_idx to consume server output) */
    audio_pcm_ring_t *play = &g_pwm_dev.playback_ring;
    u32 available = (play->write_idx - play->read_idx + AUDIO_PCM_RING_FRAMES)
                    % AUDIO_PCM_RING_FRAMES;
    u32 consume = available < frames ? available : frames;
    play->read_idx = (play->read_idx + consume) % AUDIO_PCM_RING_FRAMES;

    /* Record callback timing for latency stats */
    sched_rt_record_callback(sched_rt_timestamp_ns(), 0);
}

/* ── Init ────────────────────────────────────────────────────────────── */

void pwm_audio_init(void)
{
    pwm_priv_t *p = &g_priv;
    p->running          = 0;
    p->next_tick        = 0;
    p->ticks_per_period = 0;

    g_pwm_ops.configure = pwm_configure;
    g_pwm_ops.start     = pwm_start;
    g_pwm_ops.stop      = pwm_stop;
    g_pwm_ops.close     = pwm_close;

    const char *nm = "PWM-soft";
    int i = 0;
    while (nm[i] && i < AUDIO_NAME_MAX - 1)
        { g_pwm_dev.info.name[i] = nm[i]; i++; }
    g_pwm_dev.info.name[i]         = '\0';
    g_pwm_dev.info.type            = AUDIO_DEV_PWM;
    g_pwm_dev.info.inputs          = 0;
    g_pwm_dev.info.outputs         = 2;
    g_pwm_dev.info.max_sample_rate = 96000;

    g_pwm_dev.ops           = &g_pwm_ops;
    g_pwm_dev.sample_rate   = PWM_DEFAULT_SR;
    g_pwm_dev.bit_depth     = 16;
    g_pwm_dev.channels      = PWM_DEFAULT_CH;
    g_pwm_dev.period_frames = PWM_PERIOD_FRAMES;
    g_pwm_dev.callback      = NULL;
    g_pwm_dev.callback_data = NULL;
    g_pwm_dev.priv          = p;

    /* Pre-configure rings at default rate */
    pwm_configure(&g_pwm_dev, PWM_DEFAULT_SR, 16, PWM_DEFAULT_CH);

    audio_core_register(&g_pwm_dev);
}
