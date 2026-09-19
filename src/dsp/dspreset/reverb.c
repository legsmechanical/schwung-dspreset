#include "reverb.h"
#include "ramp.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* JUCE's constants (juce_Reverb.h), unchanged. */
#define COMBS 8
#define ALLPASSES 4
#define STEREO_SPREAD 23
#define INPUT_GAIN 0.015f
#define WET_SCALE 3.0f
#define ROOM_SCALE 0.28f
#define ROOM_OFFSET 0.7f
#define DAMP_SCALE 0.4f
#define RAMP_SECONDS 0.01

static const int COMB_TUNING[COMBS] = {1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617};
static const int ALLPASS_TUNING[ALLPASSES] = {556, 441, 341, 225};

/* The raw network's output below this, with nothing coming in, is idle:
 * at the loudest wet (x3) that is under -130 dBFS. */
#define IDLE_LEVEL 1e-7f
/* Input this small counts as nothing (-180 dB): a filter ahead of the reverb
 * can leave its state decaying toward zero for ever without reaching it. */
#define SILENT_LEVEL 1e-9f
/* The network holds sound for a while before any comes out (the first comb
 * alone is 1116 samples), so a quiet OUTPUT means nothing until it has
 * stayed quiet longer than the longest path through it: twice (the longest
 * comb + all four all-passes). */

typedef struct { float *buf; int size, pos; float last; } comb_t;
typedef struct { float *buf; int size, pos; } allpass_t;

struct ds_reverb {
    comb_t comb[2][COMBS];
    allpass_t allpass[2][ALLPASSES];
    ramp_t damping, feedback, wet;
    int ramp_steps, primed, idle, stale;
    int quiet;                          /* frames of silence in and near-silence out */
    int quiet_needed;
};

static float clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

static inline float comb_process(comb_t *c, float input, float damp, float feedback) {
    float out = c->buf[c->pos];
    c->last = out * (1.0f - damp) + c->last * damp;
    c->buf[c->pos] = input + c->last * feedback;
    if (++c->pos >= c->size) c->pos = 0;
    return out;
}

static inline float allpass_process(allpass_t *a, float input) {
    float held = a->buf[a->pos];
    a->buf[a->pos] = input + held * 0.5f;
    if (++a->pos >= a->size) a->pos = 0;
    return held - input;
}

ds_reverb_t *ds_reverb_create(float sample_rate) {
    ds_reverb_t *r = calloc(1, sizeof(*r));
    int rate = (int)sample_rate;
    if (!r) return NULL;
    for (int ch = 0; ch < 2; ++ch) {
        for (int k = 0; k < COMBS; ++k) {
            comb_t *c = &r->comb[ch][k];
            c->size = rate * (COMB_TUNING[k] + ch * STEREO_SPREAD) / 44100;
            if (c->size < 1) c->size = 1;
            if (!(c->buf = calloc((size_t)c->size, sizeof(float)))) { ds_reverb_destroy(r); return NULL; }
        }
        for (int k = 0; k < ALLPASSES; ++k) {
            allpass_t *a = &r->allpass[ch][k];
            a->size = rate * (ALLPASS_TUNING[k] + ch * STEREO_SPREAD) / 44100;
            if (a->size < 1) a->size = 1;
            if (!(a->buf = calloc((size_t)a->size, sizeof(float)))) { ds_reverb_destroy(r); return NULL; }
        }
    }
    r->ramp_steps = (int)floor(RAMP_SECONDS * sample_rate);
    {
        int path = r->comb[1][COMBS - 1].size;
        for (int k = 0; k < ALLPASSES; ++k) path += r->allpass[1][k].size;
        r->quiet_needed = 2 * path;
    }
    r->idle = 1;
    ds_reverb_set(r, 0.7f, 0.3f, 0.0f);          /* DecentSampler's defaults */
    r->primed = 0;                               /* the caller's first setting lands at once */
    return r;
}

void ds_reverb_destroy(ds_reverb_t *r) {
    if (!r) return;
    for (int ch = 0; ch < 2; ++ch) {
        for (int k = 0; k < COMBS; ++k) free(r->comb[ch][k].buf);
        for (int k = 0; k < ALLPASSES; ++k) free(r->allpass[ch][k].buf);
    }
    free(r);
}

