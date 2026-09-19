#include "chorus.h"
#include "ramp.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define CENTRE_MS 7.0f          /* the sweep's centre */
#define SWING_MS 10.0f          /* at depth 1 the sweep is +/- this */
#define MIN_MS 1.0f             /* the sweep never goes shorter */
#define RAMP_SECONDS 0.05
#define SILENT_LEVEL 1e-9f      /* input this small counts as nothing (-180 dB) */
#define TWO_PI 6.283185307179586

struct ds_chorus {
    float *line[2];
    unsigned mask, write;
    float sample_rate;
    double phase;               /* a float would drift: a slow LFO's step is under its precision */
    ramp_t mix, depth, rate;
    int ramp_steps, primed, idle, stale;
    unsigned quiet;             /* frames of silence in */
};

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

ds_chorus_t *ds_chorus_create(float sample_rate) {
    ds_chorus_t *c = calloc(1, sizeof(*c));
    unsigned need = (unsigned)ceilf((CENTRE_MS + SWING_MS) * sample_rate / 1000.0f) + 2, size = 16;
    if (!c) return NULL;
    while (size < need) size <<= 1;
    if (!(c->line[0] = calloc((size_t)size * 2, sizeof(float)))) { free(c); return NULL; }
    c->line[1] = c->line[0] + size;
    c->mask = size - 1;
    c->sample_rate = sample_rate;
    c->ramp_steps = (int)floor(RAMP_SECONDS * sample_rate);
    c->idle = 1;
    ds_chorus_set(c, 0.5f, 0.2f, 0.2f);          /* DecentSampler's defaults */
    c->primed = 0;                               /* the caller's first setting lands at once */
    return c;
}

void ds_chorus_destroy(ds_chorus_t *c) {
    if (!c) return;
    free(c->line[0]);
    free(c);
}

void ds_chorus_set(ds_chorus_t *c, float mix, float depth, float rate_hz) {
    int snap = !c->primed;
    c->primed = 1;
    ramp_to(&c->mix, clampf(mix, 0, 1), c->ramp_steps, snap);
    ramp_to(&c->depth, clampf(depth, 0, 1), c->ramp_steps, snap);
    ramp_to(&c->rate, clampf(rate_hz, 0, 10), c->ramp_steps, snap);
}

int ds_chorus_idle(const ds_chorus_t *c) { return c->idle; }

void ds_chorus_process(ds_chorus_t *c, float *lr, unsigned frames) {
    const float ms_to_samples = c->sample_rate / 1000.0f;
    const double rate_to_step = TWO_PI / c->sample_rate;
    int silent_in = 1;
    /* Mix at 0 and faded out: the dry is the whole output, so nothing runs.
     * What the line held is dropped when the mix comes back up. */
    if (c->mix.target == 0.0f && c->mix.left <= 0) {
        c->stale = 1;
        ramp_land(&c->depth);
        ramp_land(&c->rate);
        return;
    }
    if (c->stale) {
        c->stale = 0;
        memset(c->line[0], 0, (size_t)(c->mask + 1) * 2 * sizeof(float));
        c->idle = 1;
        c->quiet = 0;
    }
    if (c->idle) {
        unsigned i = 0;
        while (i < 2 * frames && fabsf(lr[i]) < SILENT_LEVEL) ++i;
        if (i == 2 * frames) {               /* still nothing: skip, settings land */
            ramp_land(&c->mix);
            ramp_land(&c->depth);
            ramp_land(&c->rate);
            return;
        }
        c->idle = 0;
        c->quiet = 0;
    }
    for (unsigned i = 0; i < frames; ++i) {
        float mix = ramp_next(&c->mix), depth = ramp_next(&c->depth), rate = ramp_next(&c->rate);
        float lfo = -sinf((float)c->phase);                /* the sweep starts heading short */
        float ms = CENTRE_MS + SWING_MS * depth * lfo, delay;
        unsigned whole, at;
        float frac;
        if (ms < MIN_MS) ms = MIN_MS;
        delay = ms * ms_to_samples;
        whole = (unsigned)delay;
        frac = delay - (float)whole;
        c->phase += rate * rate_to_step;
        while (c->phase >= TWO_PI) c->phase -= TWO_PI;
        at = c->write - whole;
        for (int ch = 0; ch < 2; ++ch) {
            float *line = c->line[ch], in = lr[2 * i + ch], newer, older;
            if (fabsf(in) >= SILENT_LEVEL) silent_in = 0;
            line[c->write & c->mask] = in;
            /* `delay` samples back, between the sample that many back and the
             * one before it */
            newer = line[at & c->mask];
            older = line[(at - 1) & c->mask];
            lr[2 * i + ch] = in * (1.0f - mix) + (newer + frac * (older - newer)) * mix;
        }
        c->write = (c->write + 1) & c->mask;
    }
    /* With no feedback, a line that has taken in only silence for its whole
     * length holds only silence: skip until something arrives. */
    if (!silent_in) c->quiet = 0;
    else if ((c->quiet += frames) > c->mask + 1) c->idle = 1;
}
