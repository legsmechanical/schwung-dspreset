#include "delay.h"
#include "ramp.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define RAMP_SECONDS 0.05
#define MAX_FEEDBACK 0.99f
#define SILENT_LEVEL 1e-9f      /* this small counts as nothing (-180 dB) */

struct ds_delay {
    float *line[2];
    unsigned size, write;
    float sample_rate;
    ramp_t time[2], feedback, wet;  /* times in samples */
    int ramp_steps, primed, idle;
    unsigned quiet;                 /* frames of silence in and silence written */
};

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

ds_delay_t *ds_delay_create(float sample_rate, float max_seconds) {
    ds_delay_t *d = calloc(1, sizeof(*d));
    if (!d) return NULL;
    d->size = (unsigned)ceilf(clampf(max_seconds, 0, 30) * sample_rate) + 3;
    if (!(d->line[0] = malloc((size_t)d->size * 2 * sizeof(float)))) { free(d); return NULL; }
    /* written, not just allocated: the audio thread never takes a first-touch fault */
    memset(d->line[0], 0, (size_t)d->size * 2 * sizeof(float));
    d->line[1] = d->line[0] + d->size;
    d->sample_rate = sample_rate;
    d->ramp_steps = (int)floor(RAMP_SECONDS * sample_rate);
    d->idle = 1;
    ds_delay_set(d, 0.7f, 0.0f, 0.2f, 0.5f);     /* DecentSampler's defaults */
    d->primed = 0;                               /* the caller's first setting lands at once */
    return d;
}

void ds_delay_destroy(ds_delay_t *d) {
    if (!d) return;
    free(d->line[0]);
    free(d);
}

void ds_delay_set(ds_delay_t *d, float time, float stereo_offset, float feedback, float wet) {
    int snap = !d->primed;
    float longest = (float)(d->size - 2);
    d->primed = 1;
    for (int ch = 0; ch < 2; ++ch) {
        float samples = (time + (ch ? 0.5f : -0.5f) * stereo_offset) * d->sample_rate, whole = roundf(samples);
        if (fabsf(samples - whole) < 1e-3f) samples = whole;   /* 0.09 s is 3969 samples, not 3968.9998 */
        ramp_to(&d->time[ch], clampf(samples, 1, longest), d->ramp_steps, snap);
    }
    ramp_to(&d->feedback, clampf(feedback, 0, MAX_FEEDBACK), d->ramp_steps, snap);
    ramp_to(&d->wet, clampf(wet, 0, 1), d->ramp_steps, snap);
}

int ds_delay_idle(const ds_delay_t *d) { return d->idle; }

float ds_delay_longest(const ds_delay_t *d) { return (float)(d->size - 3) / d->sample_rate; }

void ds_delay_process(ds_delay_t *d, float *lr, unsigned frames) {
    int silent = 1;
    if (d->idle) {
        unsigned i = 0;
        while (i < 2 * frames && fabsf(lr[i]) < SILENT_LEVEL) ++i;
        if (i == 2 * frames) {               /* still nothing: skip, settings land */
            ramp_land(&d->time[0]);
            ramp_land(&d->time[1]);
            ramp_land(&d->feedback);
            ramp_land(&d->wet);
            return;
        }
        d->idle = 0;
        d->quiet = 0;
    }
    for (unsigned i = 0; i < frames; ++i) {
        float feedback = ramp_next(&d->feedback), wet = ramp_next(&d->wet);
        for (int ch = 0; ch < 2; ++ch) {
            float *line = d->line[ch], in = lr[2 * i + ch];
            float time = ramp_next(&d->time[ch]), frac, newer, older, echo, back;
            unsigned whole = (unsigned)time, at;
            frac = time - (float)whole;
            /* `time` samples back: between the sample that many back and the
             * one before it (whole >= 1, so both were written earlier) */
            at = d->write >= whole ? d->write - whole : d->write + d->size - whole;
            newer = line[at];
            older = line[at ? at - 1 : d->size - 1];
            echo = newer + frac * (older - newer);
            back = in + echo * feedback;
            if (fabsf(back) < SILENT_LEVEL) back = 0;    /* no denormals in a dying tail */
            else silent = 0;
            line[d->write] = back;
            lr[2 * i + ch] = in + echo * wet;
        }
        if (++d->write == d->size) d->write = 0;
    }
    /* Once every slot has been rewritten with silence, the line holds
     * nothing: skip until something arrives. */
    if (!silent) d->quiet = 0;
    else if ((d->quiet += frames) > d->size) d->idle = 1;
}
