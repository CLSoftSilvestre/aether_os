/*
 * AetherOS — libAetherFX: Guitar effects header (Phase 8.7)
 * File: userspace/lib/libAetherFX/include/afx.h
 *
 * 25 guitar effects, each implemented as an aplug_t plugin.
 * All register via afx_register_all().
 *
 * Plugin IDs:
 *   Utility:     os.aether.tuner, os.aether.noisegate, os.aether.boost
 *   Dynamics:    os.aether.comp, os.aether.limiter
 *   Distortion:  os.aether.overdrive, os.aether.distortion, os.aether.fuzz
 *   Filter:      os.aether.eq4, os.aether.wah, os.aether.autowah
 *   Modulation:  os.aether.chorus, os.aether.flanger, os.aether.phaser,
 *                os.aether.tremolo, os.aether.vibrato, os.aether.rotary
 *   Delay:       os.aether.delay, os.aether.ppdelay
 *   Reverb:      os.aether.plate, os.aether.spring
 *   Pitch:       os.aether.pitchshift, os.aether.octave
 *   Special:     os.aether.ringmod, os.aether.looper
 */

#ifndef AETHER_AFX_H
#define AETHER_AFX_H

#include "aplug.h"

/* Register all 25 effects into the global plugin registry. */
void afx_register_all(void);

/* Individual factory functions (also callable directly). */
aplug_t *afx_tuner_factory(void);
aplug_t *afx_noisegate_factory(void);
aplug_t *afx_boost_factory(void);
aplug_t *afx_comp_factory(void);
aplug_t *afx_limiter_factory(void);
aplug_t *afx_overdrive_factory(void);
aplug_t *afx_distortion_factory(void);
aplug_t *afx_fuzz_factory(void);
aplug_t *afx_eq4_factory(void);
aplug_t *afx_wah_factory(void);
aplug_t *afx_autowah_factory(void);
aplug_t *afx_chorus_factory(void);
aplug_t *afx_flanger_factory(void);
aplug_t *afx_phaser_factory(void);
aplug_t *afx_tremolo_factory(void);
aplug_t *afx_vibrato_factory(void);
aplug_t *afx_rotary_factory(void);
aplug_t *afx_delay_factory(void);
aplug_t *afx_ppdelay_factory(void);
aplug_t *afx_plate_factory(void);
aplug_t *afx_spring_factory(void);
aplug_t *afx_pitchshift_factory(void);
aplug_t *afx_octave_factory(void);
aplug_t *afx_ringmod_factory(void);
aplug_t *afx_looper_factory(void);

#endif /* AETHER_AFX_H */
