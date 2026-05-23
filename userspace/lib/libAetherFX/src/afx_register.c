/*
 * AetherOS — libAetherFX: Registration (Phase 8.7)
 * File: userspace/lib/libAetherFX/src/afx_register.c
 *
 * Registers all 25 guitar effects into the global plugin registry.
 * Call afx_register_all() once at startup, after aplug_registry_init().
 */

#include "aplug.h"
#include "afx.h"

void afx_register_all(void)
{
    /* Utility */
    aplug_registry_register(afx_tuner_factory);
    aplug_registry_register(afx_noisegate_factory);
    aplug_registry_register(afx_boost_factory);

    /* Dynamics */
    aplug_registry_register(afx_comp_factory);
    aplug_registry_register(afx_limiter_factory);

    /* Distortion */
    aplug_registry_register(afx_overdrive_factory);
    aplug_registry_register(afx_distortion_factory);
    aplug_registry_register(afx_fuzz_factory);

    /* Filter */
    aplug_registry_register(afx_eq4_factory);
    aplug_registry_register(afx_wah_factory);
    aplug_registry_register(afx_autowah_factory);

    /* Modulation */
    aplug_registry_register(afx_chorus_factory);
    aplug_registry_register(afx_flanger_factory);
    aplug_registry_register(afx_phaser_factory);
    aplug_registry_register(afx_tremolo_factory);
    aplug_registry_register(afx_vibrato_factory);
    aplug_registry_register(afx_rotary_factory);

    /* Delay */
    aplug_registry_register(afx_delay_factory);
    aplug_registry_register(afx_ppdelay_factory);

    /* Reverb */
    aplug_registry_register(afx_plate_factory);
    aplug_registry_register(afx_spring_factory);

    /* Pitch */
    aplug_registry_register(afx_pitchshift_factory);
    aplug_registry_register(afx_octave_factory);

    /* Special */
    aplug_registry_register(afx_ringmod_factory);
    aplug_registry_register(afx_looper_factory);
}
