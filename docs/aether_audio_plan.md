# Phase 8 — AetherAudio: Realtime Guitar & Music System

> **Created:** 2026-05-21  
> **Last updated:** 2026-05-21  
> **Status:** PLANNING  
> **Latency target:** < 6 ms round-trip (input → DSP → output)  
> **Reference products:** AmpliTube 5, Neural Amp Modeler, Helix Native, Bias FX 2  

---

## Overview & Goals

AetherOS becomes the OS for guitar players. Phase 8 delivers a complete,
professional-grade signal processing system from bare-metal kernel drivers up
to a fully visual guitar amplifier and pedalboard application.

**Non-negotiable requirements:**
- ≤ 6 ms end-to-end latency at 48 kHz / 64-sample buffer
- USB Audio Class 2.0 support (Behringer UMC202HD)
- Onboard Pi 5 audio support
- VST3 / LV2 / CLAP plugin hosting (designed-in from day one)
- NEON SIMD-optimized DSP (float32x4_t throughout)
- Skeuomorphic UI — knobs, pedals, amp heads that look and feel like real hardware
- Neural Amp Modeling (.nam files, LSTM inference)
- Convolution-based cabinet simulation (WAV IR files)

---

## Architecture Stack

```
┌─────────────────────────────────────────────────────────────────┐
│                    AetherGuitar App (Phase 8.10)                │
│          Pedalboard · Amp Head · Tuner · Preset Browser         │
├─────────────────────────────────────────────────────────────────┤
│            Skeuomorphic UI Extensions (Phase 8.9)               │
│       Knob · LED · VU Meter · Spectrum · Stomp Switch           │
├─────────────────────────────────────────────────────────────────┤
│         Signal Chain Engine + MIDI (Phase 8.8)                  │
│      DAG routing · Preset system · Lock-free param queue        │
├───────────────────────┬─────────────────────────────────────────┤
│  Effects Library      │   Amp Modeling Engine (Phase 8.6)       │
│  libAetherFX (8.7)    │   IR conv · Tone stacks · LSTM/NAM      │
├───────────────────────┴─────────────────────────────────────────┤
│           Plugin Framework libAetherPlugin (Phase 8.5)          │
│           Internal CLAP API · VST3 · LV2 · CLAP adapters        │
├─────────────────────────────────────────────────────────────────┤
│           DSP Foundation libAetherDSP (Phase 8.4)               │
│        NEON FFT · Biquad · OLA conv · Envelope · Meter           │
├─────────────────────────────────────────────────────────────────┤
│          AetherSound Server (Phase 8.3)                         │
│       Audio daemon · SHM ring buffers · Device routing          │
├──────────────────────────┬──────────────────────────────────────┤
│  USB Audio UAC2 (8.1)    │  I2S/PCM Onboard Audio (8.2)         │
│  UMC202HD support        │  BCM2712 AudioMini                   │
├──────────────────────────┴──────────────────────────────────────┤
│          RT Kernel Enhancements (Phase 8.0)                     │
│    SCHED_FIFO · CPU isolation · mlockall · IRQ affinity          │
└─────────────────────────────────────────────────────────────────┘
```

---

## Latency Budget Analysis

| Stage | Contribution | Notes |
|-------|-------------|-------|
| USB isochronous (input) | ~1.0 ms | UAC2 HS: 125 µs microframes, 8 packets = 1 buffer |
| Kernel driver → ring buffer | ~0.1 ms | DMA completion IRQ → ring write |
| AetherSound server dispatch | ~0.2 ms | Signal via shared mem, RT thread wakeup |
| DSP processing (64 samples @ 48 kHz) | ~1.33 ms | Budget window |
| USB output buffer | ~1.0 ms | Same as input side |
| DAC analog latency | ~0.3 ms | UMC202HD hardware |
| **Total (64-sample buffer)** | **~4 ms** | **Target: ≤ 6 ms** |

Onboard Pi 5 I2S path: ~3 ms (no USB overhead).

---

## Phase 8.0 — RT Kernel Enhancements

**Goal:** Give audio threads hard real-time guarantees. A page fault, priority
inversion, or preemption in the audio path causes audible glitches.

### 8.0.1 — SCHED_FIFO for EL0 Threads

Current AetherOS scheduler uses a cooperative + round-robin model. Add
priority-based preemptive scheduling:

- `task_t` gains `sched_policy` (SCHED_NORMAL=0, SCHED_FIFO=1, SCHED_RR=2)
  and `rt_priority` (1-99, POSIX convention)
- Scheduler `pick_next_task()`: RT tasks always preempt normal tasks
- SCHED_FIFO: runs until it yields, blocks, or a higher-priority RT task arrives
- `SYS_SCHED_SETPARAM (930)`: userspace sets own policy/priority
- Audio thread will use SCHED_FIFO, priority 80

### 8.0.2 — CPU Affinity & Isolation

Pi 5 has 4 Cortex-A76 cores. Reserve core 3 for audio processing:

- `task_t` gains `cpu_affinity` (bitmask, 4-bit for 4 cores)
- `SYS_SCHED_SETAFFINITY (931)`: bind task to CPU set
- Kernel init masks core 3 from normal scheduler pool when audio is active
- Interrupt routing: USB xHCI IRQ also pinned to core 3 (via GIC)
- Audio server and its client processing callback run exclusively on core 3

### 8.0.3 — Memory Locking (mlockall)

Page faults in the audio callback cause latency spikes. Audio threads must
never page-fault:

- `SYS_MLOCKALL (932)`: locks all current and future pages of the calling task
  (sets a `mlocked` flag in `task_t`)
