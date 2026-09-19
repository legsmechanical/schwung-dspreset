/* Loop crossfades, sample-exact: over the last loopCrossfade frames before the
 * loop end, the audio as far before the loop start fades in (linear, or
 * equal_power — the default) — from the resident head, and from the worker's
 * stream past it. A loop starting at the file's start has nothing before it
 * and keeps a plain loop point (CS-20M and DS The Synths set 9–10 frames
 * there: they must be unchanged). */
#include "test_support.h"

#include "../src/dsp/dspreset/native_engine.h"

#define SR 44100

static float sig(uint64_t f) { return test_signal24(f, 0) / 8388607.0f; }

/* What frame `vf` of the play path must sound like. */
static float expected(uint64_t vf, uint64_t ls, uint64_t le, uint64_t xf, int equal_power) {
    uint64_t len = le - ls, f = vf >= le ? ls + (vf - ls) % len : vf;
    if (xf && f >= le - xf) {
        float t = (float)(f - (le - xf)) / (float)xf, go, gi;
        if (equal_power) { go = cosf(t * (float)M_PI_2); gi = sinf(t * (float)M_PI_2); }
        else { go = 1.0f - t; gi = t; }
        return sig(f) * go + sig(f - len) * gi;
    }
    return sig(f);
}

/* Plays note 60 for `frames`, feeding the worker before every block as the
 * plugin's worker would; returns the worst error against `expected`. */
static double play(ds_native_engine_t *e, unsigned frames, uint64_t ls, uint64_t le, uint64_t xf, int ep, float *first_diff_out) {
    float out[256];
    double worst = 0;
    ds_native_engine_note_on(e, 60, 127);
    for (unsigned at = 0; at < frames; at += 128) {
        while (ds_native_engine_service(e)) {}
        memset(out, 0, sizeof(out));
        ds_native_engine_render(e, out, 128);
        for (int i = 0; i < 128; ++i) {
            double d = fabs(out[2 * i] - expected(at + i, ls, le, xf, ep));
            if (d > worst) worst = d;
            if (first_diff_out) first_diff_out[at + i] = out[2 * i];
        }
    }
    ds_native_engine_note_off(e, 60);
    return worst;
}

static void load(ds_native_engine_t *e, const char *dir, const char *name, const char *xml) {
    char path[512], error[128];
    snprintf(path, sizeof(path), "%s/xf/%s", dir, name);
    write_text(path, xml);
    CHECK(ds_native_engine_load(e, path, SR, NULL, NULL, error, sizeof(error)) == 0);
    for (int i = 0; i < 4; ++i) e->amp_override[i] = -1;
}

int main(void) {
    const char *dir = getenv("TEST_TMP");
    char path[512], xml[1024];
    static ds_native_engine_t e;
    static float plain[60000], faded[60000];
    CHECK(dir);
    snprintf(path, sizeof(path), "mkdir -p '%s/xf'", dir); CHECK(system(path) == 0);
    snprintf(path, sizeof(path), "%s/xf/short.wav", dir); write_wav24(path, SR, 1, 20000, -1, -1);   /* resident */
    snprintf(path, sizeof(path), "%s/xf/long.wav", dir); write_wav24(path, SR, 1, 60000, -1, -1);    /* streams */

    for (int ep = 0; ep < 2; ++ep) {
        const char *mode = ep ? "equal_power" : "linear";
        double worst;
        /* from the head: loop 8000..11999, 1000-frame fade, three passes */
        snprintf(xml, sizeof(xml), "<DecentSampler><groups><group><sample path=\"short.wav\" rootNote=\"60\" ampEnvEnabled=\"false\""
                 " loopEnabled=\"true\" loopStart=\"8000\" loopEnd=\"11999\" loopCrossfade=\"1000\" loopCrossfadeMode=\"%s\"/></group></groups></DecentSampler>", mode);
        load(&e, dir, "head.dspreset", xml);
        CHECK(e.zones[0].b.xf == 1000 && !e.zones[0].b.streams);
        worst = play(&e, 8000 + 3 * 4000, 8000, 12000, 1000, ep, NULL);
        printf("  %-11s from the head: worst %.2e\n", mode, worst);
        CHECK(worst < 1e-5);
        ds_native_engine_destroy(&e);
        /* streamed: loop 30000..49999 past the 16384-frame head, 2000-frame fade */
        snprintf(xml, sizeof(xml), "<DecentSampler><groups><group><sample path=\"long.wav\" rootNote=\"60\" ampEnvEnabled=\"false\""
                 " loopEnabled=\"true\" loopStart=\"30000\" loopEnd=\"49999\" loopCrossfade=\"2000\" loopCrossfadeMode=\"%s\"/></group></groups></DecentSampler>", mode);
        load(&e, dir, "stream.dspreset", xml);
        CHECK(e.zones[0].b.xf == 2000 && e.zones[0].b.streams);
        worst = play(&e, 30000 + 3 * 20000, 30000, 50000, 2000, ep, NULL);
        printf("  %-11s streamed:      worst %.2e\n", mode, worst);
        CHECK(worst < 1e-5);
        ds_native_engine_destroy(&e);
    }
    /* the default is equal_power */
    load(&e, dir, "default.dspreset", "<DecentSampler><groups><group><sample path=\"short.wav\" rootNote=\"60\" loopEnabled=\"true\""
                                      " loopStart=\"8000\" loopEnd=\"11999\" loopCrossfade=\"1000\"/></group></groups></DecentSampler>");
    CHECK(e.zones[0].b.xf_equal_power);
    ds_native_engine_destroy(&e);

    /* a loop from the file's start with a 9-frame fade (CS-20M, DS The
     * Synths) renders exactly as it did without one */
    load(&e, dir, "zero.dspreset", "<DecentSampler><groups><group><sample path=\"short.wav\" rootNote=\"60\" ampEnvEnabled=\"false\""
                                   " loopEnabled=\"true\" loopStart=\"0\" loopEnd=\"4999\"/></group></groups></DecentSampler>");
    play(&e, 20000, 0, 5000, 0, 1, plain);
    ds_native_engine_destroy(&e);
    load(&e, dir, "zero9.dspreset", "<DecentSampler><groups><group><sample path=\"short.wav\" rootNote=\"60\" ampEnvEnabled=\"false\""
                                    " loopEnabled=\"true\" loopStart=\"0\" loopEnd=\"4999\" loopCrossfade=\"9\" loopCrossfadeMode=\"linear\"/></group></groups></DecentSampler>");
    CHECK(e.zones[0].b.xf == 0);
    play(&e, 20000, 0, 5000, 0, 1, faded);
    CHECK(!memcmp(plain, faded, 20000 * sizeof(float)));
    ds_native_engine_destroy(&e);

    puts("loop crossfade test passed");
    return 0;
}
