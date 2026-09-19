#include "effects.h"

#include <math.h>
#include <string.h>

float ds_fx_param(const ds_effect_t *fx, const char *name, float fallback) {
    for (unsigned i = 0; i < fx->param_count; ++i)
        if (!strcmp(fx->param_names[i], name)) return fx->param_values[i];
    return fallback;
}

float ds_fx_default(const char *type, const char *name) {
    static const struct { const char *type, *name; float value; } defaults[] = {
        {"reverb", "roomSize", 0.7f}, {"reverb", "damping", 0.3f}, {"reverb", "wetLevel", 0},
        {"chorus", "mix", 0.5f}, {"chorus", "modDepth", 0.2f}, {"chorus", "modRate", 0.2f},
        {"delay", "delayTime", 0.7f}, {"delay", "stereoOffset", 0}, {"delay", "feedback", 0.2f},
        {"delay", "wetLevel", 0.5f}, {"gain", "level", 0},
        {"peak", "frequency", 10000}, {"notch", "frequency", 10000}, {NULL, "frequency", 22000},
        {NULL, "resonance", 0.7f}, {NULL, "q", 0.7f}, {NULL, "gain", 1}};
    for (unsigned i = 0; i < sizeof(defaults) / sizeof(defaults[0]); ++i)
        if ((!defaults[i].type || !strcmp(defaults[i].type, type)) && !strcmp(defaults[i].name, name)) return defaults[i].value;
    return 0;
}

static float setting(const ds_effect_t *fx, const char *name) { return ds_fx_param(fx, name, ds_fx_default(fx->type, name)); }

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

void ds_fx_prepare(ds_fx_coeffs_t *c, const ds_effect_t *fx, float sr) {
    const char *t = fx->type;
    float nyquist = 0.5f * sr, freq, w0, cw, sw, q, alpha, a0 = 1, b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    memset(c, 0, sizeof(*c));
    c->kind = DS_FX_BYPASS;
    if (!fx->enabled) return;
    if (!strcmp(t, "gain")) {
        /* levelUnit defaults to decibels; a numeric "linear" flag is not an
         * attribute we can carry, so a linear preset sets levelUnit and we see it
         * as the non-numeric string it is -- see preset_model's effect parse. */
        float level = ds_fx_param(fx, "level", 0);
        c->gain = ds_fx_param(fx, "levelLinear", 0) > 0.5f ? level : powf(10.0f, level / 20.0f);
        c->kind = c->gain == 1.0f ? DS_FX_BYPASS : DS_FX_GAIN;
        return;
    }
    if (!strcmp(t, "reverb")) {
        /* DecentSampler's defaults: roomSize 0.7, damping 0.3, wetLevel 0 (silent) */
        c->room = clampf(setting(fx, "roomSize"), 0, 1);
        c->damping = clampf(setting(fx, "damping"), 0, 1);
        c->wet = clampf(setting(fx, "wetLevel"), 0, 1);
        /* at wetLevel 0 the reverb still runs until its 10 ms fade is out;
         * then it skips itself (reverb.c) */
        c->kind = DS_FX_REVERB;
        return;
    }
    if (!strcmp(t, "chorus")) {
        /* DecentSampler's defaults: mix 0.5, modDepth 0.2, modRate 0.2 Hz */
        c->mix = clampf(setting(fx, "mix"), 0, 1);
        c->depth = clampf(setting(fx, "modDepth"), 0, 1);
        c->rate = clampf(setting(fx, "modRate"), 0, 10);
        c->kind = DS_FX_CHORUS;
        return;
    }
    if (!strcmp(t, "delay")) {
        /* Tempo-synced time (delayTimeFormat="musical_time") needs a mapping
         * DecentSampler does not document: left silent rather than guessed. */
        if (ds_fx_param(fx, "musicalTime", 0) > 0.5f) { c->kind = DS_FX_UNSUPPORTED; return; }
        /* defaults: delayTime 0.7 s, stereoOffset 0, feedback 0.2, wetLevel 0.5 */
        c->time = clampf(setting(fx, "delayTime"), 0, 20);
        c->offset = clampf(setting(fx, "stereoOffset"), -10, 10);
        c->feedback = clampf(setting(fx, "feedback"), 0, 1);
        c->wet = clampf(setting(fx, "wetLevel"), 0, 1);
        c->kind = DS_FX_DELAY;
        return;
    }
    if (!strcmp(t, "lowpass_1pl")) {
        freq = ds_fx_param(fx, "frequency", 22000);
        if (freq >= 0.49f * sr) return;
        c->pole = expf(-2.0f * (float)M_PI * clampf(freq, 10, nyquist) / sr);
        c->kind = DS_FX_ONEPOLE;
        return;
    }
    freq = ds_fx_param(fx, "frequency", !strcmp(t, "peak") || !strcmp(t, "notch") ? 10000 : 22000);
    if (!strcmp(t, "lowpass") || !strcmp(t, "lowpass_4pl") || !strcmp(t, "highpass") || !strcmp(t, "bandpass"))
        q = clampf(ds_fx_param(fx, "resonance", 0.7f), 0.025f, 5.0f);
    else if (!strcmp(t, "peak") || !strcmp(t, "notch"))
        q = clampf(ds_fx_param(fx, "q", 0.7f), 0.01f, 18.0f);
    else { c->kind = DS_FX_UNSUPPORTED; return; }
    if ((!strcmp(t, "lowpass") || !strcmp(t, "lowpass_4pl")) && freq >= 0.49f * sr) return;   /* wide open */
    if (!strcmp(t, "highpass") && freq <= 10) return;
    freq = clampf(freq, 10, 0.49f * sr);
    w0 = 2.0f * (float)M_PI * freq / sr;
    cw = cosf(w0);
    sw = sinf(w0);
    alpha = sw / (2.0f * q);
    if (!strcmp(t, "lowpass") || !strcmp(t, "lowpass_4pl")) {
        b0 = (1 - cw) / 2; b1 = 1 - cw; b2 = b0; a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha;
    } else if (!strcmp(t, "highpass")) {
        b0 = (1 + cw) / 2; b1 = -(1 + cw); b2 = b0; a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha;
    } else if (!strcmp(t, "bandpass")) {
        b0 = alpha; b1 = 0; b2 = -alpha; a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha;
    } else if (!strcmp(t, "notch")) {
        b0 = 1; b1 = -2 * cw; b2 = 1; a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha;
    } else {                                                   /* peak: JUCE's A = sqrt(gain) */
        float gain = clampf(ds_fx_param(fx, "gain", 1), 0, 10), A = sqrtf(gain);
        if (gain == 1.0f) return;
        b0 = 1 + alpha * A; b1 = -2 * cw; b2 = 1 - alpha * A;
        a0 = 1 + alpha / A; a1 = -2 * cw; a2 = 1 - alpha / A;
    }
    c->b0 = b0 / a0; c->b1 = b1 / a0; c->b2 = b2 / a0; c->a1 = a1 / a0; c->a2 = a2 / a0;
    c->kind = DS_FX_BIQUAD;
}

