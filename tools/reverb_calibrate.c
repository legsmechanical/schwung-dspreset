/* Calibrates the plate (src/dsp/dspreset/reverb.c) against the reverb
 * DecentSampler actually runs: JUCE's Freeverb (juce::Reverb, ISC licence),
 * reimplemented here as a REFERENCE ONLY — it is never built into the module.
 *
 * For a DecentSampler roomSize/damping it measures, on each reverb's impulse
 * response: RT60 (T30 x2 from the Schroeder energy-decay curve) of the whole
 * signal, and of a 4 kHz band vs a 300 Hz band (how fast the highs die), and
 * the wet level for noise in. Then it prints what the plate's mapping should be.
 *
 *   cc -O2 -Isrc/dsp tools/reverb_calibrate.c src/dsp/dspreset/reverb.c -lm -o /tmp/rc && /tmp/rc
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dspreset/reverb.h"

#define SR 44100
#define LEN (SR * 12)

/* ---- reference: juce::Reverb ------------------------------------------- */
typedef struct { float *b; int n, i; float last; } comb_t;
typedef struct { float *b; int n, i; } ap_t;
typedef struct { comb_t c[2][8]; ap_t a[2][4]; float feedback, damp1, damp2, wet1, dry, gain; } fv_t;

static void fv_init(fv_t *f, float room, float damping, float wet) {
    static const int ct[8] = {1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617}, at[4] = {556, 441, 341, 225};
    for (int ch = 0; ch < 2; ++ch) {
        for (int k = 0; k < 8; ++k) { f->c[ch][k].n = ct[k] + ch * 23; f->c[ch][k].b = calloc(f->c[ch][k].n, 4); f->c[ch][k].i = 0; f->c[ch][k].last = 0; }
        for (int k = 0; k < 4; ++k) { f->a[ch][k].n = at[k] + ch * 23; f->a[ch][k].b = calloc(f->a[ch][k].n, 4); f->a[ch][k].i = 0; }
    }
    f->feedback = room * 0.28f + 0.7f;
    f->damp1 = damping * 0.4f; f->damp2 = 1.0f - f->damp1;
    f->wet1 = wet * 3.0f;           /* width 1: wet2 = 0 */
    f->dry = 0; f->gain = 0.015f;
}

static float comb(comb_t *c, float x, const fv_t *f) {
    float out = c->b[c->i];
    c->last = out * f->damp2 + c->last * f->damp1;
    c->b[c->i] = x + c->last * f->feedback;
    if (++c->i >= c->n) c->i = 0;
    return out;
}
static float apass(ap_t *a, float x) {
    float out = a->b[a->i];
    a->b[a->i] = x + out * 0.5f;
    if (++a->i >= a->n) a->i = 0;
    return out - x;
}
static void fv_process(fv_t *f, float *lr, int frames) {
    for (int i = 0; i < frames; ++i) {
        float in = (lr[2 * i] + lr[2 * i + 1]) * f->gain, o[2] = {0, 0};
        for (int ch = 0; ch < 2; ++ch) {
            for (int k = 0; k < 8; ++k) o[ch] += comb(&f->c[ch][k], in, f);
            for (int k = 0; k < 4; ++k) o[ch] = apass(&f->a[ch][k], o[ch]);
        }
        lr[2 * i] = o[0] * f->wet1; lr[2 * i + 1] = o[1] * f->wet1;
    }
}
static void fv_free(fv_t *f) {
    for (int ch = 0; ch < 2; ++ch) { for (int k = 0; k < 8; ++k) free(f->c[ch][k].b); for (int k = 0; k < 4; ++k) free(f->a[ch][k].b); }
}

/* ---- measurement --------------------------------------------------------- */

