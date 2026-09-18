/* <effect type="reverb"> is JUCE's juce::Reverb: its structure (comb and
 * all-pass lengths, stereo spread, input gain, wet scale) pinned sample by
 * sample, its tail length per roomSize, its damping, the 10 ms fade when the
 * wet goes to 0, that it stops work once the tail is gone and wakes for sound,
 * and that a preset's reverb knob reaches it. */
#include "test_support.h"

#include "../src/dsp/dspreset/native_engine.h"

#define SR 44100

/* RT60 (T30 x2) of the left channel of an impulse response. */
static double rt60(const float *lr, int n) {
    double total = 0, acc, t5 = -1, t35 = -1;
    for (int i = 0; i < n; ++i) total += (double)lr[2 * i] * lr[2 * i];
    acc = total;
    for (int i = 0; i < n; ++i) {
        double db = 10 * log10(acc / total + 1e-30);
        if (t5 < 0 && db <= -5) t5 = i;
        if (db <= -35) { t35 = i; break; }
        acc -= (double)lr[2 * i] * lr[2 * i];
    }
    return t35 < 0 ? -1 : 2.0 * (t35 - t5) / SR;
}

/* A second, deliberately plain transcription of juce::Reverb (width 1, no
 * ramps), to hold reverb.c to sample by sample. */
typedef struct { float *b; int n, i; float last; } ref_comb_t;
typedef struct { float *b; int n, i; } ref_ap_t;
static void ref_impulse(float room, float damping, float *out_l, int n) {
    static const int ct[8] = {1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617}, at[4] = {556, 441, 341, 225};
    ref_comb_t c[8];
    ref_ap_t ap[4];
    float fb = room * 0.28f + 0.7f, d1 = damping * 0.4f, d2 = 1.0f - d1;
    for (int k = 0; k < 8; ++k) { c[k].n = ct[k]; c[k].b = calloc(ct[k], sizeof(float)); c[k].i = 0; c[k].last = 0; }
    for (int k = 0; k < 4; ++k) { ap[k].n = at[k]; ap[k].b = calloc(at[k], sizeof(float)); ap[k].i = 0; }
    for (int t = 0; t < n; ++t) {
        float in = t == 0 ? 2.0f * 0.015f : 0.0f, o = 0;
        for (int k = 0; k < 8; ++k) {
            float y = c[k].b[c[k].i];
            c[k].last = y * d2 + c[k].last * d1;
            c[k].b[c[k].i] = in + c[k].last * fb;
            c[k].i = (c[k].i + 1) % c[k].n;
            o += y;
        }
        for (int k = 0; k < 4; ++k) {
            float h = ap[k].b[ap[k].i];
            ap[k].b[ap[k].i] = o + h * 0.5f;
            ap[k].i = (ap[k].i + 1) % ap[k].n;
            o = h - o;
        }
        out_l[t] = o * 3.0f;
    }
    for (int k = 0; k < 8; ++k) free(c[k].b);
    for (int k = 0; k < 4; ++k) free(ap[k].b);
}

/* Wet-only impulse response: an impulse of 1 on both channels, dry removed. */
static float *impulse(float room, float damping, float wet, int n) {
    float *ir = calloc((size_t)n * 2, sizeof(float));
    ds_reverb_t *r = ds_reverb_create(SR);
    CHECK(ir && r);
    ds_reverb_set(r, room, damping, wet);
    ir[0] = ir[1] = 1.0f;
    for (int b = 0; b + 128 <= n; b += 128) ds_reverb_process(r, ir + 2 * b, 128);
    ir[0] -= 1.0f; ir[1] -= 1.0f;
    ds_reverb_destroy(r);
    return ir;
}

/* RT60 of the first difference: the highs' tail. */
static double rt60_highs(const float *ir, int n) {
    float *d = calloc((size_t)n * 2, sizeof(float));
    double rt;
    for (int i = 1; i < n; ++i) d[2 * i] = ir[2 * i] - ir[2 * i - 2];
    rt = rt60(d, n);
    free(d);
    return rt;
}

