/*
 * AetherOS — libAetherDSP: Parameter Smoother (Phase 8.4)
 * File: userspace/lib/libAetherDSP/src/adsp_smooth.c
 *
 * One-pole lowpass smoother for automatable parameters.
 * adsp_smooth_tick() is inline in adsp.h for zero call overhead.
 */

#include "adsp.h"
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

void adsp_smooth_init(adsp_smooth_t *s,
                      float initial, float time_ms, float sr)
{
    s->target  = initial;
    s->current = initial;
    /* Coefficient for one-pole at cutoff fc = 1000/(2π·time_ms) */
    float fc = 1000.0f / (2.0f * (float)M_PI * time_ms);
    s->coef = 1.0f - expf(-2.0f * (float)M_PI * fc / sr);
}
