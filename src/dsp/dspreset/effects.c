#include "effects.h"

#include <math.h>
#include <string.h>

float ds_fx_param(const ds_effect_t *fx, const char *name, float fallback) {
    for (unsigned i = 0; i < fx->param_count; ++i)
        if (!strcmp(fx->param_names[i], name)) return fx->param_values[i];
    return fallback;
}

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

void ds_fx_process(const ds_fx_coeffs_t *c, ds_fx_state_t *s, float *lr, unsigned frames) {
    switch (c->kind) {
    case DS_FX_GAIN:
        for (unsigned i = 0; i < frames * 2; ++i) lr[i] *= c->gain;
        break;
    case DS_FX_ONEPOLE:
        for (unsigned ch = 0; ch < 2; ++ch) {
            float y = s->z1[ch];
            for (unsigned i = 0; i < frames; ++i) { y = lr[2 * i + ch] + c->pole * (y - lr[2 * i + ch]); lr[2 * i + ch] = y; }
            s->z1[ch] = fabsf(y) < 1e-20f ? 0 : y;           /* no denormals in the tail */
        }
        break;
    case DS_FX_BIQUAD:
        for (unsigned ch = 0; ch < 2; ++ch) {                /* transposed direct form II */
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