int main(void) {
    const char *dir = getenv("TEST_TMP");
    const int n = SR * 10;
    float *ir;
    double corr = 0, el = 0, er = 0;
    CHECK(dir);

    /* JUCE's network, sample by sample: input (L+R) x 0.015, wet x 3; the
     * first comb (1116, right 1116+23) arrives through four all-passes, each
     * passing a new sample through negated; the second comb at 1188. */
    ir = impulse(0.5f, 0, 1.0f, SR);
    for (int i = 0; i < 1116; ++i) CHECK(ir[2 * i] == 0.0f);
    for (int i = 0; i < 1139; ++i) CHECK(ir[2 * i + 1] == 0.0f);
    CHECK(fabsf(ir[2 * 1116] - 0.09f) < 1e-6f && fabsf(ir[2 * 1139 + 1] - 0.09f) < 1e-6f);
    for (int i = 1117; i < 1188; ++i) CHECK(ir[2 * i] == 0.0f);
    CHECK(fabsf(ir[2 * 1188] - 0.09f) < 1e-6f);
    free(ir);

    /* all of it, against the plain transcription: two seconds, every sample */
    {
        const int len = 128 * 690;                           /* whole blocks: ~2 s */
        float *want = calloc(len, sizeof(float));
        double worst = 0;
        ir = impulse(0.8f, 0.5f, 1.0f, len);
        ref_impulse(0.8f, 0.5f, want, len);
        for (int i = 0; i < len; ++i) if (fabs(ir[2 * i] - want[i]) > worst) worst = fabs(ir[2 * i] - want[i]);
        printf("  vs plain juce::Reverb transcription: worst difference %g\n", worst);
        CHECK(worst < 1e-6);      /* identical under clang; gcc may fuse a multiply-add differently */
        free(ir); free(want);
    }

    /* tail length per roomSize: feedback 0.7 + 0.28 x roomSize */
    {
        const float rooms[3] = {0.3f, 0.7f, 0.9f};
        const double want[3] = {0.93, 2.06, 4.59};
        for (int k = 0; k < 3; ++k) {
            double got;
            ir = impulse(rooms[k], 0, 1.0f, n);
            got = rt60(ir, n);
            printf("  roomSize %.1f: RT60 %.2f s (want %.2f)\n", rooms[k], got, want[k]);
            CHECK(fabs(got - want[k]) < 0.03 * want[k]);
            free(ir);
        }
    }

    /* damping darkens: the highs die faster, the whole a little */
    {
        float *open = impulse(0.7f, 0, 1.0f, n), *damped = impulse(0.7f, 0.9f, 1.0f, n);
        double ho = rt60_highs(open, n), hd = rt60_highs(damped, n);
        printf("  damping 0 -> 0.9: highs RT60 %.2f -> %.2f s\n", ho, hd);
        CHECK(hd < 0.6 * ho);
        free(open); free(damped);
    }

    /* stereo */
    ir = impulse(0.9f, 0.6f, 1.0f, n);
    for (int i = 0; i < n; ++i) {
        CHECK(isfinite(ir[2 * i]) && isfinite(ir[2 * i + 1]));
        corr += (double)ir[2 * i] * ir[2 * i + 1]; el += (double)ir[2 * i] * ir[2 * i]; er += (double)ir[2 * i + 1] * ir[2 * i + 1];
    }
    printf("  left/right correlation %.3f\n", corr / sqrt(el * er));
    CHECK(fabs(corr / sqrt(el * er)) < 0.3);
    free(ir);

    /* wet to 0 mid-tail: fades over 10 ms (441 samples), then exactly dry;
     * back up, the old tail is gone */
    {
        ds_reverb_t *r = ds_reverb_create(SR);
        float b[2 * 1024];
        int last_wet = -1;
        ds_reverb_set(r, 0.9f, 0, 1.0f);
        memset(b, 0, sizeof(b)); b[0] = b[1] = 1.0f;
        for (int k = 0; k < 40; ++k) { ds_reverb_process(r, b, 1024); memset(b, 0, sizeof(b)); }
        ds_reverb_set(r, 0.9f, 0, 0.0f);
        for (int k = 0; k < 4; ++k) {
            memset(b, 0, sizeof(b));
            ds_reverb_process(r, b, 128);
            for (int i = 0; i < 128; ++i) if (b[2 * i] != 0.0f) last_wet = k * 128 + i;
        }
        printf("  wet 1 -> 0: last wet sample at %d\n", last_wet);
        CHECK(last_wet >= 400 && last_wet < 441);
        memset(b, 0, sizeof(b));
        ds_reverb_process(r, b, 128);                        /* a block at rest: now skipped */
        for (int i = 0; i < 256; ++i) CHECK(b[i] == 0.0f);
        ds_reverb_set(r, 0.9f, 0, 1.0f);
        memset(b, 0, sizeof(b));
        ds_reverb_process(r, b, 1024);
        for (int i = 0; i < 2048; ++i) CHECK(b[i] == 0.0f);
        ds_reverb_destroy(r);
    }

    /* idle: stops once the tail is under -130 dB, never before; wakes for sound */
    {
        ds_reverb_t *r = ds_reverb_create(SR);
        float b[256];
        int blocks = 0;
        double last_peak = 0;
        ds_reverb_set(r, 0.5f, 0.3f, 1.0f);
        CHECK(ds_reverb_idle(r));
        memset(b, 0, sizeof(b)); b[0] = b[1] = 0.5f;
        ds_reverb_process(r, b, 128);
        CHECK(!ds_reverb_idle(r));
        while (!ds_reverb_idle(r) && blocks < SR * 20 / 128) {
            memset(b, 0, sizeof(b));
            ds_reverb_process(r, b, 128);
            last_peak = 0;
            for (int i = 0; i < 256; ++i) if (fabs(b[i]) > last_peak) last_peak = fabs(b[i]);
            ++blocks;
        }
        printf("  idle after %.1f s, last block peak %.1f dB\n", blocks * 128.0 / SR, 20 * log10(last_peak + 1e-30));
        CHECK(ds_reverb_idle(r) && blocks * 128.0 / SR > 2.0 && last_peak < 3.1e-7);
        memset(b, 0, sizeof(b)); b[10] = 0.5f;               /* sound: awake again */
        ds_reverb_process(r, b, 128);
        CHECK(!ds_reverb_idle(r));
        ds_reverb_destroy(r);
    }

    /* through the engine: a preset reverb only ADDS; wet 0 is exactly dry; its knob reaches it */
    {
        static ds_native_engine_t dry, wet;
        char path[512], error[128];
        float a[256], b[256];
        double diff = 0;
        snprintf(path, sizeof(path), "mkdir -p '%s/rv'", dir); CHECK(system(path) == 0);
        snprintf(path, sizeof(path), "%s/rv/t.wav", dir); write_sine24(path, 30000, 440, 0.5);
        snprintf(path, sizeof(path), "%s/rv/dry.dspreset", dir);
        write_text(path, "<DecentSampler><groups attack=\"0\"><group><sample path=\"t.wav\" rootNote=\"60\"/></group></groups></DecentSampler>");
        CHECK(ds_native_engine_load(&dry, path, SR, NULL, NULL, error, sizeof(error)) == 0);
        snprintf(path, sizeof(path), "%s/rv/wet.dspreset", dir);
        write_text(path, "<DecentSampler><ui><tab><labeled-knob minValue=\"0\" maxValue=\"100\" value=\"0\">"
                         "<binding type=\"effect\" level=\"instrument\" position=\"0\" parameter=\"FX_REVERB_WET_LEVEL\" factor=\"0.01\"/>"
                         "</labeled-knob></tab></ui>"
                         "<groups attack=\"0\"><group><sample path=\"t.wav\" rootNote=\"60\"/></group></groups>"
                         "<effects><effect type=\"reverb\" roomSize=\"0.9\" damping=\"0.6\"/></effects></DecentSampler>");
        CHECK(ds_native_engine_load(&wet, path, SR, NULL, NULL, error, sizeof(error)) == 0);
        CHECK(!strcmp(wet.model.controls[0].name, "Reverb") && wet.reverb[0]);
        ds_native_engine_note_on(&dry, 60, 127); ds_native_engine_note_on(&wet, 60, 127);
        for (int blk = 0; blk < 20; ++blk) {                   /* knob at 0: identical to no reverb */
            memset(a, 0, sizeof(a)); memset(b, 0, sizeof(b));
            ds_native_engine_render(&dry, a, 128); ds_native_engine_render(&wet, b, 128);
            for (int i = 0; i < 256; ++i) CHECK(a[i] == b[i]);
        }
        ds_native_engine_set_control(&wet, 0, 60);             /* Reverb up: dry + a tail */
        for (int blk = 0; blk < 60; ++blk) {
            memset(a, 0, sizeof(a)); memset(b, 0, sizeof(b));
            ds_native_engine_render(&dry, a, 128); ds_native_engine_render(&wet, b, 128);
            for (int i = 0; i < 256; ++i) diff += (double)(b[i] - a[i]) * (b[i] - a[i]);
        }
        printf("  preset Reverb knob 0 -> 60: wet adds %.4f rms\n", sqrt(diff / (60 * 256)));
        CHECK(sqrt(diff / (60 * 256)) > 0.02);
        ds_native_engine_destroy(&dry); ds_native_engine_destroy(&wet);
    }
    puts("reverb test passed");
    return 0;
}
