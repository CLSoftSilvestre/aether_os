/*
 * AetherOS — libaetheraudio: Audio Client Library (Phase 8.3)
 * File: userspace/lib/libaetheraudio/include/aetheraudio.h
 *
 * Public API for applications that want to process audio.
 * Built on top of the AetherSound server (aether_sound daemon).
 *
 * Usage:
 *   1. audio_client_open(48000, 2, 64) — connect to audio server
 *   2. audio_client_set_process_callback(my_callback, userdata)
 *   3. audio_client_activate() — start receiving audio callbacks
 *   4. [run event loop]
 *   5. audio_client_deactivate() + audio_client_close()
 *
 * The process callback is called at 48 kHz / 64-sample intervals (~750 Hz).
 * It MUST be lock-free: no malloc, no printk, no blocking calls.
 * All parameters are float32 deinterleaved: in[ch][frame], out[ch][frame].
 */

#ifndef AETHER_AUDIO_H
#define AETHER_AUDIO_H

/* ── Callback types ──────────────────────────────────────────────────── */

/*
 * Process callback — the audio hot path.
 * in:  deinterleaved input  [channels × frames] float32
 * out: deinterleaved output [channels × frames] float32
 */
typedef void (*audio_process_cb_t)(float *in, float *out,
                                   unsigned int frames, void *userdata);

/* ── Opaque client handle ────────────────────────────────────────────── */

typedef struct audio_client audio_client_t;

/* ── API ─────────────────────────────────────────────────────────────── */

/*
 * Open connection to the audio server.
 * sample_rate: 44100, 48000, or 96000 Hz
 * channels:    1 (mono) or 2 (stereo)
 * period_frames: buffer size per callback (64 = lowest latency, 256 = safer)
 * Returns NULL on error.
 */
audio_client_t *audio_client_open(unsigned int sample_rate,
                                   unsigned int channels,
                                   unsigned int period_frames);

/* Close and free the client. */
void audio_client_close(audio_client_t *client);

/* Install the RT process callback.  Must be called before activate(). */
void audio_client_set_process_callback(audio_client_t *client,
                                        audio_process_cb_t cb,
                                        void *userdata);

/* Activate: start receiving audio callbacks.  Returns 0 on success.   */
int  audio_client_activate(audio_client_t *client);

/* Deactivate: stop the callback.  Safe to call from any thread.        */
void audio_client_deactivate(audio_client_t *client);

/*
 * Process one block — call this from the application main loop (for
 * polling-based apps) or let the RT thread call it automatically.
 * In RT mode, audio_client_activate() spawns a polling task that calls
 * audio_client_run_once() at the configured period.
 */
void audio_client_run_once(audio_client_t *client);

/* Get input / output float buffers (period_frames × channels).         */
float *audio_client_get_input_buffer(audio_client_t *client);
float *audio_client_get_output_buffer(audio_client_t *client);

/* Get estimated round-trip latency in nanoseconds.                     */
unsigned long long audio_client_get_latency(audio_client_t *client);

/* Get actual sample rate negotiated with the server.                   */
unsigned int audio_client_get_sample_rate(audio_client_t *client);

/* Get period frames negotiated with the server.                        */
unsigned int audio_client_get_period_frames(audio_client_t *client);

#endif /* AETHER_AUDIO_H */
