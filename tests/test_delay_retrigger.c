/* A sample's start delay (delay / delayUnit: seconds, samples, beats at the
 * host's tempo) sample-exact, the envelope waiting through it; retriggering
 * (retriggerEnabled / retriggerInterval) repeating each sample at exact
 * frames while the key is held, and stopping when it is released; and the
 * host's tempo reaching "beats" through the real plugin. */
#include "test_support.h"

#include "../src/dsp/dspreset/native_engine.h"

#define SR 44100

static float sig(uint64_t f) { return test_signal24(f, 0) / 8388607.0f; }

static void load(ds_native_engine_t *e, const char *dir, const char *xml) {
    char path[512], error[128];
    snprintf(path, sizeof(path), "%s/dr/p.dspreset", dir);
    write_text(path, xml);
    CHECK(ds_native_engine_load(e, path, SR, NULL, NULL, error, sizeof(error)) == 0);
    for (int i = 0; i < 4; ++i) e->amp_override[i] = -1;
}

/* Renders `n` frames of note 60 into `out` (mono, the left channel). */
static void play(ds_native_engine_t *e, float *out, unsigned n, unsigned release_at) {
    float block[256];
    ds_native_engine_note_on(e, 60, 127);
    for (unsigned at = 0; at < n; at += 128) {
        if (at == release_at) ds_native_engine_note_off(e, 60);
        memset(block, 0, sizeof(block));
        ds_native_engine_render(e, block, 128);
        for (int i = 0; i < 128 && at + i < n; ++i) out[at + i] = block[2 * i];
    }
}

static void settle(plugin_t *p) {
    for (int i = 0; i < 500; ++i) { usleep(10000); if (i > 20 && !plugin_uint(p, "is_loading")) break; }
}