/* RT60 of the left channel from its energy-decay curve: time from -5 to -35 dB, x2. */
static double rt60(const float *lr, int n) {
    double *edc = malloc(n * sizeof(double)), total = 0, t5 = -1, t35 = -1;
    for (int i = n - 1; i >= 0; --i) { total += (double)lr[2 * i] * lr[2 * i]; edc[i] = total; }
    for (int i = 0; i < n; ++i) {
        double db = 10 * log10(edc[i] / total + 1e-30);
        if (t5 < 0 && db <= -5) t5 = i;
        if (t35 < 0 && db <= -35) { t35 = i; break; }
    }
    free(edc);
    return t35 < 0 ? -1 : 2.0 * (t35 - t5) / SR;
}

/* A 2nd-order band-pass (RBJ, Q 1) over the left channel, in place. */
static void bandpass(float *lr, int n, double hz) {
    double w = 2 * M_PI * hz / SR, al = sin(w) / 2, a0 = 1 + al;
    double b0 = al / a0, b2 = -al / a0, a1 = -2 * cos(w) / a0, a2 = (1 - al) / a0, z1 = 0, z2 = 0;
    for (int i = 0; i < n; ++i) {
        double x = lr[2 * i], y = b0 * x + z1;
        z1 = -a1 * y + z2; z2 = b2 * x - a2 * y; lr[2 * i] = (float)y;
    }
}

typedef struct { double rt, rt_lo, rt_hi, level; } measure_t;

static int g_raw = 0;
static float g_raw_decay, g_raw_damp;

static measure_t measure(int plate, float room, float damping) {
    static float ir[2 * LEN], band[2 * LEN], noise[2 * SR * 2];
    measure_t m;
    double s = 0;
    unsigned seed = 1;
    memset(ir, 0, sizeof(ir));
    ir[0] = ir[1] = 1.0f;
    if (plate) {
        ds_reverb_t *r = ds_reverb_create(SR);
        if (g_raw) ds_reverb_set_raw(r, g_raw_decay, g_raw_damp, 1.0f);
        else ds_reverb_set(r, room, damping, 1.0f);
        /* the plate ADDS wet to dry: render from silence with a separate impulse */
        {
            static float buf[2 * 128];
            for (int b = 0; b < LEN; b += 128) {
                memset(buf, 0, sizeof(buf));
                if (b == 0) buf[0] = buf[1] = 1.0f;
                ds_reverb_process(r, buf, 128);
                memcpy(ir + 2 * b, buf, sizeof(buf));
            }
        }
        ds_reverb_clear(r);
        for (int i = 0; i < 2 * SR * 2; ++i) { seed = seed * 1664525u + 1013904223u; noise[i] = 0; }
        for (int i = 0; i < SR * 2; ++i) { seed = seed * 1664525u + 1013904223u; noise[2 * i] = noise[2 * i + 1] = ((seed >> 8) / 16777216.0f - 0.5f); }
        {   /* wet only: subtract the dry input the plate leaves in place */
            static float buf[2 * 128];
            for (int b = 0; b < SR * 2; b += 128) {
                memcpy(buf, noise + 2 * b, sizeof(buf));
                ds_reverb_process(r, buf, 128);
                for (int k = 0; k < 256; ++k) noise[2 * b + k] = buf[k] - noise[2 * b + k];
            }
        }
        ds_reverb_destroy(r);
    } else {
        fv_t f;
        fv_init(&f, room, damping, 1.0f);
        fv_process(&f, ir, LEN);
        fv_free(&f);
        fv_init(&f, room, damping, 1.0f);
        for (int i = 0; i < SR * 2; ++i) { seed = seed * 1664525u + 1013904223u; noise[2 * i] = noise[2 * i + 1] = ((seed >> 8) / 16777216.0f - 0.5f); }
        fv_process(&f, noise, SR * 2);
        fv_free(&f);
    }
    for (int i = SR; i < SR * 2; ++i) s += (double)noise[2 * i] * noise[2 * i];
    m.level = sqrt(s / SR);
    m.rt = rt60(ir, LEN);
    memcpy(band, ir, sizeof(band)); bandpass(band, LEN, 300); m.rt_lo = rt60(band, LEN);
    memcpy(band, ir, sizeof(band)); bandpass(band, LEN, 4000); m.rt_hi = rt60(band, LEN);
    return m;
}

