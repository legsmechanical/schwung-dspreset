#ifndef DSPRESET_BITCRUSHER_H
#define DSPRESET_BITCRUSHER_H

/* DecentSampler's <effect type="bit_crusher">: bitDepth (1-24), sampleRateReduction
 * (1-32: a value of 4 keeps every fourth sample) and mix. DecentSampler
 * documents what they do, not its formulas; ours: each kept sample is held
 * for N frames and rounded to 2^(bits-1) steps either side of zero; the mix
 * is linear and glides over 50 ms. 24 bits, no reduction and full mix is an
 * exact pass-through. Instrument level only (the guide lists it among the
 * effects that do not run per note). */

typedef struct ds_bitcrusher ds_bitcrusher_t;

ds_bitcrusher_t *ds_bitcrusher_create(float sample_rate);
void ds_bitcrusher_destroy(ds_bitcrusher_t *crusher);
void ds_bitcrusher_set(ds_bitcrusher_t *crusher, float bit_depth, float reduction, float mix);
void ds_bitcrusher_process(ds_bitcrusher_t *crusher, float *lr, unsigned frames);

#endif
