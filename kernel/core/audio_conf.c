/*
 * AetherOS — Audio Configuration Subsystem
 * File: kernel/core/audio_conf.c
 */

#include "aether/audio_conf.h"
#include "aether/config.h"
#include "aether/printk.h"

/* ── Live state ──────────────────────────────────────────────────────────── */

audio_conf_t g_audio_conf;

/* ── Helpers ──────────────────────────────────────────────────────────────── */

static int valid_sample_rate(u32 sr)
{
    return sr == 44100 || sr == 48000 || sr == 96000;
}

static int valid_period(u16 p)
{
    return p == 64 || p == 128 || p == 256 || p == 512;
}

static int valid_depth(u8 d)
{
    return d == 16 || d == 24 || d == 32;
}

static void kstrncpy(char *dst, const char *src, int n)
{
    int i = 0;
    while (i < n - 1 && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

/* ── Init ──────────────────────────────────────────────────────────────────── */

void audio_conf_init(void)
{
    /* Sensible defaults */
    g_audio_conf.output_volume  = 80;
    g_audio_conf.input_gain     = 80;
    g_audio_conf.alert_volume   = 75;
    g_audio_conf.output_balance = 0;
    g_audio_conf.output_mute    = 0;
    g_audio_conf.sample_rate    = 48000;
    g_audio_conf.period_frames  = 64;
    g_audio_conf.bit_depth      = 16;
    g_audio_conf.output_dev[0]  = '\0';
    g_audio_conf.input_dev[0]   = '\0';

    char buf[16];

    if (kconfig_read("/config/audio.cfg", "output_volume", buf, 16) == 0) {
        int v = katoi(buf);
        if (v >= 0 && v <= 100) g_audio_conf.output_volume = (u8)v;
    }
    if (kconfig_read("/config/audio.cfg", "input_gain", buf, 16) == 0) {
        int v = katoi(buf);
        if (v >= 0 && v <= 100) g_audio_conf.input_gain = (u8)v;
    }
    if (kconfig_read("/config/audio.cfg", "alert_volume", buf, 16) == 0) {
        int v = katoi(buf);
        if (v >= 0 && v <= 100) g_audio_conf.alert_volume = (u8)v;
    }
    if (kconfig_read("/config/audio.cfg", "output_mute", buf, 16) == 0)
        g_audio_conf.output_mute = (u8)(katoi(buf) != 0);
    if (kconfig_read("/config/audio.cfg", "output_balance", buf, 16) == 0) {
        int v = katoi(buf);
        if (v >= -100 && v <= 100) g_audio_conf.output_balance = (s8)v;
    }
    if (kconfig_read("/config/audio.cfg", "sample_rate", buf, 16) == 0) {
        u32 sr = (u32)katoi(buf);
        if (valid_sample_rate(sr)) g_audio_conf.sample_rate = sr;
    }
    if (kconfig_read("/config/audio.cfg", "period_frames", buf, 16) == 0) {
        u16 p = (u16)katoi(buf);
        if (valid_period(p)) g_audio_conf.period_frames = p;
    }
    if (kconfig_read("/config/audio.cfg", "bit_depth", buf, 16) == 0) {
        u8 d = (u8)katoi(buf);
        if (valid_depth(d)) g_audio_conf.bit_depth = d;
    }
    kconfig_read("/config/audio.cfg", "output_dev",
                 g_audio_conf.output_dev, AUDIO_CONF_NAME_MAX);
    kconfig_read("/config/audio.cfg", "input_dev",
                 g_audio_conf.input_dev,  AUDIO_CONF_NAME_MAX);

    kinfo("audio_conf: vol=%u mute=%u bal=%d gain=%u sr=%u period=%u\n",
          g_audio_conf.output_volume, g_audio_conf.output_mute,
          (int)g_audio_conf.output_balance, g_audio_conf.input_gain,
          g_audio_conf.sample_rate, g_audio_conf.period_frames);
}

/* ── Get / Set ───────────────────────────────────────────────────────────── */

void audio_conf_get(audio_conf_t *out)
{
    if (!out) return;
    *out = g_audio_conf;
}

int audio_conf_set(const audio_conf_t *cfg)
{
    if (!cfg) return -1;
    if (cfg->output_volume > 100) return -1;
    if (cfg->input_gain   > 100) return -1;
    if (cfg->alert_volume > 100) return -1;
    if ((int)cfg->output_balance < -100 || (int)cfg->output_balance > 100)
        return -1;
    if (!valid_sample_rate(cfg->sample_rate))  return -1;
    if (!valid_period(cfg->period_frames))     return -1;
    if (!valid_depth(cfg->bit_depth))          return -1;

    g_audio_conf = *cfg;

    char buf[12];

    kitoa((int)cfg->output_volume,  buf, 12); kconfig_write("/config/audio.cfg", "output_volume",  buf);
    kitoa((int)cfg->input_gain,     buf, 12); kconfig_write("/config/audio.cfg", "input_gain",     buf);
    kitoa((int)cfg->alert_volume,   buf, 12); kconfig_write("/config/audio.cfg", "alert_volume",   buf);
    kitoa((int)cfg->output_mute,    buf, 12); kconfig_write("/config/audio.cfg", "output_mute",    buf);
    kitoa((int)cfg->output_balance, buf, 12); kconfig_write("/config/audio.cfg", "output_balance", buf);
    kitoa((int)cfg->sample_rate,    buf, 12); kconfig_write("/config/audio.cfg", "sample_rate",    buf);
    kitoa((int)cfg->period_frames,  buf, 12); kconfig_write("/config/audio.cfg", "period_frames",  buf);
    kitoa((int)cfg->bit_depth,      buf, 12); kconfig_write("/config/audio.cfg", "bit_depth",      buf);

    if (cfg->output_dev[0])
        kconfig_write("/config/audio.cfg", "output_dev", cfg->output_dev);
    if (cfg->input_dev[0])
        kconfig_write("/config/audio.cfg", "input_dev",  cfg->input_dev);

    return 0;
}
