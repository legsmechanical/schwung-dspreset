#include "compressor.h"
#include "ramp.h"

#include <math.h>
#include <stdlib.h>

struct ds_compressor {
    float sample_rate;
    float threshold, exponent;          /* linear threshold; gain = (level/threshold)^exponent above it */
    float attack, release;              /* follower coefficients */
    float input, output;                /* linear */
    int auto_bypass;
    float level;                        /* the follower */
    ramp_t engaged;                     /* autoBypass: 1 in, 0 out */
    int ramp_steps;
};

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
static float coef(float ms, float rate) { return expf(-1.0f / (ms * 0.001f * rate)); }

ds_compressor_t *ds_compressor_create(float sample_rate) {
    ds_compressor_t *c = calloc(1, sizeof(*c));
    if (!c) return NULL;
    c->sample_rate = sample_rate;
    c->ramp_steps = (int)floor(0.05 * sample_rate);
    ds_compressor_set(c, -12, 4, 5, 100, 0, 0, 0);     /* DecentSampler's defaults */
    ramp_to(&c->engaged, 1.0f, 0, 1);
    return c;
}

void ds_compressor_destroy(ds_compressor_t *c) { free(c); }

void ds_compressor_set(ds_compressor_t *c, float threshold_db, float ratio, float attack_ms, float release_ms,
                       float input_db, float output_db, int auto_bypass) {
    c->threshold = powf(10.0f, clampf(threshold_db, -60, 0) / 20.0f);
    c->exponent = 1.0f / clampf(ratio, 1, 20) - 1.0f;
    c->attack = coef(clampf(attack_ms, 0.1f, 200), c->sample_rate);
    c->release = coef(clampf(release_ms, 5, 2000), c->sample_rate);
    c->input = input_db == 0 ? 1.0f : powf(10.0f, clampf(input_db, -24, 24) / 20.0f);
    c->output = output_db == 0 ? 1.0f : powf(10.0f, clampf(output_db, -24, 24) / 20.0f);
    c->auto_bypass = auto_bypass;
    if (!auto_bypass) ramp_to(&c->engaged, 1.0f, 0, 1);
}

void ds_compressor_process(ds_compressor_t *c, float *lr, unsigned frames) {
    for (unsigned i = 0; i < frames; ++i) {
        float l = lr[2 * i] * c->input, r = lr[2 * i + 1] * c->input;
        float peak = fabsf(l) > fabsf(r) ? fabsf(l) : fabsf(r), gain = c->output, on;
        c->level = peak > c->level ? peak + c->attack * (c->level - peak) : peak + c->release * (c->level - peak);
        if (c->level < 1e-20f) c->level = 0;
        if (c->level > c->threshold && c->exponent != 0) gain *= powf(c->level / c->threshold, c->exponent);
        if (c->auto_bypass) ramp_to(&c->engaged, c->level > c->threshold ? 1.0f : 0.0f, c->ramp_steps, 0);
        on = ramp_next(&c->engaged);
        if (on == 1.0f) { lr[2 * i] = l * gain; lr[2 * i + 1] = r * gain; }
        else { lr[2 * i] = lr[2 * i] * (1.0f - on) + l * gain * on; lr[2 * i + 1] = lr[2 * i + 1] * (1.0f - on) + r * gain * on; }
    }
}