/* plate measured with raw settings */
static measure_t plate_raw(float decay, float damp) {
    measure_t m;
    g_raw = 1; g_raw_decay = decay; g_raw_damp = damp;
    m = measure(1, 0, 0);
    g_raw = 0;
    return m;
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "fit")) {
        /* 1. decay: the raw decay whose RT matches Freeverb's, damping 0 */
        printf("decay table (room 0.0 .. 1.0 step 0.1):\n");
        for (int k = 0; k <= 10; ++k) {
            float room = k / 10.0f, lo = 0.0f, hi = 0.99f;
            double target = measure(0, room, 0).rt;
            for (int it = 0; it < 18; ++it) {
                float mid = 0.5f * (lo + hi);
                if (plate_raw(mid, 0).rt < target) lo = mid; else hi = mid;
            }
            printf("  room %.1f  Freeverb RT %.2f s  ->  decay %.4f  (plate RT %.2f)\n", room, target, 0.5f * (lo + hi), plate_raw(0.5f * (lo + hi), 0).rt);
        }
        return 0;
    }
    if (argc > 1 && !strcmp(argv[1], "level")) {
        /* 3. loudness: Freeverb's wet level over the plate's, same DecentSampler settings */
        double sum = 0, lo = 1e9, hi = 0;
        int n = 0;
        for (float room = 0.1f; room <= 1.001f; room += 0.2f)
            for (float damp = 0.0f; damp <= 0.91f; damp += 0.3f) {
                measure_t f = measure(0, room, damp), p = measure(1, room, damp);
                double ratio = f.level / p.level;
                printf("  room %.1f damp %.1f: Freeverb %.3f plate %.3f ratio %.3f  | RT %.2f vs %.2f, hi/lo %.2f vs %.2f\n",
                       room, damp, f.level, p.level, ratio, f.rt, p.rt, f.rt_hi / f.rt_lo, p.rt_hi / p.rt_lo);
                sum += log(ratio); n++;
                if (ratio < lo) lo = ratio;
                if (ratio > hi) hi = ratio;
            }
        printf("geometric mean ratio %.4f (range %.3f .. %.3f)\n", exp(sum / n), lo, hi);
        return 0;
    }
    if (argc > 1 && !strcmp(argv[1], "damp")) {
        /* 2. damping: the raw tank damping whose hi/lo RT ratio matches, at the fitted decay */
        printf("damping table (damping 0.0 .. 1.0 step 0.1), room 0.7:\n");
        for (int k = 0; k <= 10; ++k) {
            float d = k / 10.0f, lo = 0.0f, hi = 0.95f;
            measure_t f = measure(0, 0.7f, d);
            double target = f.rt_hi / f.rt_lo;
            for (int it = 0; it < 16; ++it) {
                float mid = 0.5f * (lo + hi);
                measure_t p = plate_raw(ds_reverb_decay_for_room(0.7f), mid);
                if (p.rt_hi / p.rt_lo > target) lo = mid; else hi = mid;
            }
            printf("  damping %.1f  Freeverb hi/lo %.3f  ->  tank damping %.4f\n", d, target, 0.5f * (lo + hi));
        }
        return 0;
    }

    printf("room damp | Freeverb RT  lo/hi   level | plate RT  lo/hi   level\n");
    for (float room = 0.1f; room <= 1.001f; room += 0.2f) {
        for (float damp = 0.0f; damp <= 0.61f; damp += 0.3f) {
            measure_t f = measure(0, room, damp), p = measure(1, room, damp);
            printf("%.1f  %.1f  | %5.2f  %5.2f/%5.2f %.3f | %5.2f  %5.2f/%5.2f %.3f\n",
                   room, damp, f.rt, f.rt_lo, f.rt_hi, f.level, p.rt, p.rt_lo, p.rt_hi, p.level);
        }
    }
    return 0;
}
