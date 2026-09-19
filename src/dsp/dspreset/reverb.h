#ifndef DSPRESET_REVERB_H
#define DSPRESET_REVERB_H

/* DecentSampler's <effect type="reverb">: JUCE's juce::Reverb (Jezar's
 * Freeverb), which is what DecentSampler runs (confirmed by Josh 2026-09-18).
 * Ported from juce_Reverb.h as released in JUCE 7, under ISC (JUCE 8+ is
 * AGPL; notice in THIRD_PARTY.md):
 * the same 8 combs + 4 all-passes per channel, tunings, stereo spread, input
 * gain, scale factors and 10 ms parameter ramps, so a preset rings as its
 * author heard it.
 *
 * DecentSampler exposes roomSize, damping and wetLevel. Width is JUCE's full
 * 1.0 and the dry signal passes at unity (dryLevel 0.5 in JUCE's terms): a
 * wetLevel of 0 leaves the sound exactly as it was.
 *
 * One addition: once nothing is coming in and the tail is below -130 dB, it
 * stops computing until sound arrives. Inaudible, it saves the whole cost
 * while idle, and the tail never reaches the denormal range.
 *
 * Buffers are allocated once (worker); processing allocates nothing. */

typedef struct ds_reverb ds_reverb_t;

ds_reverb_t *ds_reverb_create(float sample_rate);
void ds_reverb_destroy(ds_reverb_t *reverb);
void ds_reverb_clear(ds_reverb_t *reverb);
/* DecentSampler's parameters, 0..1 each. Cheap: may be called every block;
 * a change glides over 10 ms, as in JUCE. */
void ds_reverb_set(ds_reverb_t *reverb, float room_size, float damping, float wet_level);
/* Interleaved stereo `lr` in place: dry at unity plus the reverb. */
void ds_reverb_process(ds_reverb_t *reverb, float *lr, unsigned frames);
/* 1 while it is skipping work (no input, tail gone). For tests. */
int ds_reverb_idle(const ds_reverb_t *reverb);

#endif
