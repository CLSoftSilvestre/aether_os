/*
 * AetherOS — CPU Affinity & Isolation (Phase 8.0)
 * File: kernel/core/cpu_affinity.c
 *
 * On the Pi 5 (4× Cortex-A76), core 3 is reserved for audio processing when
 * audio is active.  The affinity mask in task_t is already consulted by
 * sched_rt_find_next() when selecting the next RT task.
 *
 * SMP note: AetherOS currently boots only a single core (core 0) in QEMU and
 * on the Pi target.  This file records the architectural intent and provides
 * the kernel API so audio code can call it unconditionally.  When SMP
 * bringup is added, cpu_affinity_set_audio_isolation() will write the GIC
 * IRQ routing register and the per-core PSCI boot address.
 */

#include "aether/sched.h"
#include "aether/printk.h"
#include "aether/types.h"

/* ── Module state ────────────────────────────────────────────────────── */

static int g_audio_isolation = 0;   /* 1 = core 3 reserved for audio    */

/* ── Public API ──────────────────────────────────────────────────────── */

/*
 * cpu_affinity_init — called from kernel_main during boot.
 * On single-core QEMU this is a no-op; on Pi 5 it would set up the GIC
 * target routing for audio IRQs.
 */
void cpu_affinity_init(void)
{
    g_audio_isolation = 0;
    kinfo("cpu_affinity: initialised (4-core mask; SMP not yet active)\n");
}

/*
 * cpu_affinity_set_audio_isolation — reserve core 3 for audio.
 *
 * When enabled:
 *   - All new tasks default to CPU_MASK_ALL & ~CPU_MASK_CORE3 (cores 0-2).
 *   - The audio server task is pinned to CPU_MASK_CORE3.
 *   - xHCI USB IRQ should be routed to core 3 (requires GIC ITARGETSR write).
 *
 * On current single-core QEMU this records intent only; the scheduler's
 * cpu_affinity field in task_t is still respected in the RT pick path.
 */
void cpu_affinity_set_audio_isolation(int enable)
{
    g_audio_isolation = enable ? 1 : 0;
    kinfo("cpu_affinity: audio isolation %s\n",
          g_audio_isolation ? "enabled (core 3 reserved)" : "disabled");
}

int cpu_affinity_audio_isolation(void)
{
    return g_audio_isolation;
}
