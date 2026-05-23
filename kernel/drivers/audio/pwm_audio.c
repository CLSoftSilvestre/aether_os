/*
 * AetherOS — PWM / Software Audio Fallback (Phase 8.2)
 * File: kernel/drivers/audio/pwm_audio.c
 *
 * QEMU -M virt has no I2S or USB audio hardware.  This driver provides a
 * software-rendered audio device so the audio server can be tested without
 * real hardware.
 *
 * Architecture:
 *   A timer-driven polling function fills the capture_ring with a 440 Hz
 *   sine test tone so the DSP chain (AetherGuitar, AetherSound) has a real
 *   signal to process even without a USB audio interface connected.
 *   The AetherSound server (Phase 8.3) reads the capture ring, runs the
 *   DSP chain in userspace (float32), and writes processed PCM to the
 *   playback_ring.  The kernel reads the playback_ring and "plays" it
 *   (currently: drains the ring to maintain timing; real PWM output is a
 *   future enhancement using the BCM2712 PWM peripheral on Pi 5).
 *
 * Test tone:
 *   Recursive digital sine oscillator — y[n] = K·y[n-1] − y[n-2]
 *   K = 2·cos(2π·440/48000) in Q15 = 65422.  Amplitude −12 dBFS (8192).
 *   One s64 multiply per sample; no float, no lookup table.
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

/* ── 440 Hz test tone oscillator ─────────────────────────────────────── */
/* Recursive digital sine: y[n] = K*y[n-1] - y[n-2]
 * K  = 2*cos(2π*440/48000) in Q15 = 65422
 * y0 = -round(8192 * sin(2π*440/48000)) = -472  (primes the recurrence)
 * y1 = 0
 * Produces A*sin(n*ω) at A=8192 (-12 dBFS) once the first period runs. */
#define OSC_K_Q15  65422
#define OSC_AMP    8192

static s32 g_osc_y0 = -472;
static s32 g_osc_y1 =    0;

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

/* ── Direct fill — called from SYS_AUDIO_READ (userspace polling model) ── */
/* Generates exactly 'frames' samples into capture_ring regardless of timing.
 * Used when userspace drives the period rather than a kernel timer. */
void pwm_audio_fill(u32 frames)
{
    u32 ch  = g_pwm_dev.channels ? g_pwm_dev.channels : 2u;
    audio_pcm_ring_t *cap = &g_pwm_dev.capture_ring;

    for (u32 f = 0; f < frames; f++) {
        s32 y2 = (s32)(((s64)OSC_K_Q15 * g_osc_y1) >> 15) - g_osc_y0;
        g_osc_y0 = g_osc_y1;
        g_osc_y1 = y2;
        s16 sample = (s16)(y2 > 32767 ? 32767 : y2 < -32767 ? -32767 : y2);

        for (u32 c = 0; c < ch; c++) {
            u32 wi = (cap->write_idx * ch + c) % (AUDIO_PCM_RING_FRAMES * ch);
            cap->pcm[wi] = sample;
        }
        cap->write_idx = (cap->write_idx + 1) % AUDIO_PCM_RING_FRAMES;
    }
}

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

    /* Write 440 Hz sine test tone to capture ring (simulates guitar input).
     * Oscillator: y[n] = (OSC_K_Q15 * y[n-1] >> 15) - y[n-2].
     * The same sample goes to all channels — mono test signal. */
    audio_pcm_ring_t *cap = &g_pwm_dev.capture_ring;
    for (u32 f = 0; f < frames; f++) {
        s32 y2 = (s32)(((s64)OSC_K_Q15 * g_osc_y1) >> 15) - g_osc_y0;
        g_osc_y0 = g_osc_y1;
        g_osc_y1 = y2;
        s16 sample = (s16)(y2 > 32767 ? 32767 : y2 < -32767 ? -32767 : y2);

        for (u32 c = 0; c < ch; c++) {
            u32 wi = (cap->write_idx * ch + c) % (AUDIO_PCM_RING_FRAMES * ch);
            cap->pcm[wi] = sample;
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
    g_pwm_dev.info.inputs          = 2;   /* synthetic: 440 Hz test tone */
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