static void process_channels(const ds_fx_coeffs_t *c, ds_fx_state_t *s, float *lr, unsigned frames, unsigned channels) {
    switch (c->kind) {
    case DS_FX_GAIN:
        for (unsigned i = 0; i < frames; ++i) for (unsigned ch = 0; ch < channels; ++ch) lr[2 * i + ch] *= c->gain;
        break;
    case DS_FX_ONEPOLE:
        for (unsigned ch = 0; ch < channels; ++ch) {
            float y = s->z1[ch];
            for (unsigned i = 0; i < frames; ++i) { y = lr[2 * i + ch] + c->pole * (y - lr[2 * i + ch]); lr[2 * i + ch] = y; }
            s->z1[ch] = fabsf(y) < 1e-20f ? 0 : y;           /* no denormals in the tail */
        }
        break;
    case DS_FX_BIQUAD:
        for (unsigned ch = 0; ch < channels; ++ch) {         /* transposed direct form II */
            float z1 = s->z1[ch], z2 = s->z2[ch];
            for (unsigned i = 0; i < frames; ++i) {
                float x = lr[2 * i + ch], y = c->b0 * x + z1;
                z1 = c->b1 * x - c->a1 * y + z2;
                z2 = c->b2 * x - c->a2 * y;
                lr[2 * i + ch] = y;
            }
            s->z1[ch] = fabsf(z1) < 1e-20f ? 0 : z1;
            s->z2[ch] = fabsf(z2) < 1e-20f ? 0 : z2;
        }
        break;
    default: break;
    }
}

void ds_fx_process(const ds_fx_coeffs_t *c, ds_fx_state_t *s, float *lr, unsigned frames) {
    process_channels(c, s, lr, frames, 2);
}

void ds_fx_process_left(const ds_fx_coeffs_t *c, ds_fx_state_t *s, float *lr, unsigned frames) {
    process_channels(c, s, lr, frames, 1);
}