- On page fault: if `mlocked=1`, kernel panics with diagnostic instead of
  swapping (we don't have swap anyway, but prevents future regressions)
- AetherSound server calls mlockall during init

### 8.0.4 — High-Precision Audio Timer

- Expose `CNTPCT_EL0` directly to audio thread via `SYS_AUDIO_TIMESTAMP (933)`
- Returns nanosecond timestamp (CNTPCT × 1000 / CNTFRQ in MHz)
- Used by audio server for jitter measurement and drift correction

### 8.0.5 — IRQ Latency Hardening

- Disable IRQ coalescing on xHCI controller (ERSTS register, 0 coalesce)
- USB isochronous IRQ must complete and wake audio thread within 100 µs
- Add kernel IRQ latency tracer: record max IRQ-to-thread-wakeup time
  (accessible via `SYS_AUDIO_LATENCY_STATS (934)`)

**New files:**
- `kernel/core/sched_rt.c` — SCHED_FIFO/RR implementation
- `kernel/core/cpu_affinity.c` — CPU pinning
- `kernel/include/aether/sched.h` — scheduling types and constants

---

## Phase 8.1 — USB Audio Class 2.0 Driver

**Goal:** Full USB Audio Class 2.0 (UAC2) support targeting the Behringer
UMC202HD but designed for any UAC2-compliant interface.

### UMC202HD Specifications
- USB 2.0 High-Speed (480 Mbit/s)
- 2 inputs × 2 outputs (TRS/XLR combo + Hi-Z instrument input)
- 24-bit / 44.1 kHz, 48 kHz, 96 kHz
- UAC2 isochronous endpoints: IN (capture) + OUT (playback)
- USB Vendor ID: 0x1397, Product ID: 0x0507

### 8.1.1 — xHCI Isochronous Transfer Support

Existing `usb/ohci.c` only handles OHCI (USB 1.x). The Pi 5's USB 3.0 host
uses xHCI. The UMC202HD needs USB 2.0 HS isochronous transfers:

- Add `kernel/drivers/usb/xhci.c` — xHCI host controller driver
- Transfer Ring management (Event/Command/Transfer rings, 256-entry TRBs)
- Isochronous TRB type: `ISOCH_TRB`, `Interval` field controls scheduling
- Double-buffered isochronous: keep 2 TRBs in flight at all times
- IRQ handler: `xhci_irq()` → event ring dequeue → complete audio callbacks

### 8.1.2 — USB Audio Class 2.0 Class Driver

- `kernel/drivers/usb/audio/uac2.c` — UAC2 class driver
- **Enumeration:** parse AudioControl interface, AudioStreaming interfaces,
  Terminal descriptors (Input Terminal, Output Terminal, Feature Units)
- **Format negotiation:** SET_CUR → AS_GENERAL → format (PCM), sample rate,
  bit depth. Prefer 48 kHz / 24-bit (packed in 3 bytes per sample)
- **Streaming endpoints:** async isochronous OUT (playback), IN (capture),
  feedback endpoint for sync (SOF-based)
- **DMA ring buffer:** 8-packet ring (1 ms × 8 = 8 ms), kernel-side
  lock-free ring (`audio_ring_t`) — power-of-2 sample count

### 8.1.3 — Kernel Audio Device API

Unified API for both USB audio and onboard I2S:

```c
/* kernel/include/aether/audio_dev.h */
typedef struct audio_dev audio_dev_t;

audio_dev_t *audio_dev_open(const char *name);
void audio_dev_close(audio_dev_t *dev);
int  audio_dev_configure(audio_dev_t *dev, uint32_t sample_rate,
                         uint8_t bit_depth, uint8_t channels);
int  audio_dev_start(audio_dev_t *dev);
int  audio_dev_stop(audio_dev_t *dev);

/* Called from DMA IRQ — must be lock-free and ISR-safe */
typedef void (*audio_callback_t)(float *in, float *out,
                                 uint32_t frames, void *userdata);
int  audio_dev_set_callback(audio_dev_t *dev, audio_callback_t cb,
                            void *userdata, uint32_t period_frames);
```

### 8.1.4 — USB MIDI Class Driver

UMC202HD exposes a USB MIDI 1.0 interface:

- `kernel/drivers/usb/midi/usb_midi.c` — USB MIDI class driver
- Bulk endpoint polling (interrupt fallback)
- MIDI 1.0 parse: note on/off, CC, program change, pitch bend, SysEx
- Kernel MIDI event ring → userspace via `SYS_MIDI_READ (940)`

### 8.1.5 — Syscalls

| Syscall | Number | Description |
|---------|--------|-------------|
| `SYS_AUDIO_ENUM` | 935 | Enumerate audio devices |
| `SYS_AUDIO_OPEN` | 936 | Open audio device |
| `SYS_AUDIO_CLOSE` | 937 | Close audio device |
| `SYS_AUDIO_CONFIGURE` | 938 | Set sample rate / format |
| `SYS_AUDIO_START` | 939 | Start streaming |
| `SYS_MIDI_READ` | 940 | Read MIDI events |
| `SYS_MIDI_WRITE` | 941 | Send MIDI events |

**New files:**
- `kernel/drivers/usb/xhci.c` + `xhci.h`
- `kernel/drivers/usb/audio/uac2.c` + `uac2.h`
- `kernel/drivers/usb/midi/usb_midi.c` + `usb_midi.h`
- `kernel/include/aether/audio_dev.h`

---

## Phase 8.2 — I2S / PCM Onboard Audio Driver

**Goal:** Use Pi 5's AudioMini (PCM5102A DAC + PCM1804 ADC over I2S) for
zero-latency loopback testing without USB overhead.

### BCM2712 I2S Architecture
- Pi 5 has an I2S peripheral on GPIO 18-21 (BCM2712)
- AudioMini HAT or direct TRS uses PCM5102A (DAC) + PCM1804 (ADC)
- Can also use the PWM audio output (3.5mm jack) for monitoring

### 8.2.1 — I2S Peripheral Driver

- `kernel/drivers/audio/i2s_bcm2712.c`
- MMIO: I2S base at BCM2712 I2S offset from RP1 bridge
- DMA channel allocation (DMA_CS, DMA_CONBLK_AD, DMA_TI)
- Double-buffer DMA: 2 × 64-sample buffers, alternating
- DMA completion IRQ → `audio_callback_t` → next buffer scheduled
- Supports 44.1/48/96 kHz, 16/24/32-bit, stereo

### 8.2.2 — PWM Audio Fallback

For QEMU testing (no I2S in `-M virt`):

- `kernel/drivers/audio/pwm_audio.c`
- Software-render to ring buffer, user reads via `SYS_AUDIO_OPEN`
- 44.1 kHz mono, 16-bit (sufficient for testing DSP chain)
- QEMU `-audiodev` virtio or PA backend

**New files:**
- `kernel/drivers/audio/i2s_bcm2712.c` + `i2s_bcm2712.h`
- `kernel/drivers/audio/pwm_audio.c` (QEMU fallback)
- `kernel/drivers/audio/audio_core.c` — device registry, shared ring logic

---

## Phase 8.3 — AetherSound Server & Client Library

**Goal:** The audio server is the central router. Apps connect to it as clients.
It owns the hardware device exclusively and mixes/routes audio between clients.

### Architecture

```
┌────────────────┐    SHM ring buffer    ┌──────────────────────┐
│  AetherGuitar  │ ──────────────────→  │                      │
│  (audio client)│ ←────────────────── │   aether_sound       │
└────────────────┘    SHM ring buffer    │   (server daemon)    │
                                         │                      │
┌────────────────┐                       │  RT thread: FIFO 80  │
│  future client │ ──────────────────→  │  CPU affinity: core 3│
└────────────────┘                       │  mlockall            │
                                         └──────────┬───────────┘
                                                    │
                                         ┌──────────▼───────────┐
                                         │  audio_dev_t         │
                                         │  (UAC2 or I2S)       │
                                         └──────────────────────┘
```

### 8.3.1 — AetherSound Daemon

- `userspace/apps/aether_sound/main.c`
- Spawned by `init` at boot (before compositor, lower priority)
- Sets SCHED_FIFO priority 80, CPU affinity core 3, mlockall
- Opens audio device via `SYS_AUDIO_OPEN` (UAC2 preferred, I2S fallback)
- Creates N shared memory audio buffers (GPU BO mechanism, PMM pages)
- Accepts client connections via named IPC channel
  (`SYS_IPC_CONNECT (942)` — new light IPC)

### 8.3.2 — Lock-Free Audio Ring Buffer

Critical structure for zero-copy audio between server and clients:

```c
/* Power-of-2 size, cache-line aligned, atomic head/tail */
typedef struct {
    _Atomic uint32_t  write_idx;   /* written by producer */
    uint8_t           _pad0[60];   /* separate cache line */
    _Atomic uint32_t  read_idx;    /* written by consumer */
    uint8_t           _pad1[60];
    uint32_t          size_mask;   /* size - 1 */
    float             data[];      /* interleaved float32 samples */
} audio_ring_t;
```

- Server write path: DSP output → ring → client reads
- Client write path: microphone source → ring → server reads (for recording)
- No mutex in hot path — only atomic load/store with acquire/release

### 8.3.3 — Client Library (libAetherAudio)

- `userspace/lib/libaetheraudio/` — client API
- `audio_client_open(rate, channels, period_frames)` → handle
- `audio_client_set_process_callback(cb, userdata)` — RT callback
- `audio_client_activate()` / `audio_client_deactivate()`
- `audio_client_get_input_buffer()` / `audio_client_get_output_buffer()`
- `audio_client_get_latency()` → nanoseconds

### 8.3.4 — Audio Monitoring & Debug

- `SYS_AUDIO_LATENCY_STATS (934)`: returns max/avg/min callback jitter
- `userspace/apps/aether_audmon/main.c` — minimal audio monitor app
  (CPU%, latency, xrun count, input level meter)

**New files:**
- `userspace/apps/aether_sound/main.c`
- `userspace/apps/aether_audmon/main.c`
- `userspace/lib/libaetheraudio/` (client API + ring buffer)

---

## Phase 8.4 — libAetherDSP Foundation

**Goal:** High-performance DSP primitives optimized for AArch64 NEON SIMD.
All processing in float32. This library has zero dependencies.

### Design Principles
- All DSP functions process in blocks of N samples (N must be a multiple of 4
  for NEON vectorization)
- No dynamic allocation in processing path — all state pre-allocated
- Denormal protection via FZ bit in FPCR (flush-to-zero enabled at init)
- Functions documented with algorithmic reference, not just name

### 8.4.1 — NEON Utilities & Math

```c
/* lib/libAetherDSP/include/adsp_neon.h */
void adsp_neon_init(void);           /* set FPCR FZ=1 DAZ=1 */
void adsp_add_f32(float *dst, const float *a, const float *b, int n);
void adsp_mul_f32(float *dst, const float *a, const float *b, int n);
void adsp_mad_f32(float *dst, const float *a, float gain, const float *b, int n);
void adsp_clamp_f32(float *dst, const float *src, float lo, float hi, int n);
float adsp_rms_f32(const float *x, int n);
float adsp_peak_f32(const float *x, int n);
void adsp_interleave_f32(float *dst, const float *l, const float *r, int frames);
void adsp_deinterleave_f32(float *l, float *r, const float *src, int frames);
```

### 8.4.2 — Biquad Filter (IIR)

Direct Form II Transposed — numerically stable, NEON-friendly:

```c
typedef struct {
    float b0, b1, b2, a1, a2; /* coefficients */
    float w1, w2;              /* state */
} adsp_biquad_t;

/* Cookbook formulae (Robert Bristow-Johnson) */
void adsp_biquad_lpf(adsp_biquad_t *b, float fc, float q, float sr);
void adsp_biquad_hpf(adsp_biquad_t *b, float fc, float q, float sr);
void adsp_biquad_bpf(adsp_biquad_t *b, float fc, float q, float sr);
void adsp_biquad_notch(adsp_biquad_t *b, float fc, float q, float sr);
void adsp_biquad_peaking(adsp_biquad_t *b, float fc, float q, float db, float sr);
void adsp_biquad_loshelf(adsp_biquad_t *b, float fc, float db, float sr);
void adsp_biquad_hishelf(adsp_biquad_t *b, float fc, float db, float sr);
void adsp_biquad_process(adsp_biquad_t *b, float *y, const float *x, int n);
void adsp_biquad_process_x4(adsp_biquad_t b[4], float *y, const float *x, int n);
```

### 8.4.3 — FFT Engine

Cooley-Tukey radix-2 in-place, float32, NEON butterfly:

```c
typedef struct adsp_fft adsp_fft_t;

adsp_fft_t *adsp_fft_create(int size);   /* size must be power-of-2 */
void        adsp_fft_destroy(adsp_fft_t *fft);
void        adsp_fft_forward(adsp_fft_t *fft, float *re, float *im);
void        adsp_fft_inverse(adsp_fft_t *fft, float *re, float *im);
void        adsp_rfft_forward(adsp_fft_t *fft, const float *x,
                              float *re, float *im);   /* real input */
```

Pre-computed twiddle factors in ROM. FFT sizes 64–65536 supported.

### 8.4.4 — Overlap-Add Convolution Engine

Used for IR (cabinet) simulation and convolution reverb:

```c
typedef struct adsp_conv adsp_conv_t;

adsp_conv_t *adsp_conv_create(const float *ir, int ir_len, int block_size);
void         adsp_conv_destroy(adsp_conv_t *conv);
void         adsp_conv_process(adsp_conv_t *conv,
                               float *out, const float *in, int n);
```

Uniform partitioned convolution (UPC) for IR lengths > 4096:
- Direct convolution for first partition (low latency)
- FFT-based OLA for remaining partitions (efficiency)

### 8.4.5 — Envelope Follower & Metering

```c
typedef struct { float attack_coef, release_coef, state; } adsp_env_t;
void  adsp_env_init(adsp_env_t *e, float attack_ms, float release_ms, float sr);
float adsp_env_peak(adsp_env_t *e, float x);
float adsp_env_rms(adsp_env_t *e, float x2);  /* input is x*x */

/* dBFS metering with peak hold */
typedef struct adsp_meter adsp_meter_t;
adsp_meter_t *adsp_meter_create(float hold_sec, float decay_db_per_sec, float sr);
void          adsp_meter_process(adsp_meter_t *m, const float *x, int n);
float         adsp_meter_peak_dbfs(adsp_meter_t *m);
float         adsp_meter_rms_dbfs(adsp_meter_t *m);
float         adsp_meter_held_peak_dbfs(adsp_meter_t *m);
```

### 8.4.6 — Waveshaper / Nonlinearity

```c
/* Lookup table + cubic interpolation for fast soft-clip models */
typedef struct adsp_waveshaper adsp_waveshaper_t;
adsp_waveshaper_t *adsp_waveshaper_create(float (*fn)(float), int table_size);
void adsp_waveshaper_destroy(adsp_waveshaper_t *ws);
void adsp_waveshaper_process(adsp_waveshaper_t *ws,
                             float *y, const float *x, float drive, int n);

/* Built-in shapes */
float adsp_shape_soft_clip(float x);    /* tanh approx */
float adsp_shape_hard_clip(float x);    /* clamp(-1, 1) */
float adsp_shape_asymmetric(float x);   /* diode bridge model */
float adsp_shape_fuzz(float x);         /* BJT saturation */
float adsp_shape_tube(float x);         /* triode approx */
```

### 8.4.7 — Resampler

For sample rate conversion (e.g., 96kHz IR loaded at 48kHz project rate):

```c
typedef struct adsp_resampler adsp_resampler_t;
adsp_resampler_t *adsp_resampler_create(float ratio, int quality);
void adsp_resampler_process(adsp_resampler_t *r,
                            const float *in, int in_frames,
                            float *out, int *out_frames);
```

**New directory:** `userspace/lib/libAetherDSP/`
- `src/adsp_neon.c` + `adsp_neon_asm.S` (NEON intrinsics)
- `src/adsp_biquad.c`
- `src/adsp_fft.c`
- `src/adsp_conv.c`
- `src/adsp_env.c`
- `src/adsp_waveshaper.c`
- `src/adsp_resampler.c`
- `include/adsp.h` (master include)

---

## Phase 8.5 — Plugin Framework (libAetherPlugin)

**Goal:** A clean, extensible plugin API that natively supports the internal
effects library AND can host VST3, LV2, and CLAP plugins from disk.

### Design: CLAP-Inspired Internal API

CLAP (CLever Audio Plug-in) is the modern, open plugin standard. We adopt its
architecture for the internal API, making our internal effects
indistinguishable from loaded CLAP plugins.

### 8.5.1 — Internal Plugin API

```c
/* userspace/lib/libAetherPlugin/include/aeplugin.h */

#define AEPLUGIN_API_VERSION 1

typedef enum {
    AEPLUGIN_PARAM_FLOAT = 0,
    AEPLUGIN_PARAM_INT,
    AEPLUGIN_PARAM_BOOL,
    AEPLUGIN_PARAM_ENUM,
} aeplugin_param_type_t;

typedef struct {
    uint32_t            id;
    const char         *name;
    const char         *unit;      /* "dB", "Hz", "ms", "%" */
    aeplugin_param_type_t type;
    double              min_val;
    double              max_val;
    double              default_val;
    double              step;      /* 0 = continuous */
    bool                is_automatable;
} aeplugin_param_info_t;

typedef struct aeplugin_instance aeplugin_instance_t;

typedef struct {
    uint32_t    api_version;
    const char *id;          /* reverse-DNS, e.g. "io.aetheros.fx.overdrive" */
    const char *name;
    const char *author;
    const char *description;
    const char *category;    /* "distortion", "reverb", "modulation", etc. */
    uint32_t    param_count;
    const aeplugin_param_info_t *params;

    aeplugin_instance_t *(*create)(uint32_t sample_rate, uint32_t max_block);
    void (*destroy)(aeplugin_instance_t *inst);
    void (*activate)(aeplugin_instance_t *inst);
    void (*deactivate)(aeplugin_instance_t *inst);
    void (*reset)(aeplugin_instance_t *inst);

    /* RT-safe. in/out may alias if bypass. */
    void (*process)(aeplugin_instance_t *inst,
                    const float *const *in, float **out,
                    uint32_t frames);

    double (*get_param)(aeplugin_instance_t *inst, uint32_t param_id);
    void   (*set_param)(aeplugin_instance_t *inst, uint32_t param_id, double val);

    /* Optional: parameter smoothing flush (called before snapshot) */
    void (*flush_params)(aeplugin_instance_t *inst);
} aeplugin_descriptor_t;
```

### 8.5.2 — Plugin Registry

- `aeplugin_registry_scan(const char *path)` — scan directory for plugins
- Plugin formats supported: `.ae` (internal), `.so` (CLAP/VST3/LV2 shared)
- Plugin manifest: `/plugins/<name>.aeplugin` (simple key=value)
- Registry stores `aeplugin_descriptor_t *` list
- `aeplugin_registry_find_by_id(id)` → descriptor
- Thread-safe reads (scan happens at init only)

### 8.5.3 — VST3 Host Adapter

VST3 is a COM-like C++ plugin format. We implement a thin host adapter:

- Load `.vst3` shared library via `dlopen` (needs ELF .so support)
- Query `GetPluginFactory()` → `IPluginFactory3`
- Create `IComponent` + `IAudioProcessor` + `IEditController`
- Convert between VST3 `ProcessData` and our `aeplugin_process` call
- Parameter sync: VST3 `IParamValueQueue` ↔ our set_param
- **Note:** VST3 requires C++ ABI — compile adapter as C++20

### 8.5.4 — LV2 Host Adapter

LV2 is the standard Linux audio plugin format:

- Parse `.lv2` bundle: `manifest.ttl` (Turtle RDF)
- Load plugin `.so` via dlopen
- LV2 `LV2_Descriptor.connect_port` for audio I/O + control ports
- `lv2_run(n_samples)` → our process callback
- URID map feature, Atom feature for MIDI I/O
- Minimal Turtle parser (we don't need a full RDF stack)

### 8.5.5 — CLAP Host Adapter

CLAP is the newest open plugin standard with the best latency model:

- Load `.clap` shared library
- Query `clap_plugin_entry.get_factory(CLAP_PLUGIN_FACTORY_ID)`
- Create `clap_plugin_t`, call `init → activate → start_processing`
- `clap_plugin.process` takes `clap_process_t` with audio buffers + events
- Parameter changes via `CLAP_EVENT_PARAM_VALUE` in event list

### 8.5.6 — Parameter Smoothing

RT-safe parameter changes require smoothing to avoid zipper noise:

```c
typedef struct {
    float target;
    float current;
    float coef;    /* e^(-2π * 10Hz / sr) — 10ms smooth */
} adsp_smooth_t;

static inline float adsp_smooth_tick(adsp_smooth_t *s) {
    s->current += s->coef * (s->target - s->current);
    return s->current;
}
```

Every plugin parameter that affects audio uses `adsp_smooth_t` internally.

**New directory:** `userspace/lib/libAetherPlugin/`

---

## Phase 8.6 — Amp Modeling Engine (libAetherAmp)

**Goal:** Authentic amp simulation. Two parallel engines: IR-based cabinet
simulation + neural amp modeling (LSTM/WaveNet inference for pre-amp stages).

### 8.6.1 — Impulse Response Loader

```c
typedef struct {
    float   *samples;       /* mono float32, sr-normalized */
    uint32_t length;
    uint32_t sample_rate;
    char     name[64];
} aamp_ir_t;

aamp_ir_t *aamp_ir_load_wav(const char *path);
aamp_ir_t *aamp_ir_load_raw(const float *data, int len, int sr);
void       aamp_ir_destroy(aamp_ir_t *ir);
```

WAV parser handles: PCM 16/24/32-bit, float32, mono/stereo (auto-mix to mono).
Sample rate conversion via `adsp_resampler_t` if IR SR ≠ project SR.

### 8.6.2 — Cabinet Simulation

Wraps `adsp_conv_t` with IR management:

```c
typedef struct aamp_cab aamp_cab_t;

aamp_cab_t *aamp_cab_create(const aamp_ir_t *ir, int block_size);
void        aamp_cab_destroy(aamp_cab_t *cab);
void        aamp_cab_process(aamp_cab_t *cab,
                             float *out, const float *in, int n);
void        aamp_cab_swap_ir(aamp_cab_t *cab, const aamp_ir_t *new_ir);
```

Bundled cabinet IRs (stored in `/cabs/` on disk):
- `4x12_v30_sm57.wav` — Marshall 4×12 Celestion V30, SM57 at cap
- `4x12_g12m_sm57.wav` — Marshall 4×12 Greenback
- `2x12_alnico_blue.wav` — Vox 2×12 Alnico Blue
- `1x12_jensen_p12n.wav` — Fender 1×12 Jensen P12N
- `4x12_mesa_m25.wav` — Mesa 4×12 Black Shadow C90

### 8.6.3 — Tone Stack Models

Passive and active tone stack simulations using biquad chains:

```c
typedef struct { adsp_biquad_t bass, mid, treble; } aamp_tonestack_t;

/* Style: AAMP_TONESTACK_FENDER, MARSHALL, VOX, MESA, BAXANDALL */
void aamp_tonestack_init(aamp_tonestack_t *ts, int style, float sr);
void aamp_tonestack_set(aamp_tonestack_t *ts,
                        float bass, float mid, float treble); /* 0.0-1.0 */
void aamp_tonestack_process(aamp_tonestack_t *ts,
                            float *y, const float *x, int n);
```

Each model derived from the actual passive filter networks in the reference
amp circuits (using component values from schematics).

### 8.6.4 — Neural Amp Modeler (NAM) Engine

The NAM engine runs LSTM and WaveNet models trained on real amp recordings.
Models are distributed as `.nam` files (JSON header + float32 binary weights).

**Supported architectures:**
- `LSTM` (1 or 2 hidden layers, hidden_size 8–128)
- `WaveNet` (dilated causal conv, 1–4 layers, dilation 1/2/4/8)
- `Linear` (simple matrix multiply — used for boosters)

**LSTM inference (core hot path):**
```c
typedef struct aamp_nam aamp_nam_t;

aamp_nam_t *aamp_nam_load(const char *path);   /* parse .nam file */
void        aamp_nam_destroy(aamp_nam_t *nam);
void        aamp_nam_process(aamp_nam_t *nam,
                             float *out, const float *in, int n);
void        aamp_nam_set_input_gain(aamp_nam_t *nam, float db);
void        aamp_nam_set_output_gain(aamp_nam_t *nam, float db);
```

LSTM GEMV operations use NEON float32x4_t to process 4 hidden units
simultaneously. For hidden_size=32: 8 NEON GEMV rows per step.

Activation functions: tanh (minimax polynomial approx, < 0.1% error),
sigmoid (1/(1+exp(-x)) via fast exp approximation).

**WaveNet inference:**
Dilated causal convolution processed as: input buffer → gated activation →
skip connection → residual add. Input conditioning via 1-channel signal.

### 8.6.5 — Pre-Amp Stage Chain

```c
/* Full amp signal chain */
typedef struct {
    adsp_biquad_t       input_hpf;     /* instrument input high-pass */
    adsp_biquad_t       input_lpf;     /* anti-alias */
    adsp_waveshaper_t  *drive_stage;   /* soft/hard clip */
    aamp_tonestack_t    tonestack;
    adsp_biquad_t       presence;      /* high-shelf */
    aamp_cab_t         *cabinet;       /* or aamp_nam_t for neural */
    adsp_biquad_t       output_lpf;    /* speaker rolloff */
} aamp_amp_t;
```

Pre-amp models (rule-based, not neural):
- `AAMP_MODEL_FENDER_TWIN` — clean, glassy
- `AAMP_MODEL_MARSHALL_JCM800` — classic British crunch
- `AAMP_MODEL_MESA_DUAL_RECT` — tight modern high-gain
- `AAMP_MODEL_VOX_AC30` — chime, class-A compression
- `AAMP_MODEL_CUSTOM` — user-tunable drive + tone

**New directory:** `userspace/lib/libAetherAmp/`

---

## Phase 8.7 — Guitar Effects Library (libAetherFX)

Each effect is a self-contained `aeplugin_descriptor_t`. All implement the
exact plugin API from Phase 8.5. This means they work identically whether
inserted via the internal signal chain or loaded as a CLAP plugin.

### 8.7.1 — Input / Utility

**Tuner** (`io.aetheros.fx.tuner`)
- YIN algorithm: autocorrelation-based f0 detection
- Chromatic, A=440 Hz reference (adjustable to 432-446)
- 20 Hz – 2000 Hz detection range (bass + guitar)
- Update rate: 30 Hz display, 1000 Hz detection
- Passes audio through unmodified (always active)

**Noise Gate** (`io.aetheros.fx.noise_gate`)
- Expander/gate: threshold, attack, hold, release, range
- Hysteresis: open threshold > close threshold by 3 dB
- Lookahead: 5 ms (requires small additional latency compensation)
- Side-chain low-pass at 3 kHz (keys off midrange buzz, not pick attack)

**Compressor** (`io.aetheros.fx.compressor`)
- RMS detector (10 ms window), peak limiter on output
- Parameters: threshold, ratio (1:1–20:1), attack, release, knee, makeup gain
- VCA gain-computer with soft knee
- Gain reduction meter output

### 8.7.2 — Gain / Drive

**Overdrive** (`io.aetheros.fx.overdrive`)
- TS-808 Tube Screamer model: op-amp soft clip + asymmetric diode limiting
- JFET input stage (J201 Vp=-0.3V model) for authentic impedance loading
- Tone control: single-pole RC high-pass (classic TS tone)
- Parameters: drive (0–1), tone (0–1), level (0–1)

**Distortion** (`io.aetheros.fx.distortion`)
- ProCo RAT model: op-amp hard clip + LM308 gain stage
- Filter (dist): variable low-pass before clipping (0.5–1.0 = more bass)
- Parameters: distortion, filter, volume

**Fuzz** (`io.aetheros.fx.fuzz`)
- Big Muff Pi model: two hard-clipping stages with tone control
- Tone: Big Muff style: LPF/HPF crossfade
- Sustain control adjusts drive depth
- BJT saturation model (NPN silicon)

**Boost** (`io.aetheros.fx.boost`)
- Clean transparent buffer + adjustable gain (+0 to +30 dB)
- Optional low-pass (presence cut) + high-pass (bass cut)
- Treble booster variant: Rangemaster model (germanium transistor LPF)

**Amp Drive** (`io.aetheros.fx.amp_drive`)
- Uses `aamp_amp_t` pre-amp stage (rule-based)
- Parameters: gain, bass, mid, treble, presence, master
- Selectable amp model (FENDER / MARSHALL / MESA / VOX / CUSTOM)

### 8.7.3 — EQ / Filter

**Parametric EQ** (`io.aetheros.fx.peq`)
- 4 bands: LF shelf + 2× peaking bell + HF shelf
- Each band: frequency, gain ±18 dB, Q (0.1–10)
- Uses `adsp_biquad_t` per band

**Graphic EQ** (`io.aetheros.fx.geq`)
- 10 bands: 31, 63, 125, 250, 500, 1k, 2k, 4k, 8k, 16k Hz
- ±15 dB per band
- Constant-Q biquad bank

**Wah** (`io.aetheros.fx.wah`)
- Vox wah model: resonant bandpass sweep 400–2200 Hz
- Q = 4.5 (characteristically honky)
- Controlled by expression pedal (MIDI CC 11) or envelope follower
- Auto-wah mode: envelope → wah frequency

### 8.7.4 — Modulation

**Chorus** (`io.aetheros.fx.chorus`)
- Stereo chorus: 2-voice ± detuned delay lines (1–20 ms)
- Sine LFO per voice, independently phased
- Parameters: rate (0.1–10 Hz), depth (0–1), mix (dry–wet)
- BBD (bucket brigade) saturation model optional

**Flanger** (`io.aetheros.fx.flanger`)
- Short delay (0.5–10 ms), LFO modulated
- Feedback path for resonance
- Manual control (static notch position)
- Parameters: rate, depth, feedback, mix

**Phaser** (`io.aetheros.fx.phaser`)
- 4- or 8-stage all-pass phaser (MXR Phase 90 model)
- Sine LFO modulates all-pass center frequencies in unison
- Parameters: rate, depth, feedback, stages (4/8), mix

**Tremolo** (`io.aetheros.fx.tremolo`)
- Amplitude modulation: LFO × input
- LFO shapes: sine, triangle, square, ramp up/down
- Parameters: rate (0.5–20 Hz), depth (0–1), shape, mix

**Vibrato** (`io.aetheros.fx.vibrato`)
- Pitch modulation via delay line modulation
- Deviation: ±50 cents max
- Parameters: rate, depth, mix (0 = vibrato only, not chorus)

**Rotary Speaker** (`io.aetheros.fx.rotary`)
- Leslie 122 simulation: horn + drum independent rotation
- Fast/slow modes with acceleration modeling
- Parameters: speed (slow/fast/stop), horn_rate, drum_rate, mix

### 8.7.5 — Delay

**Digital Delay** (`io.aetheros.fx.digital_delay`)
- Single echo, up to 2000 ms delay time
- Ping-pong mode: alternates L/R
- Tempo sync: tap tempo or MIDI clock
- Parameters: time, feedback, mix, ping_pong

**Multi-Tap Delay** (`io.aetheros.fx.multitap_delay`)
- 4 independent taps with time, level, pan per tap
- Shared feedback bus
- Parameters: tap[0..3].time, tap[0..3].level, feedback

**Tape Delay** (`io.aetheros.fx.tape_delay`)
- Wow: slow pitch drift (0.1–2 Hz, up to ±50 cents)
- Flutter: faster irregular pitch (1–15 Hz, ±20 cents)
- Saturation: Ferrofluid model (soft clip on delay buffer)
- High-frequency rolloff in feedback path (tape bandwidth limit)
- Parameters: time, feedback, wow, flutter, saturation, mix

### 8.7.6 — Reverb

**Spring Reverb** (`io.aetheros.fx.spring_reverb`)
- Physical spring model: 3 spring tanks in parallel
- Each spring: delay + low-pass (sag) + feedback
- Characteristic "boing" when triggered
- Parameters: decay, tone, sag, drip, mix

**Room Reverb** (`io.aetheros.fx.room_reverb`)
- FDN (Feedback Delay Network), 8 channels
- Hadamard mixing matrix for decorrelation
- Parameters: size (room volume scale), decay, damp, mix

**Hall Reverb** (`io.aetheros.fx.hall_reverb`)
- FDN 16 channels, large delay lines (50–200 ms)
- Pre-delay: 0–100 ms
- Early reflections: 7-tap comb
- Parameters: pre_delay, size, decay, damp, diffusion, mix

**Plate Reverb** (`io.aetheros.fx.plate_reverb`)
- Dattorro figure-8 plate algorithm
- Parameters: pre_delay, decay, damp, mix

**Convolution Reverb** (`io.aetheros.fx.conv_reverb`)
- WAV IR loading via `aamp_ir_load_wav`
- UPC convolution via `adsp_conv_t`
- Parameters: ir_path (string), mix, pre_gain, post_gain

### 8.7.7 — Pitch

**Octave** (`io.aetheros.fx.octave`)
- Sub octave (−1 oct): full-wave rectifier + LPF + mix
- Super octave (+1 oct): ring modulator × self (aliasing controlled)
- Parameters: oct_down, oct_up, mix

**Pitch Shifter** (`io.aetheros.fx.pitch_shifter`)
- Phase vocoder: FFT analysis → pitch shift → IFFT synthesis
- Latency: 1 FFT frame = 512 samples at 48kHz = 10.7ms
  (acceptable for pitch shift — not in main signal chain by default)
- Parameters: semitones (−12 to +12), cents (±100), mix

**Harmonizer** (`io.aetheros.fx.harmonizer`)
- 2-voice pitch shifter with musical interval selection
- Key-aware: user selects key + scale → intervals auto-selected
- Parameters: voice[0..1].interval, key, scale, voice[0..1].level

---

## Phase 8.8 — Signal Chain Engine + MIDI

### 8.8.1 — Signal Routing DAG

```c
typedef struct achain_node achain_node_t;
typedef struct achain achain_t;

achain_t      *achain_create(uint32_t sample_rate, uint32_t block_size);
void           achain_destroy(achain_t *chain);
achain_node_t *achain_add_plugin(achain_t *chain, aeplugin_descriptor_t *desc);
void           achain_remove_node(achain_t *chain, achain_node_t *node);
void           achain_move_node(achain_t *chain, achain_node_t *node, int new_pos);
void           achain_connect(achain_node_t *src, int src_port,
                              achain_node_t *dst, int dst_port);
void           achain_process(achain_t *chain,
                              const float *in, float *out, int n);
```

RT-safe: no allocation in `achain_process`. All routing computed at
`achain_activate()` time into a flat ordered node list.

### 8.8.2 — Lock-Free Parameter Queue

Real-time safe bridge between UI thread and audio thread:

```c
#define APARAM_QUEUE_SIZE 256
typedef struct { uint32_t node_id; uint32_t param_id; double value; } aparam_ev_t;
typedef struct { _Atomic uint32_t w, r; aparam_ev_t ev[APARAM_QUEUE_SIZE]; } aparam_queue_t;

void aparam_queue_push(aparam_queue_t *q, uint32_t node_id,
                       uint32_t param_id, double val);  /* UI thread */
void aparam_queue_drain(aparam_queue_t *q, achain_t *chain); /* audio thread */
```

UI changes a knob → `aparam_queue_push` → audio thread drains queue each
block → `achain_node_set_param`. Zero locks, zero allocations.

### 8.8.3 — Preset System

Binary preset format `.aepre`:
```
Header: magic "AEPR" + version (uint32) + crc32 (uint32)
Node table: N nodes × { plugin_id (64B), param_count × {id, value} }
Metadata: name (128B), author (64B), category (32B)
```

JSON format for import/export (`json_parse` reuses QuickJS internals):
```json
{
  "name": "Blues Clean",
  "chain": [
    { "plugin": "io.aetheros.fx.compressor", "params": {"threshold": -18, "ratio": 4} },
    { "plugin": "io.aetheros.fx.amp_drive",  "params": {"gain": 0.3, "model": 0} }
  ]
}
```

- `/presets/` directory on FAT32 disk
- `preset_browser_t`: list, load, save, delete, search by name/category

### 8.8.4 — MIDI Routing

```c
typedef struct { uint8_t status, data1, data2, _pad; } midi_event_t;

/* Map MIDI CC → plugin parameter */
typedef struct {
    uint8_t  cc;
    uint32_t node_id;
    uint32_t param_id;
    double   scale_min, scale_max;
} midi_mapping_t;

void midi_router_add_mapping(midi_mapping_t *m);
void midi_router_remove_cc(uint8_t cc);
void midi_router_process(const midi_event_t *events, int n,
                         aparam_queue_t *queue); /* called per audio block */
```

MIDI learn mode: arm a parameter → next CC received → auto-map.
MIDI clock: extracts BPM from clock messages → updates tempo-synced delays.

---

## Phase 8.9 — Skeuomorphic UI Extensions

**Goal:** Widgets that look and feel like real guitar hardware. GPU-accelerated,
highly responsive, visually stunning.

### Design Language: "Guitar Hardware"

Visual references:
- Pedals: Boss DS-1 (orange stamped metal, rubber knob caps), Ibanez TS-808
  (green anodized, cream chickenhead knobs), Electro-Harmonix (aluminum chassis)
- Amp heads: Fender Blackface (black Tolex, silver grille), Marshall JCM800
  (black vinyl, checkerboard grill), Mesa/Boogie (wicker grille, hardwood)
- Color palette: matte blacks, anodized blues/reds/greens, cream plastics
- Typography: Helvetica-style (Noto Sans), all-caps knob labels
- Texture maps: knurled metal, cork board, rubber, brushed aluminum

### 8.9.1 — Widget: Knob (WIDGET_KNOB)

The most important widget. Two styles:

**Davies 1900H style** (large, ridged, black):
- 270° rotation range
- Shadow under knob
- Value arc on ring (colored line)
- Tick marks at min/center/max
- Double-click: reset to default
- Drag up/down: ±0.5% per pixel (fine control)
- Shift+drag: ±0.1% per pixel (precision)
- Right-click context: "Set value...", "Reset to default", "MIDI learn"

**Chickenhead style** (pointer knob, small):
- Triangular pointer, 270° range
- Used for EQ, presence, etc.

**LED Crown style** (Marshall-style):
- LED ring around knob base
- Illuminated arc showing current value

```c
typedef struct {
    widget_t   base;
    float      value;       /* 0.0 – 1.0 */
    float      default_val;
    int        style;       /* KNOB_DAVIES / KNOB_CHICKENHEAD / KNOB_LED */
    uint32_t   color;       /* value arc color */
    const char *label;
    const char *unit;
    float      min, max;    /* display range for tooltip */
    void (*on_change)(float val, void *userdata);
    void *userdata;
} widget_knob_t;
```

### 8.9.2 — Widget: LED (WIDGET_LED)

```c
typedef enum { LED_ROUND, LED_RECT, LED_SEGMENT } led_shape_t;
typedef struct {
    widget_t   base;
    bool       on;
    uint32_t   color_on;   /* ARGB */
    uint32_t   color_off;
    led_shape_t shape;
    int        glow_radius; /* 0 = no glow */
} widget_led_t;
```

Rendered with radial gradient (inner bright → outer dim) when ON.
Glow: draw with gaussian-blurred circle at 25% alpha overlay.

### 8.9.3 — Widget: VU Meter (WIDGET_VU)

```c
typedef struct {
    widget_t  base;
    float     peak_dbfs;      /* updated by audio thread via atomic */
    float     rms_dbfs;
    float     held_peak_dbfs;
    int       orientation;    /* WIDGET_VU_VERT / WIDGET_VU_HORIZ */
    bool      show_rms;
    bool      show_peak_hold;
} widget_vu_t;
```

Gradient fill: green (−60 to −12 dBFS) → yellow (−12 to −6) → red (−6 to 0).
Peak hold: white line that holds for 2 seconds then slowly decays.

### 8.9.4 — Widget: Spectrum Analyzer (WIDGET_SPECTRUM)

- Real-time FFT spectrum display
- 30 Hz – 20 kHz, logarithmic frequency axis
- Peak hold bar (white) + live fill (gradient blue→green→red)
- Smoothing: 5 ms attack, 200 ms release (like hardware spectrum analyzers)
- Update rate: 60 fps (one FFT per frame)
- GPU-rendered: each frequency bin = 1 pixel column

### 8.9.5 — Widget: Stomp Switch (WIDGET_STOMP)

Accurate Boss/MXR-style 3PDT footswitch:
- Large clickable circle with metallic rim
- On: LED illuminates (color configurable per pedal)
- Physical depression animation (spring-bounce, 80ms)
- Click sound optional (via brief audio pulse)

### 8.9.6 — Widget: Fader (WIDGET_FADER)

- Vertical or horizontal
- Knurled cap (Alps fader style)
- Optional notch at center position
- Drag gesture, double-click reset

### 8.9.7 — Pedal Panel (WIDGET_PEDAL_PANEL)

Container widget rendering a complete guitar pedal:
- Textured background (stamped metal, painted, anodized)
- Raised edges (inner shadow)
- Logo/name area
- Hosts child knob/LED/switch widgets
- Each pedal type has its own texture + color scheme

### 8.9.8 — Amp Head Panel (WIDGET_AMP_HEAD)

- Full chassis simulation: Fender Blackface, Marshall JCM, Mesa, Vox
- Hosts knob rows, channel switches, VU meters
- Grille texture below (for combo) or separate cabinet widget

---

## Phase 8.10 — AetherGuitar Application

**Goal:** The flagship application. Visually stunning, intuitively laid out,
professional quality.

### 8.10.1 — Application Architecture

```
┌────────────────────────────────────────────────────────────────┐
│  Navigation Bar: [PEDALBOARD] [AMP] [TUNER] [NAM] [SETTINGS]  │
├────────────────────────────────────────────────────────────────┤
│                                                                │
│                     Active View                                │
│         (Pedalboard / Amp / Tuner / NAM / Settings)            │
│                                                                │
├────────────────────────────────────────────────────────────────┤
│  Status Bar: Input: -6dB [████░░░]  Output: -9dB [███░░░]      │
│              Latency: 4.2ms  Rate: 48kHz  Buffer: 64  CPU: 12% │
└────────────────────────────────────────────────────────────────┘
```

Audio client activates on launch. Signal chain: audio server input →
AetherGuitar DSP chain → audio server output.

### 8.10.2 — Pedalboard View

Cork-textured horizontal board. Pedals laid out left-to-right in signal order.

Features:
- Horizontal scroll (hand cursor drag) for boards wider than screen
- Each pedal: accurate skeuomorphic rendering via `WIDGET_PEDAL_PANEL`
- Stomp switch toggle (bypass each pedal, LED on/off)
- Drag pedals to reorder signal chain
- "+" button: open palette to add new pedal
- "−" button (long-press pedal): remove from chain
- Pedal palette: scrollable grid of available effects with category filter

Visual pedal designs implemented for:
- Tuner: Boss TU-3 style (large display panel)
- Noise Gate: ISP Decimator style (black, yellow label)
- Compressor: MXR Dyna Comp style (red, three knobs)
- Overdrive: TS-808 style (green, three knobs)
- Distortion: Boss DS-1 style (orange, three knobs)
- Fuzz: Big Muff Pi style (large, triangle shape)
- Chorus: Boss CE-1 style (blue)
- Flanger: A/DA Flanger style (brushed aluminum)
- Phaser: MXR Phase 90 style (orange, one knob)
- Tremolo: Fender Trem-King style (surf green)
- Delay: Boss DD-7 style (yellow, multi-knob)
- Reverb: EHX Holy Grail style (gold)
- Wah: Dunlop Cry Baby style (wah graphic)

### 8.10.3 — Amplifier View

Full-width amp head rendering:
- Top section: amp model selector (tabs: FENDER / MARSHALL / MESA / VOX / CUSTOM)
- Control panel: Preamp Gain, Bass, Mid, Treble, Presence, Master Volume
- Channel switch: Clean / Crunch / Lead
- Cabinet section: speaker cabinet image + IR selector dropdown
- Each amp model has accurate front-panel rendering

### 8.10.4 — NAM View (Neural Amp Modeler)

- Model library: grid of loaded .nam files with amp name + icon
- Browse/import: file picker for `.nam` files from disk
- Active model: large card showing model name, author, tags
- Input gain (−20 to +20 dB), Output gain
- Tone control (0–1) — passed as condition to NAM model
- EQ override: optional post-NAM EQ chain
- Cabinet: same IR cabinet panel as Amp view

### 8.10.5 — Tuner Screen

Full-screen chromatic tuner:
- Large note name (A, Bb, B, C, etc.)
- Octave indicator
- Frequency readout (Hz, 1 decimal)
- Needle meter (analog style): ±50 cents deviation
- Color: red when out of tune, yellow when close, green when locked
- Strobe tuner mode option (ultra-precise)
- Reference pitch: 440 Hz default, adjustable 432–446

### 8.10.6 — Settings View

**Audio Device:**
- Device dropdown (detected UAC2 devices + onboard I2S)
- Sample rate: 44100 / 48000 / 96000 Hz
- Buffer size: 32 / 64 / 128 / 256 / 512 samples
- Latency estimate (calculated and displayed)
- "Test tone" button

**Plugin Paths:**
- List of directories to scan for VST3 / LV2 / CLAP plugins
- Rescan button
- Plugin list with enable/disable per plugin

**MIDI:**
- Device list (USB MIDI devices)
- MIDI channel filter
- MIDI CC mapping table

**Calibration:**
- Input trim: per-input gain calibration
- Output trim: per-output level calibration

### 8.10.7 — Preset Browser

- Left panel: category tree (Factory / User / Imported)
- Right panel: preset list with name, amp model, effect count
- Load preset (instant — all param changes enqueued atomically)
- Save preset (name input dialog)
- Export/Import `.aepre` file
- Favorite marking

---

## File Structure

```
aether_os/
├── kernel/
│   ├── core/
│   │   ├── sched_rt.c            [8.0.1] RT scheduler
│   │   └── cpu_affinity.c        [8.0.2]
│   ├── drivers/
│   │   ├── audio/
│   │   │   ├── audio_core.c      [8.1.3] Device registry
│   │   │   ├── i2s_bcm2712.c     [8.2.1] I2S driver
│   │   │   └── pwm_audio.c       [8.2.2] QEMU fallback
│   │   └── usb/
│   │       ├── xhci.c            [8.1.1] xHCI host
│   │       ├── audio/
│   │       │   └── uac2.c        [8.1.2] USB Audio
│   │       └── midi/
│   │           └── usb_midi.c    [8.1.4]
│   └── include/aether/
│       ├── sched.h               [8.0]
│       └── audio_dev.h           [8.1.3]
│
├── userspace/
│   ├── apps/
│   │   ├── aether_sound/         [8.3.1] Audio server
│   │   ├── aether_audmon/        [8.3.4] Audio monitor
│   │   └── aether_guitar/        [8.10]  Main app
│   └── lib/
│       ├── libaetheraudio/       [8.3.3] Client library
│       ├── libAetherDSP/         [8.4]   DSP primitives
│       ├── libAetherPlugin/      [8.5]   Plugin framework
│       ├── libAetherAmp/         [8.6]   Amp modeling
│       └── libAetherFX/          [8.7]   Effects library
│           ├── fx_tuner.c
│           ├── fx_gate.c
│           ├── fx_compressor.c
│           ├── fx_overdrive.c
│           ├── fx_distortion.c
│           ├── fx_fuzz.c
│           ├── fx_eq_parametric.c
│           ├── fx_eq_graphic.c
│           ├── fx_wah.c
│           ├── fx_chorus.c
│           ├── fx_flanger.c
│           ├── fx_phaser.c
│           ├── fx_tremolo.c
│           ├── fx_vibrato.c
│           ├── fx_rotary.c
│           ├── fx_digital_delay.c
│           ├── fx_multitap_delay.c
│           ├── fx_tape_delay.c
│           ├── fx_spring_reverb.c
│           ├── fx_room_reverb.c
│           ├── fx_hall_reverb.c
│           ├── fx_plate_reverb.c
│           ├── fx_conv_reverb.c
│           ├── fx_octave.c
│           ├── fx_pitch_shifter.c
│           └── fx_harmonizer.c
```

---

## Testing Strategy

### Unit Tests (per library)
- `libAetherDSP`: frequency response tests for each biquad type (compare to
  analytical transfer function); FFT round-trip accuracy; convolution identity IR
- `libAetherFX`: each effect processes a 1-second sine sweep, output verified
  for gain correctness and no NaN/Inf values
- `libAetherAmp`: IR convolution output == direct convolution output (numerical)
- `libAetherPlugin`: plugin create→process→destroy lifecycle with no leaks

### Integration Tests
- Full signal chain: sine input → DSP chain → verify frequency content (no
  unexpected DC offset, gain > −60 dBFS)
- MIDI → parameter: CC event → chain param change in next audio block
- Preset save/load: save chain state, reload, verify all params identical
- Latency measurement: inject 1-sample click at input, measure output delay

### Performance Benchmarks
- 64-sample buffer @ 48kHz: total DSP chain must complete in < 1.0 ms
  (leaving 333 µs headroom in the 1.33 ms budget)
- LSTM inference (hidden=32): must run in < 0.2 ms per 64-sample block
- IR convolution (4096-point): < 0.3 ms per 64-sample block
- Memory bandwidth: entire audio chain < 2 MB working set (L2 cache of A76)

---

## Dependencies on Previous Phases

| Dependency | Required For | Status |
|-----------|-------------|--------|
| Phase 5.2.12 xHCI USB | Phase 8.1 UAC2 | Pending |
| Phase 5.1 network stack | Optional (model download) | Complete |
| Phase 6.1 GPU BOs | Phase 8.3 SHM audio buffers | Complete |
| libwidget | Phase 8.9 UI extensions | Complete |
| FreeType fonts | Phase 8.10 UI text | Complete |
| Compositor WM | Phase 8.10 windowed app | Complete (WM8) |

---

## Implementation Order & Milestones

| Phase | Description | Est. Duration |
|-------|-------------|---------------|
| 8.0 | RT Kernel Enhancements | 1 week |
| 8.1 | USB Audio UAC2 + xHCI | 2 weeks |
| 8.2 | I2S Onboard + QEMU audio | 1 week |
| 8.3 | AetherSound Server | 1 week |
| 8.4 | libAetherDSP | 2 weeks |
| 8.5 | libAetherPlugin | 1 week |
| 8.6 | libAetherAmp | 2 weeks |
| 8.7 | libAetherFX (all effects) | 3 weeks |
| 8.8 | Signal Chain + MIDI | 1 week |
| 8.9 | Skeuomorphic UI Extensions | 2 weeks |
| 8.10 | AetherGuitar Application | 3 weeks |
| **Total** | | **~19 weeks** |

---

## Status

| Sub-phase | Status | Notes |
|-----------|--------|-------|
| 8.0 RT Kernel | ⬜ TODO | |
| 8.1 USB Audio UAC2 | ⬜ TODO | Depends on xHCI (5.2.12) |
| 8.2 I2S + PWM QEMU | ⬜ TODO | |
| 8.3 AetherSound | ⬜ TODO | |
| 8.4 libAetherDSP | ⬜ TODO | |
| 8.5 libAetherPlugin | ⬜ TODO | |
| 8.6 libAetherAmp | ⬜ TODO | |
| 8.7 libAetherFX | ⬜ TODO | |
| 8.8 Signal Chain + MIDI | ⬜ TODO | |
| 8.9 Skeuomorphic UI | ⬜ TODO | |
| 8.10 AetherGuitar App | ⬜ TODO | |
