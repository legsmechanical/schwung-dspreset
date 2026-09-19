#ifndef DSPRESET_DELAY_H
#define DSPRESET_DELAY_H

/* DecentSampler's <effect type="delay">: delayTime (s), stereoOffset (s: half
 * is taken off the left's time and half added to the right's), feedback and
 * wetLevel. DecentSampler documents what these do but not how its delay is
 * built, so this is our own straightforward echo: each channel repeats
 * itself (no ping-pong), the dry passes at unity with the echoes added at
 * wetLevel, and every setting glides over 50 ms, so turning the time knob
 * bends the pitch of the echoes rather than clicking.
 *
 * Feedback is capped at 0.99: at 1.0 echoes of held notes would pile up
 * without limit.
 *
 * One addition: with nothing coming in and the line silent, it skips its
 * work until sound arrives.
 *
 * The line is allocated once (worker) for the longest time the preset can
 * reach; processing allocates nothing. */

typedef struct ds_delay ds_delay_t;

ds_delay_t *ds_delay_create(float sample_rate, float max_seconds);
void ds_delay_destroy(ds_delay_t *delay);
/* Times in seconds, clamped to what the line holds (at least one sample);
 * feedback and wet 0..1. Cheap: may be called every block. */
void ds_delay_set(ds_delay_t *delay, float time, float stereo_offset, float feedback, float wet);
/* Interleaved stereo `lr` in place: dry at unity plus the echoes. */
void ds_delay_process(ds_delay_t *delay, float *lr, unsigned frames);
/* The longest time the line holds, in seconds. For tests. */
float ds_delay_longest(const ds_delay_t *delay);
/* 1 while it is skipping work. For tests. */
int ds_delay_idle(const ds_delay_t *delay);

#endif