int main(void) {
    const char *dir = getenv("TEST_TMP");
    char path[2048];
    static ds_native_engine_t e;
    static float out[40000];
    CHECK(dir);
    snprintf(path, sizeof(path), "mkdir -p '%s/dr'", dir); CHECK(system(path) == 0);
    snprintf(path, sizeof(path), "%s/dr/long.wav", dir); write_wav24(path, SR, 1, 30000, -1, -1);
    snprintf(path, sizeof(path), "%s/dr/hit.wav", dir); write_wav24(path, SR, 1, 1000, -1, -1);

    /* 10 ms, 100 samples, half a beat at 60 BPM: silence, then the file from its start */
    {
        static const struct { const char *attrs; unsigned onset; float bpm; } cases[] = {
            {"delay=\"0.01\"", 441, 120}, {"delay=\"100\" delayUnit=\"samples\"", 100, 120},
            {"delay=\"0.5\" delayUnit=\"beats\"", 22050, 60}, {"delay=\"0.01\" delayUnit=\"seconds\"", 441, 120}};
        for (unsigned c = 0; c < sizeof(cases) / sizeof(cases[0]); ++c) {
            char xml[512];
            snprintf(xml, sizeof(xml), "<DecentSampler><groups><group><sample path=\"long.wav\" rootNote=\"60\" ampEnvEnabled=\"false\" %s/></group></groups></DecentSampler>", cases[c].attrs);
            load(&e, dir, xml);
            e.bpm = cases[c].bpm;
            play(&e, out, cases[c].onset + 512, ~0u);
            for (unsigned i = 0; i < cases[c].onset + 512; ++i)
                CHECK(fabsf(out[i] - (i < cases[c].onset ? 0.0f : sig(i - cases[c].onset))) < 1e-5f);
            ds_native_engine_destroy(&e);
        }
    }
    /* the envelope waits too: a 10 ms attack starts at the onset, not before */
    load(&e, dir, "<DecentSampler><groups attack=\"0.01\"><group><sample path=\"long.wav\" rootNote=\"60\" delay=\"0.01\"/></group></groups></DecentSampler>");
    play(&e, out, 2000, ~0u);
    CHECK(out[440] == 0.0f && fabsf(out[441 + 220] - 0.5f * sig(220)) < 0.01f);   /* half-way up at 5 ms in */
    ds_native_engine_destroy(&e);

    /* retrigger every 50 ms (2205 frames) after a 10 ms delay: onsets at 441 + 2205 k */
    load(&e, dir, "<DecentSampler><groups><group><sample path=\"hit.wav\" rootNote=\"60\" ampEnvEnabled=\"false\" delay=\"0.01\""
                  " retriggerEnabled=\"true\" retriggerInterval=\"0.05\" retriggerIntervalUnit=\"seconds\"/></group></groups></DecentSampler>");
    play(&e, out, 20000, 12800);                               /* released at frame 12800 */
    {
        int onsets = 0;
        for (unsigned i = 0; i < 20000; ++i) {
            unsigned k = i < 441 ? 0 : (i - 441) / 2205, from = 441 + 2205 * k;
            int sounding = i >= 441 && i - from < 1000 && from < 12800 + 128;
            float want = sounding ? sig(i - from) : 0.0f;
            if (fabsf(out[i] - want) >= 1e-5f) { fprintf(stderr, "frame %u: %g, want %g\n", i, out[i], want); CHECK(0); }
            if (sounding && i == from) onsets++;
        }
        printf("  retrigger every 2205 frames after 441: %d onsets before the release, none after\n", onsets);
        CHECK(onsets == 6);                                    /* 441, 2646, 4851, 7056, 9261, 11466 */
    }
    ds_native_engine_destroy(&e);
    /* an interval shorter than a block: several repeats in one, overlapping */
    load(&e, dir, "<DecentSampler><groups><group><sample path=\"hit.wav\" rootNote=\"60\" ampEnvEnabled=\"false\" delay=\"441\" delayUnit=\"samples\""
                  " retriggerEnabled=\"true\" retriggerInterval=\"50\" retriggerIntervalUnit=\"samples\"/></group></groups></DecentSampler>");
    play(&e, out, 640, ~0u);
    for (unsigned i = 0; i < 640; ++i) {
        float want = 0;
        for (unsigned from = 441; from <= i; from += 50) want += sig(i - from);
        CHECK(fabsf(out[i] - want) < 1e-5f);
    }
    ds_native_engine_destroy(&e);
    /* the default interval unit is beats: 1 beat at 120 BPM = 22050 frames */
    load(&e, dir, "<DecentSampler><groups><group><sample path=\"hit.wav\" rootNote=\"60\" retriggerEnabled=\"true\" retriggerInterval=\"1\"/></group></groups></DecentSampler>");
    ds_native_engine_note_on(&e, 60, 127);
    CHECK(e.retrig_count == 1 && e.retrig[0].every == 22050);
    ds_native_engine_destroy(&e);

    /* through the plugin: the host's tempo (60 BPM) makes a beat a second */
    {
        char mod[512];
        plugin_t p;
        int16_t pcm[256];
        long first = -1;
        snprintf(mod, sizeof(mod), "%s/dr-module", dir);
        snprintf(path, sizeof(path), "rm -rf '%s' && mkdir -p '%s/instruments/D'", mod, mod); CHECK(system(path) == 0);
        snprintf(path, sizeof(path), "%s/instruments/D/long.wav", mod); write_wav24(path, SR, 1, 30000, -1, -1);
        snprintf(path, sizeof(path), "%s/instruments/D/1 D.dspreset", mod);
        write_text(path, "<DecentSampler><groups><group><sample path=\"long.wav\" rootNote=\"60\" ampEnvEnabled=\"false\""
                         " delay=\"0.25\" delayUnit=\"beats\"/></group></groups></DecentSampler>");
        g_test_bpm = 60.0f;
        plugin_open_in(&p, mod);
        usleep(600000);
        settle(&p);
        plugin_render(&p, pcm);                                /* the tempo reaches the engine */
        plugin_midi(&p, 0x90, 60, 127);
        for (long at = 0; at < 22050 && first < 0; at += 128) {
            plugin_render(&p, pcm);
            for (int i = 0; i < 128 && first < 0; ++i) if (pcm[2 * i]) first = at + i;
        }
        printf("  a quarter beat at the host's 60 BPM: first sound at frame %ld (want ~11025)\n", first);
        CHECK(first >= 11025 && first < 11025 + 8);            /* the file's first frames are small */
        plugin_close(&p);
        g_test_bpm = 120.0f;
    }

    puts("delay and retrigger test passed");
    return 0;
}
