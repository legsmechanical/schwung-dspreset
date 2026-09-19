#ifndef DSPRESET_COMPRESSOR_H
#define DSPRESET_COMPRESSOR_H

/* DecentSampler's <effect type="compressor">: threshold (dB), ratio, attack
 * and release (ms), inputGain and outputGain (dB), autoBypass, with
 * "stereo-linked" detection. Our own: a peak follower on the louder channel
 * (after the input gain), gain reduced by (level - threshold) x (1 - 1/ratio)
 * dB above the threshold, both channels by the same amount. autoBypass fades
 * the whole effect out (50 ms) while the follower is under the threshold and
 * back in when it crosses. Ratio 1 and 0 dB gains are an exact pass-through.
 * Instrument level only. */

typedef struct ds_compressor ds_compressor_t;

ds_compressor_t *ds_compressor_create(float sample_rate);
void ds_compressor_destroy(ds_compressor_t *comp);
void ds_compressor_set(ds_compressor_t *comp, float threshold_db, float ratio, float attack_ms, float release_ms,
                       float input_db, float output_db, int auto_bypass);
void ds_compressor_process(ds_compressor_t *comp, float *lr, unsigned frames);

#endif
