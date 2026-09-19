/* Bit crusher, gate and compressor, through the engine at instrument level:
 * each exactly transparent where it should be, and each doing its job to the
 * sample (crusher, gate) or the tenth of a dB (compressor). */
#include "test_support.h"

#include "../src/dsp/dspreset/native_engine.h"

#define SR 44100
#define N (SR / 2)

static int g_channel = 0, g_pan = 0;    /* which channel render() keeps; the sample's pan */

static void render(const char *dir, const char *name, const char *effect, float *out) {
    static ds_native_engine_t e;
    char path[512], error[128], xml[1024];
    float block[256];
    snprintf(path, sizeof(path), "%s/ld/%s.dspreset", dir, name);
    snprintf(xml, sizeof(xml), "<DecentSampler><groups><group><sample path=\"%s\" rootNote=\"60\" ampEnvEnabled=\"false\" pan=\"%d\"/></group></groups>"
                               "<effects>%s</effects></DecentSampler>", strstr(name, "sine") ? "sine.wav" : strstr(name, "step") ? "dcstep.wav" : strstr(name, "dc") ? "dc.wav" : "sig.wav", g_pan, effect);
    write_text(path, xml);
    CHECK(ds_native_engine_load(&e, path, SR, NULL, NULL, error, sizeof(error)) == 0);
    for (int i = 0; i < 4; ++i) e.amp_override[i] = -1;
    ds_native_engine_note_on(&e, 60, 127);
    for (int at = 0; at < N; at += 128) {
        memset(block, 0, sizeof(block));
        ds_native_engine_render(&e, block, 128);
        for (int i = 0; i < 128 && at + i < N; ++i) out[at + i] = block[2 * i + g_channel];
    }
    ds_native_engine_destroy(&e);
}

/* A steady level (24-bit mono): the compressor's curve, without a follower's
 * ripple. From frame `drop` on, `after` instead. */
static void write_dc24(const char *path, unsigned frames, double level, unsigned drop, double after) {
    FILE *f = fopen(path, "wb");
    int32_t v = (int32_t)lrint(level * 8388607.0), w = (int32_t)lrint(after * 8388607.0);
    CHECK(f);
    fwrite("RIFF", 1, 4, f); put32(f, 36 + frames * 3); fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f); put32(f, 16); put16(f, 1); put16(f, 1); put32(f, SR); put32(f, SR * 3); put16(f, 3); put16(f, 24);
    fwrite("data", 1, 4, f); put32(f, frames * 3);
    for (unsigned i = 0; i < frames; ++i) {
        int32_t x = i < drop ? v : w;
        fputc(x & 255, f); fputc((x >> 8) & 255, f); fputc((x >> 16) & 255, f);
    }
    fclose(f);
}

static double peak_db(const float *x, int from, int to) {
    float p = 0;
    for (int i = from; i < to; ++i) if (fabsf(x[i]) > p) p = fabsf(x[i]);
    return 20 * log10(p);
}

