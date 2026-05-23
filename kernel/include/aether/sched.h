/*
 * AetherOS — Real-Time Scheduling Types (Phase 8.0)
 * File: kernel/include/aether/sched.h
 *
 * Scheduling policies and RT support for the audio processing path.
 * Audio threads use SCHED_FIFO priority 80, pinned to CPU core 3.
 */

#ifndef AETHER_SCHED_H
#define AETHER_SCHED_H

#include "aether/types.h"

/* ── Scheduling policies ────────────────────────────────────────────── */

#define SCHED_NORMAL  0   /* default: round-robin cooperative           */
#define SCHED_FIFO    1   /* run until yields/blocks or preempted by    */
                          /* a higher-priority RT task                  */
#define SCHED_RR      2   /* like FIFO but with time-slice rotation     */
                          /* among same-priority RT tasks               */

/* RT priority range: 1 (lowest) – 99 (highest), POSIX convention.
 * Audio server uses 80; most other RT tasks should be < 80.          */
#define SCHED_RT_PRIO_MIN  1
#define SCHED_RT_PRIO_MAX  99
#define SCHED_AUDIO_PRIO   80   /* AetherSound server and audio client  */

/* ── CPU affinity ───────────────────────────────────────────────────── */

/* Pi 5 has 4 Cortex-A76 cores.  Bit N = allowed to run on core N.
 * 0xF = all cores (default).  Audio is pinned to core 3 (bit 3 = 0x8). */
#define CPU_MASK_ALL    0x0F
#define CPU_MASK_CORE0  0x01
#define CPU_MASK_CORE1  0x02
#define CPU_MASK_CORE2  0x04
#define CPU_MASK_CORE3  0x08
#define CPU_AUDIO       CPU_MASK_CORE3   /* audio thread affinity       */

/* ── Syscall numbers (Phase 8.0 RT + Phase 8.1 audio) ──────────────── */
/* Numbers 930-945 are used by display/network/user syscalls.
 * Audio/RT syscalls start at 960 to avoid conflicts.                  */

#define SYS_SCHED_SETPARAM       960  /* (policy, rt_prio) → 0/-1        */
#define SYS_SCHED_SETAFFINITY    961  /* (cpu_mask) → 0/-1               */
#define SYS_MLOCKALL             962  /* () → 0; lock all pages          */
#define SYS_AUDIO_TIMESTAMP      963  /* () → nanoseconds since boot     */
#define SYS_AUDIO_LATENCY_STATS  964  /* (stats_ptr) → 0; fills audio_latency_stats_t */

/* Phase 8.1 audio device syscalls */
#define SYS_AUDIO_ENUM           965  /* (audio_dev_info_t *arr, u32 max) → count */
#define SYS_AUDIO_OPEN           966  /* (dev_index) → handle ≥1 or -1   */
#define SYS_AUDIO_CLOSE          967  /* (handle) → 0/-1                 */
#define SYS_AUDIO_CONFIGURE      968  /* (handle, sample_rate, bit_depth, channels) → 0/-1 */
#define SYS_AUDIO_START          969  /* (handle) → 0/-1                 */
#define SYS_MIDI_READ            970  /* (buf_ptr, max_events) → count   */
#define SYS_MIDI_WRITE           971  /* (buf_ptr, count) → sent or -1   */

/* ── Audio latency statistics (returned by SYS_AUDIO_LATENCY_STATS) ── */

typedef struct {
    u32 callback_count;       /* total audio callbacks fired             */
    u32 xrun_count;           /* callbacks that ran late (xruns)         */
    u64 min_latency_ns;       /* minimum callback jitter in nanoseconds  */
    u64 max_latency_ns;       /* maximum callback jitter                 */
    u64 avg_latency_ns;       /* rolling average jitter                  */
    u64 last_timestamp_ns;    /* CNTPCT timestamp of last callback       */
} audio_latency_stats_t;

/* ── RT scheduler API (implemented in sched_rt.c) ──────────────────── */

/* Apply policy/priority to the calling task (called from syscall.c). */
int  sched_rt_set_policy(u32 pid, int policy, int rt_priority);

/* Apply CPU affinity mask to a task. */
int  sched_rt_set_affinity(u32 pid, u8 cpu_mask);

/* Lock all memory pages of a task (prevent page-fault latency spikes). */
int  sched_rt_mlockall(u32 pid);

/* Return CNTPCT_EL0 as nanoseconds since boot. */
u64  sched_rt_timestamp_ns(void);

/* Fill latency statistics structure. */
void sched_rt_get_latency_stats(audio_latency_stats_t *out);

/* Record an audio callback firing (called by audio driver). */
void sched_rt_record_callback(u64 now_ns, int xrun);

/* Called from scheduler.c — returns index of highest-priority ready RT
 * task in g_tasks[0..n_tasks), or -1 if none ready.
 * Exposed via sched_rt_find_next() so scheduler.c can call it without
 * exposing the task table globally.                                    */
int  sched_rt_find_next(void);

/* Initialise RT subsystem (called from scheduler_init). */
void sched_rt_init(void);

/* ── CPU affinity API (implemented in cpu_affinity.c) ──────────────── */

void cpu_affinity_init(void);
void cpu_affinity_set_audio_isolation(int enable);
int  cpu_affinity_audio_isolation(void);

#endif /* AETHER_SCHED_H */
