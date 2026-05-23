/*
 * AetherOS — AetherSound Audio Server (Phase 8.3)
 * File: userspace/apps/aether_sound/main.c
 *
 * The central audio routing daemon.  Started by init at boot.
 *
 * Responsibilities:
 *   1. Open the audio device (UAC2 preferred, I2S fallback, PWM for QEMU)
 *   2. Set SCHED_FIFO priority 80, CPU affinity core 3, mlockall
 *   3. Run a tight polling loop at the configured period rate
 *   4. Per period: read from kernel capture ring → float32 → audio chain
 *                  → float32 → kernel playback ring
 *
 * For Phase 8.3, the "audio chain" is a pass-through (input → output)
 * until Phase 8.4-8.7 implement the DSP libraries and Phase 8.8 builds
 * the full signal chain engine.
 *
 * The server prints detected audio devices at startup and logs xruns.
 */

#include <sys.h>
#include "aetheraudio.h"

#ifndef NULL
#define NULL ((void *)0)
#endif

/* ── Pass-through callback ───────────────────────────────────────────── */

static void passthrough_cb(float *in, float *out,
                            unsigned int frames, void *userdata)
{
    (void)userdata;
    for (unsigned int i = 0; i < frames * 2; i++)
        out[i] = in[i];
}

/* ── Helpers ─────────────────────────────────────────────────────────── */

static int strlenx(const char *s)
{
    int n = 0;
    while (s[n]) n++;
    return n;
}

static void print_str(const char *s)
{
    sys_write(1, s, strlenx(s));
}

static void enumerate_devices(void)
{
    audio_dev_info_t devs[8];
    int n = sys_audio_enum(devs, 8);

    print_str("[aether_sound] Audio devices:\n");
    for (int i = 0; i < n; i++) {
        print_str("  ");
        print_str(devs[i].name);
        print_str("\n");
    }
    if (n == 0)
        print_str("  (none found)\n");
}

/* ── Main ────────────────────────────────────────────────────────────── */

int main(void)
{
    print_str("[aether_sound] AetherSound server starting\n");

    enumerate_devices();

    /* Open audio client at 48 kHz, stereo, 64-sample period */
    audio_client_t *client = audio_client_open(48000, 2, 64);
    if (!client) {
        print_str("[aether_sound] ERROR: no audio device available\n");
        sys_exit(1);
    }

    audio_client_set_process_callback(client, passthrough_cb, NULL);

    if (audio_client_activate(client) < 0) {
        print_str("[aether_sound] ERROR: activate failed\n");
        sys_exit(1);
    }

    unsigned long long lat = audio_client_get_latency(client);
    /* Print latency: lat is in nanoseconds, divide to get ms × 10 */
    unsigned int lat_us = (unsigned int)(lat / 1000);
    (void)lat_us;   /* Phase 8.3: log this via a formatted print when available */

    print_str("[aether_sound] Running (48 kHz, 2ch, 64 frames, passthrough)\n");

    /* Main loop — polls at audio rate */
    while (1) {
        audio_client_run_once(client);
        /* Brief yield to avoid starving other tasks on single-core QEMU */
        sys_sched_yield();
    }

    /* unreachable */
    audio_client_close(client);
    return 0;
}
