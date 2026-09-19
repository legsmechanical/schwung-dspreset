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

enum { DS_FX_BYPASS = 0, DS_FX_BIQUAD, DS_FX_ONEPOLE, DS_FX_GAIN, DS_FX_REVERB, DS_FX_CHORUS, DS_FX_DELAY,
       DS_FX_BITCRUSHER, DS_FX_GATE, DS_FX_COMPRESSOR, DS_FX_UNSUPPORTED };

typedef struct {
    int kind;
    float b0, b1, b2, a1, a2;       /* biquad, normalised by a0 */
    float pole;                     /* one-pole low-pass */
    float gain;                     /* gain effect, linear */
    float room, damping, wet;       /* reverb: DecentSampler's settings (reverb.c maps them); delay's wet too */
    float mix, depth, rate;         /* chorus */
    float time, offset, feedback;   /* delay, seconds */
    float bits, reduction;          /* bit crusher (mix above) */
    float amount;                   /* gate (mix above) */
    float threshold, ratio, attack, release, input, output;   /* compressor: dB, x, ms, ms, dB, dB */
    int auto_bypass;
} ds_fx_coeffs_t;

typedef struct { float z1[2], z2[2]; } ds_fx_state_t;

/* Also reports types this build does not render yet (phaser...). The reverb,
 * chorus and delay only carry their settings here: their running state is
 * reverb.c / chorus.c / delay.c, owned by the engine. */
void ds_fx_prepare(ds_fx_coeffs_t *out, const ds_effect_t *fx, float sample_rate);
/* In place on interleaved stereo. Audio thread: no allocation, no locks. */
void ds_fx_process(const ds_fx_coeffs_t *c, ds_fx_state_t *state, float *lr, unsigned frames);
/* The same on the LEFT channel only. Every effect here is linear and per
 * channel, so a mono note can be filtered once and panned afterwards. */
void ds_fx_process_left(const ds_fx_coeffs_t *c, ds_fx_state_t *state, float *lr, unsigned frames);
float ds_fx_param(const ds_effect_t *fx, const char *name, float fallback);
/* DecentSampler's default for an attribute the preset leaves out. */
float ds_fx_default(const char *type, const char *name);

#endif