void ds_reverb_clear(ds_reverb_t *r) {
    for (int ch = 0; ch < 2; ++ch) {
        for (int k = 0; k < COMBS; ++k) { memset(r->comb[ch][k].buf, 0, (size_t)r->comb[ch][k].size * sizeof(float)); r->comb[ch][k].last = 0; }
        for (int k = 0; k < ALLPASSES; ++k) memset(r->allpass[ch][k].buf, 0, (size_t)r->allpass[ch][k].size * sizeof(float));
    }
    r->idle = 1;
    r->quiet = 0;
}

void ds_reverb_set(ds_reverb_t *r, float room_size, float damping, float wet_level) {
    /* the first setting lands at once; later ones glide (JUCE: reset() snaps) */
    int snap = !r->primed;
    r->primed = 1;
    ramp_to(&r->damping, clamp01(damping) * DAMP_SCALE, r->ramp_steps, snap);
    ramp_to(&r->feedback, clamp01(room_size) * ROOM_SCALE + ROOM_OFFSET, r->ramp_steps, snap);
    ramp_to(&r->wet, clamp01(wet_level) * WET_SCALE, r->ramp_steps, snap);   /* width 1: wet2 = 0 */
}

int ds_reverb_idle(const ds_reverb_t *r) { return r->idle; }

void ds_reverb_process(ds_reverb_t *r, float *lr, unsigned frames) {
    float peak = 0;
    /* Wet at 0 and faded out: nothing to hear, so nothing is run and the dry
     * passes untouched. JUCE keeps the tail running unheard; here it is
     * dropped instead, once, when the wet comes back up (the memset is
     * ~100 KB, once per knob move off 0). */
    if (r->wet.target == 0.0f && r->wet.left <= 0) {
        r->stale = 1;
        ramp_land(&r->damping);
        ramp_land(&r->feedback);
        return;
    }
    if (r->stale) { r->stale = 0; ds_reverb_clear(r); }
    if (r->idle) {
        unsigned i = 0;
        while (i < 2 * frames && fabsf(lr[i]) < SILENT_LEVEL) ++i;
        if (i == 2 * frames) {               /* still nothing: skip, settings land */
            ramp_land(&r->damping);
            ramp_land(&r->feedback);
            ramp_land(&r->wet);
            return;
        }
        r->idle = 0;
        r->quiet = 0;
    }
    {
        int silent_in = 1;
        for (unsigned i = 0; i < frames; ++i) {
            float in_l = lr[2 * i], in_r = lr[2 * i + 1];
            float input = (in_l + in_r) * INPUT_GAIN, out_l = 0, out_r = 0;
            float damp = ramp_next(&r->damping), feedback = ramp_next(&r->feedback), wet = ramp_next(&r->wet);
            if (fabsf(in_l) >= SILENT_LEVEL || fabsf(in_r) >= SILENT_LEVEL) silent_in = 0;
            for (int k = 0; k < COMBS; ++k) {
                out_l += comb_process(&r->comb[0][k], input, damp, feedback);
                out_r += comb_process(&r->comb[1][k], input, damp, feedback);
            }
            for (int k = 0; k < ALLPASSES; ++k) {
                out_l = allpass_process(&r->allpass[0][k], out_l);
                out_r = allpass_process(&r->allpass[1][k], out_r);
            }
            if (fabsf(out_l) > peak) peak = fabsf(out_l);
            if (fabsf(out_r) > peak) peak = fabsf(out_r);
            lr[2 * i] = in_l + out_l * wet;
            lr[2 * i + 1] = in_r + out_r * wet;
        }
        /* Idle once the output has stayed under -130 dB for longer than any
         * path through the network: what the lines still hold has come out
         * over that window, so it is as quiet. It stays in place and plays
         * out under the next sound. */
        if (!silent_in || peak >= IDLE_LEVEL) r->quiet = 0;
        else if ((r->quiet += (int)frames) >= r->quiet_needed) r->idle = 1;
    }
}
