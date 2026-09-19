#include "bitcrusher.h"
#include "ramp.h"

#include <math.h>
#include <stdlib.h>

struct ds_bitcrusher {
    float bits, steps;          /* steps = 2^(bits-1); 0 = no rounding */
    unsigned hold, held;        /* keep one sample in `hold`; frames since the last kept */
    float kept[2];
    ramp_t mix;
    int ramp_steps, primed;
};

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

ds_bitcrusher_t *ds_bitcrusher_create(float sample_rate) {
    ds_bitcrusher_t *c = calloc(1, sizeof(*c));
    if (!c) return NULL;
    c->ramp_steps = (int)floor(0.05 * sample_rate);
    ds_bitcrusher_set(c, 24, 1, 1);             /* DecentSampler's defaults */
    c->primed = 0;
    return c;
}

void ds_bitcrusher_destroy(ds_bitcrusher_t *c) { free(c); }

void ds_bitcrusher_set(ds_bitcrusher_t *c, float bit_depth, float reduction, float mix) {
    int snap = !c->primed;
    c->primed = 1;
    c->bits = clampf(bit_depth, 1, 24);
    /* 24 bits is a float's own precision: rounding there would only disturb it */
    c->steps = c->bits >= 24 ? 0 : powf(2.0f, c->bits - 1);
    c->hold = (unsigned)lrintf(clampf(reduction, 1, 32));
    ramp_to(&c->mix, clampf(mix, 0, 1), c->ramp_steps, snap);
}

void ds_bitcrusher_process(ds_bitcrusher_t *c, float *lr, unsigned frames) {
    if (!c->steps && c->hold == 1 && c->mix.target == 1.0f && c->mix.left <= 0) { c->held = 0; return; }   /* nothing to do */
    for (unsigned i = 0; i < frames; ++i) {
        float mix = ramp_next(&c->mix);
        if (c->held == 0)
            for (int ch = 0; ch < 2; ++ch) {
                float x = lr[2 * i + ch];
                c->kept[ch] = c->steps ? floorf(x * c->steps + 0.5f) / c->steps : x;
            }
        if (++c->held >= c->hold) c->held = 0;
        for (int ch = 0; ch < 2; ++ch) lr[2 * i + ch] = lr[2 * i + ch] * (1.0f - mix) + c->kept[ch] * mix;
    }
}