int main(void) {
    const char *dir = getenv("TEST_TMP");
    char path[512];
    static float dry[N], wet[N], sdry[N], swet[N];
    CHECK(dir);
    snprintf(path, sizeof(path), "mkdir -p '%s/ld'", dir); CHECK(system(path) == 0);
    snprintf(path, sizeof(path), "%s/ld/sig.wav", dir); write_wav24(path, SR, 1, N + 1000, -1, -1);
    snprintf(path, sizeof(path), "%s/ld/sine.wav", dir); write_sine24(path, N + 1000, 1000, 0.5);   /* -6 dBFS */
    snprintf(path, sizeof(path), "%s/ld/dc.wav", dir); write_dc24(path, N + 1000, 0.5, ~0u, 0);   /* -6 dBFS, steady */
    snprintf(path, sizeof(path), "%s/ld/dcstep.wav", dir); write_dc24(path, N + 1000, 0.5, 11025, 0.1);  /* then -20 dBFS */
    render(dir, "sig-dry", "", dry);
    render(dir, "sine-dry", "", sdry);

    /* bit crusher */
    render(dir, "sig-bc0", "<effect type=\"bit_crusher\"/>", wet);           /* 24 bits, x1, mix 1 */
    CHECK(!memcmp(wet, dry, sizeof(dry)));
    render(dir, "sig-bc", "<effect type=\"bit_crusher\" bitDepth=\"2\" sampleRateReduction=\"4\"/>", wet);
    for (int i = 0; i < N; ++i) CHECK(wet[i] == floorf(dry[i & ~3] * 2 + 0.5f) / 2);   /* every 4th, in halves */
    render(dir, "sig-bcm", "<effect type=\"bit_crusher\" bitDepth=\"3\" mix=\"0.25\"/>", wet);
    for (int i = 0; i < N; ++i) CHECK(fabsf(wet[i] - (0.75f * dry[i] + 0.25f * floorf(dry[i] * 4 + 0.5f) / 4)) < 1e-6f);

    /* gate */
    render(dir, "sig-g0", "<effect type=\"gate\" amount=\"0\"/>", wet);
    CHECK(!memcmp(wet, dry, sizeof(dry)));
    render(dir, "sig-g1", "<effect type=\"gate\" amount=\"1\"/>", wet);
    for (int i = 221; i < N; ++i) CHECK(wet[i] == 0.0f);                     /* shut within 5 ms, and stays */
    CHECK(wet[100] != 0.0f);
    render(dir, "sig-gh", "<effect type=\"gate\" amount=\"0.5\"/>", wet);
    {   /* the same coin flips, the same fades */
        unsigned rng = DS_GATE_SEED, open = 0, shut = 0;
        float gain = 1, target = 1, stepv = 1.0f / (0.005f * SR);
        for (int i = 0; i < N; ++i) {
            if (i % 2205 == 0) { target = ds_gate_draw(&rng) < 0.5f ? 0.0f : 1.0f; if (target > 0) open++; else shut++; }
            if (gain < target) { gain += stepv; if (gain > target) gain = target; }
            else if (gain > target) { gain -= stepv; if (gain < target) gain = target; }
            CHECK(fabsf(wet[i] - dry[i] * gain) < 1e-6f);
        }
        printf("  gate at 0.5: %u windows open, %u shut\n", open, shut);
        CHECK(open >= 3 && shut >= 3);
    }

    /* compressor: a steady -6 dBFS through -12 dB at 4:1 settles at -10.5; a
     * 1 kHz sine a little higher (its follower rides below the peaks) */
    render(dir, "dc-c", "<effect type=\"compressor\" threshold=\"-12\" ratio=\"4\" attack=\"5\" release=\"100\"/>", swet);
    printf("  compressor: a steady -6 dBFS in, %.3f dBFS out (want -10.500)\n", peak_db(swet, N - 4410, N));
    CHECK(fabs(peak_db(swet, N - 4410, N) + 10.5) < 0.01);
    render(dir, "sine-c", "<effect type=\"compressor\" threshold=\"-12\" ratio=\"4\" attack=\"5\" release=\"100\"/>", swet);
    printf("  and a -6 dBFS sine: %.2f dBFS\n", peak_db(swet, N - 4410, N));
    CHECK(peak_db(swet, N - 4410, N) < -9.0 && peak_db(swet, N - 4410, N) > -10.5);
    render(dir, "sine-c1", "<effect type=\"compressor\" threshold=\"-12\" ratio=\"1\"/>", swet);   /* 1:1 */
    CHECK(!memcmp(swet, sdry, sizeof(sdry)));
    render(dir, "sine-c20", "<effect type=\"compressor\" threshold=\"0\" ratio=\"8\"/>", swet);    /* under the threshold */
    CHECK(!memcmp(swet, sdry, sizeof(sdry)));
    render(dir, "sine-cg", "<effect type=\"compressor\" threshold=\"0\" ratio=\"8\" inputGain=\"-6\" outputGain=\"6\"/>", swet);
    for (int i = 0; i < N; ++i) CHECK(fabsf(swet[i] - sdry[i] * powf(10, -6 / 20.0f) * powf(10, 6 / 20.0f)) < 1e-6f);
    render(dir, "sine-ca", "<effect type=\"compressor\" threshold=\"-12\" ratio=\"4\" attack=\"200\"/>", swet);
    printf("  a 200 ms attack lets the first 5 ms through at %.2f dBFS\n", peak_db(swet, 0, 220));
    CHECK(peak_db(swet, 0, 220) > -7.0);
    /* the attack: a step to 0.5 is followed with a 20 ms time constant, so
     * 882 frames in the level is 0.5 (1 - 1/e) and the gain follows from it */
    render(dir, "dc-att", "<effect type=\"compressor\" threshold=\"-12\" ratio=\"4\" attack=\"20\"/>", swet);
    {
        double level = 0.5 * (1 - exp(-1.0)), thr = pow(10, -12 / 20.0), want = 0.5 * pow(level / thr, 1 / 4.0 - 1);
        printf("  a 20 ms attack, 882 frames into a step: %.4f (want %.4f)\n", swet[881], want);
        CHECK(fabs(swet[881] - want) < 2e-3);
    }
    /* the release: 0.5 falls to 0.1 at frame 11025; the follower lets go with
     * a 100 ms time constant, so 2000 frames later it is 0.1 + 0.4 e^(-2000/4410) */
    render(dir, "step-rel", "<effect type=\"compressor\" threshold=\"-12\" ratio=\"4\" release=\"100\"/>", swet);
    {
        double level = 0.1 + 0.4 * exp(-2000.0 / 4410), thr = pow(10, -12 / 20.0), want = 0.1 * pow(level / thr, 1 / 4.0 - 1);
        printf("  100 ms release, 2000 frames after a drop: %.5f (want %.5f)\n", swet[11025 + 1999], want);
        CHECK(fabs(swet[11025 + 1999] - want) < 5e-4);
    }
    /* stereo-linked: the loud RIGHT channel sets the gain for both */
    g_pan = 100; g_channel = 1;
    render(dir, "dc-right", "<effect type=\"compressor\" threshold=\"-12\" ratio=\"4\"/>", swet);
    CHECK(fabs(peak_db(swet, N - 4410, N) + 10.5) < 0.01);
    g_pan = 0; g_channel = 0;
    /* autoBypass: under the threshold its +6 dB makeup fades away; over it, it is back */
    render(dir, "sine-cab", "<effect type=\"compressor\" threshold=\"0\" ratio=\"4\" outputGain=\"6\" autoBypass=\"true\"/>", swet);
    for (int i = 2206; i < N; ++i) CHECK(swet[i] == sdry[i]);
    render(dir, "dc-cab2", "<effect type=\"compressor\" threshold=\"-12\" ratio=\"4\" autoBypass=\"true\"/>", swet);
    CHECK(fabs(peak_db(swet, N - 4410, N) + 10.5) < 0.01);

    /* knobs reach them */
    {
        static ds_native_engine_t e;
        char error[128];
        float block[256];
        snprintf(path, sizeof(path), "%s/ld/knobs.dspreset", dir);
        write_text(path, "<DecentSampler><ui><tab>"
                         "<labeled-knob minValue=\"1\" maxValue=\"32\" value=\"1\"><binding type=\"effect\" level=\"instrument\" position=\"0\" parameter=\"FX_SAMPLE_RATE_REDUCTION\"/></labeled-knob>"
                         "<labeled-knob minValue=\"0\" maxValue=\"1\" value=\"0\"><binding type=\"effect\" level=\"instrument\" position=\"1\" parameter=\"FX_GATE_AMOUNT\"/></labeled-knob>"
                         "<labeled-knob minValue=\"-60\" maxValue=\"0\" value=\"0\"><binding type=\"effect\" level=\"instrument\" position=\"2\" parameter=\"FX_THRESHOLD\"/></labeled-knob>"
                         "<labeled-knob minValue=\"1\" maxValue=\"20\" value=\"4\"><binding type=\"effect\" level=\"instrument\" position=\"2\" parameter=\"FX_RATIO\"/></labeled-knob>"
                         "<labeled-knob minValue=\"-24\" maxValue=\"24\" value=\"0\"><binding type=\"effect\" level=\"instrument\" position=\"2\" parameter=\"FX_OUTPUT_GAIN\"/></labeled-knob>"
                         "</tab></ui><groups><group><sample path=\"sig.wav\" rootNote=\"60\"/></group></groups>"
                         "<effects><effect type=\"bit_crusher\"/><effect type=\"gate\" amount=\"0\"/><effect type=\"compressor\"/></effects></DecentSampler>");
        CHECK(ds_native_engine_load(&e, path, SR, NULL, NULL, error, sizeof(error)) == 0);
        ds_native_engine_set_control(&e, 0, 8);
        ds_native_engine_set_control(&e, 1, 0.75f);
        ds_native_engine_set_control(&e, 2, -30);
        ds_native_engine_set_control(&e, 3, 10);
        ds_native_engine_set_control(&e, 4, 3);
        memset(block, 0, sizeof(block));
        ds_native_engine_render(&e, block, 128);
        CHECK(e.fx_coeffs[0].reduction == 8 && e.fx_coeffs[1].amount == 0.75f);
        CHECK(e.fx_coeffs[2].threshold == -30 && e.fx_coeffs[2].ratio == 10 && e.fx_coeffs[2].output == 3);
        ds_native_engine_destroy(&e);
    }

    puts("lofi and dynamics test passed");
    return 0;
}
