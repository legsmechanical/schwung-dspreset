#ifndef DSPRESET_EFFECTS_H
#define DSPRESET_EFFECTS_H

/* DecentSampler's filter, EQ and gain effects. An effect's settings (from the
 * model, moved by bindings) become coefficients here; the running state lives
 * with whoever owns the signal — the engine for instrument effects, each voice
 * for group effects (DecentSampler starts a fresh copy per note).
 *
 * Coefficients follow JUCE's IIR formulas (DecentSampler is a JUCE plugin):
 * RBJ biquads, with the peak filter's gain a LINEAR factor at its centre. A
 * low-pass at or above ~Nyquist, a unity peak and a disabled effect are exact
 * pass-throughs, so an effect a preset leaves wide open changes nothing. */

#include "preset_model.h"

enum { DS_FX_BYPASS = 0, DS_FX_BIQUAD, DS_FX_ONEPOLE, DS_FX_GAIN, DS_FX_UNSUPPORTED };

typedef struct {
    int kind;
    float b0, b1, b2, a1, a2;       /* biquad, normalised by a0 */
    float pole;                     /* one-pole low-pass */
    float gain;                     /* gain effect, linear */
} ds_fx_coeffs_t;

typedef struct { float z1[2], z2[2]; } ds_fx_state_t;

/* Also reports types this build does not render yet (reverb, delay...). */
void ds_fx_prepare(ds_fx_coeffs_t *out, const ds_effect_t *fx, float sample_rate);
/* In place on interleaved stereo. Audio thread: no allocation, no locks. */
void ds_fx_process(const ds_fx_coeffs_t *c, ds_fx_state_t *state, float *lr, unsigned frames);
float ds_fx_param(const ds_effect_t *fx, const char *name, float fallback);

#endif
