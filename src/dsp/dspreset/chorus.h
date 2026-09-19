#ifndef DSPRESET_CHORUS_H
#define DSPRESET_CHORUS_H

/* DecentSampler's <effect type="chorus">, which exposes mix, modDepth and
 * modRate. DecentSampler is a JUCE plugin and those are juce::dsp::Chorus's
 * controls with its other two (centre delay, feedback) left at their
 * defaults, so this chorus BEHAVES as that one does: one sine LFO shared by
 * both channels sweeps a linearly interpolated delay of
 * max(1, 7 + 10 * depth * lfo) ms, the mix is linear (dry * (1 - mix) +
 * wet * mix), and rate, depth and mix glide over 50 ms. ⚠ That DecentSampler
 * runs JUCE's chorus is inferred from the matching controls, not confirmed.
 *
 * Our own code, written from that description: JUCE's chorus source is
 * GPL/AGPL, and none of it is used here.
 *
 * Two additions: at mix 0, once faded, the dry passes untouched and nothing
 * runs; and with nothing coming in and the line silent, it skips its work.
 *
 * The buffer is allocated once (worker); processing allocates nothing. */

typedef struct ds_chorus ds_chorus_t;

ds_chorus_t *ds_chorus_create(float sample_rate);
void ds_chorus_destroy(ds_chorus_t *chorus);
/* mix 0..1, depth 0..1, rate in Hz (0..10). Cheap: may be called every block. */
void ds_chorus_set(ds_chorus_t *chorus, float mix, float depth, float rate_hz);
/* Interleaved stereo `lr` in place. */
void ds_chorus_process(ds_chorus_t *chorus, float *lr, unsigned frames);
/* 1 while it is skipping work. For tests. */
int ds_chorus_idle(const ds_chorus_t *chorus);

#endif
