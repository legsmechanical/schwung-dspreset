#ifndef DSPRESET_REVERB_H
#define DSPRESET_REVERB_H

/* DecentSampler's <effect type="reverb">, rendered as a plate after Jon
 * Dattorro, "Effect Design Part 1: Reverberator and Other Filters" (JAES,
 * 1997), written from the paper: input diffusion into a figure-eight tank of
 * two modulated all-pass/delay halves. Denser and smoother than the Freeverb
 * DecentSampler itself uses (JUCE's), at a fraction of the work: one shared
 * network instead of 8 combs + 4 all-passes per channel.
 *
 * roomSize, damping and wetLevel keep DecentSampler's meaning. They are mapped
 * so a setting rings about as long, darkens about as fast and sits about as
 * loud as JUCE's Freeverb does at the same values (calibrated with
 * tools/reverb_calibrate.c against a reference Freeverb). The dry signal is
 * untouched: wetLevel's default of 0 is silence, as in DecentSampler.
 *
 * Buffers are allocated once (worker); processing allocates nothing. */

typedef struct ds_reverb ds_reverb_t;

ds_reverb_t *ds_reverb_create(float sample_rate);
void ds_reverb_destroy(ds_reverb_t *reverb);
void ds_reverb_clear(ds_reverb_t *reverb);
/* DecentSampler's parameters, 0..1 each. Cheap: may be called every block. */
void ds_reverb_set(ds_reverb_t *reverb, float room_size, float damping, float wet_level);
/* Adds the wet signal to interleaved stereo `lr` in place. */
void ds_reverb_process(ds_reverb_t *reverb, float *lr, unsigned frames);

/* The plate's own settings, bypassing the DecentSampler mapping: for the
 * calibration tool. decay 0..~0.99, damping 0..1 (tank low-pass), wet linear. */
void ds_reverb_set_raw(ds_reverb_t *reverb, float decay, float damping, float wet);

/* The mapping, exposed for the calibration tool and tests. */
float ds_reverb_decay_for_room(float room_size);
float ds_reverb_damping_for(float damping);

#endif
