#include "gate.h"
#include "ramp.h"

#include <math.h>
#include <stdlib.h>

struct ds_gate {
    float amount, gain, target, fade_step;
    unsigned window, until_next, rng;
    ramp_t mix;
    int ramp_steps, primed;
};

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

float ds_gate_draw(unsigned *state) {
    *state = *state * 1664525u + 1013904223u;
    return (float)(*state >> 8) / 16777216.0f;          /* 0 <= u < 1 */
}

ds_gate_t *ds_gate_create(float sample_rate) {
    ds_gate_t *g = calloc(1, sizeof(*g));
    if (!g) return NULL;
    g->window = (unsigned)lrintf(0.05f * sample_rate);
    g->fade_step = 1.0f / (0.005f * sample_rate);
    g->gain = g->target = 1.0f;
    g->rng = DS_GATE_SEED;
    g->ramp_steps = (int)floor(0.05 * sample_rate);
    ds_gate_set(g, 0.5f, 1.0f);                         /* DecentSampler's defaults */
    g->primed = 0;
    return g;
}

void ds_gate_destroy(ds_gate_t *g) { free(g); }

void ds_gate_set(ds_gate_t *g, float amount, float mix) {
    int snap = !g->primed;
    g->primed = 1;
    g->amount = clampf(amount, 0, 1);
    ramp_to(&g->mix, clampf(mix, 0, 1), g->ramp_steps, snap);
}

void ds_gate_process(ds_gate_t *g, float *lr, unsigned frames) {
    for (unsigned i = 0; i < frames; ++i) {
        float mix = ramp_next(&g->mix), wet;
        if (g->until_next == 0) {                       /* a new window: flip the coin */
            g->target = ds_gate_draw(&g->rng) < g->amount ? 0.0f : 1.0f;
            g->until_next = g->window;
        }
        g->until_next--;
        if (g->gain < g->target) { g->gain += g->fade_step; if (g->gain > g->target) g->gain = g->target; }
        else if (g->gain > g->target) { g->gain -= g->fade_step; if (g->gain < g->target) g->gain = g->target; }
        wet = g->gain * mix + (1.0f - mix);
        lr[2 * i] *= wet;
        lr[2 * i + 1] *= wet;
    }
}
