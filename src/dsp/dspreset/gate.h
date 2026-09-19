#ifndef DSPRESET_GATE_H
#define DSPRESET_GATE_H

/* DecentSampler's <effect type="gate">: "a randomized gate. Roughly every 50
 * milliseconds it flips a coin, weighted by amount, on whether to let the
 * signal through or cut it to silence, crossfading smoothly between the two
 * states". amount is the chance a window is silent; mix is linear. Our own:
 * windows of exactly 50 ms, a 5 ms linear crossfade, a fixed-seed generator
 * (the same pattern every load). Instrument level only. */

typedef struct ds_gate ds_gate_t;

ds_gate_t *ds_gate_create(float sample_rate);
void ds_gate_destroy(ds_gate_t *gate);
void ds_gate_set(ds_gate_t *gate, float amount, float mix);
void ds_gate_process(ds_gate_t *gate, float *lr, unsigned frames);
/* The generator, for tests: the chance values it draws, in order. */
float ds_gate_draw(unsigned *state);
#define DS_GATE_SEED 0x2545f491u

#endif
