/* FLAC samples (real encodes, tests/fixtures/flac): decoded sample for sample
 * against the signal they were made from — 16-bit mono and 24-bit stereo,
 * past the 16384-frame head so the worker streams them; a note restarted
 * (the decoder seeks back); a loop past the head (it seeks on every pass);
 * and through the real plugin, as the Move plays it. */
#include "test_support.h"

#include "../src/dsp/dspreset/native_engine.h"

#define SR 44100

static float expect(int bits16, uint64_t f, unsigned ch) {
    return bits16 ? (float)(test_signal24(f, 0) >> 8) / 32768.0f : (float)test_signal24(f, ch) / 8388608.0f;
}

static void copy(const char *from, const char *to) {
    char cmd[2048];
    snprintf(cmd, sizeof(cmd), "cp '%s' '%s'", from, to);
    CHECK(system(cmd) == 0);
}

/* Plays note 60 for `frames`; the worst difference from the file's frames
 * along a loop of `ls`..`le` (0, 0 = none). */
static double play(ds_native_engine_t *e, int bits16, unsigned frames, uint64_t ls, uint64_t le) {
    float out[256];
    double worst = 0;
    ds_native_engine_note_on(e, 60, 127);
    for (unsigned at = 0; at < frames; at += 128) {
        while (ds_native_engine_service(e)) {}
        memset(out, 0, sizeof(out));
        ds_native_engine_render(e, out, 128);
        for (int i = 0; i < 128 && at + i < frames; ++i) {
            uint64_t vf = at + i, f = le && vf >= le ? ls + (vf - ls) % (le - ls) : vf;
            for (unsigned ch = 0; ch < 2; ++ch) {
                double d = fabs(out[2 * i + ch] - expect(bits16, f, bits16 ? 0 : ch));
                if (d > worst) worst = d;
            }
        }
    }
    ds_native_engine_cc(e, 120, 0);
    return worst;
}

int main(void) {
    const char *dir = getenv("TEST_TMP");
    char path[1024], error[128];
    static ds_native_engine_t e;
    CHECK(dir);
    snprintf(path, sizeof(path), "mkdir -p '%s/flac/instruments/F'", dir); CHECK(system(path) == 0);
    snprintf(path, sizeof(path), "%s/flac/m.flac", dir); copy("tests/fixtures/flac/mono16.flac", path);
    snprintf(path, sizeof(path), "%s/flac/s.flac", dir); copy("tests/fixtures/flac/stereo24.flac", path);

    for (int bits16 = 1; bits16 >= 0; --bits16) {
        double worst;
        snprintf(path, sizeof(path), "%s/flac/%s.dspreset", dir, bits16 ? "m" : "s");
        write_text(path, bits16 ? "<DecentSampler><groups><group><sample path=\"m.flac\" rootNote=\"60\" ampEnvEnabled=\"false\"/></group></groups></DecentSampler>"
                                : "<DecentSampler><groups><group><sample path=\"s.flac\" rootNote=\"60\" ampEnvEnabled=\"false\"/></group></groups></DecentSampler>");
        CHECK(ds_native_engine_load(&e, path, SR, NULL, NULL, error, sizeof(error)) == 0);
        for (int i = 0; i < 4; ++i) e.amp_override[i] = -1;
        CHECK(e.sources[0].file.frame_count == 40000 && e.sources[0].file.channels == (bits16 ? 1 : 2) && e.zones[0].b.streams);
        worst = play(&e, bits16, 39000, 0, 0);
        printf("  %s: 39000 frames, worst %.2e\n", bits16 ? "16-bit mono" : "24-bit stereo", worst);
        CHECK(worst < 1e-6);
        worst = play(&e, bits16, 39000, 0, 0);                  /* again: the voice's decoder starts over */
        CHECK(worst < 1e-6);
        ds_native_engine_destroy(&e);
    }

    /* a loop past the head: 30000..39999, three passes */
    snprintf(path, sizeof(path), "%s/flac/loop.dspreset", dir);
    write_text(path, "<DecentSampler><groups><group><sample path=\"s.flac\" rootNote=\"60\" ampEnvEnabled=\"false\""
                     " loopEnabled=\"true\" loopStart=\"30000\" loopEnd=\"39999\"/></group></groups></DecentSampler>");
    CHECK(ds_native_engine_load(&e, path, SR, NULL, NULL, error, sizeof(error)) == 0);
    for (int i = 0; i < 4; ++i) e.amp_override[i] = -1;
    {
        double worst = play(&e, 0, 30000 + 3 * 10000, 30000, 40000);
        printf("  looped past the head, three passes: worst %.2e\n", worst);
        CHECK(worst < 1e-6);
    }
    ds_native_engine_destroy(&e);

    /* a file cut short (a broken download): it plays what is there, then silence, and nothing breaks */
    snprintf(path, sizeof(path), "head -c 35000 '%s/flac/s.flac' > '%s/flac/cut.flac'", dir, dir); CHECK(system(path) == 0);
    snprintf(path, sizeof(path), "%s/flac/cut.dspreset", dir);
    write_text(path, "<DecentSampler><groups><group><sample path=\"cut.flac\" rootNote=\"60\" ampEnvEnabled=\"false\"/></group></groups></DecentSampler>");
    if (!ds_native_engine_load(&e, path, SR, NULL, NULL, error, sizeof(error))) {
        float out[256];
        ds_native_engine_note_on(&e, 60, 127);
        for (int b = 0; b < 320; ++b) { while (ds_native_engine_service(&e)) {} ds_native_engine_render(&e, out, 128); }
        for (int i = 0; i < 256; ++i) CHECK(fabsf(out[i]) <= 1.0f);
        ds_native_engine_destroy(&e);
    }

    /* the real plugin, at real-time pace (its default gain, 0.7) */
    {
        plugin_t p;
        int16_t pcm[256];
        char status[256];
        int worst = 0;
        snprintf(path, sizeof(path), "%s/flac/s.dspreset", dir);
        plugin_open(&p);
        plugin_load(&p, path, status, sizeof(status));
        CHECK(!strcmp(status, "s.dspreset: 1 zones"));
        plugin_midi(&p, 0x90, 60, 127);
        for (long at = 0; at < 38400; at += 128) {
            plugin_render(&p, pcm);
            for (int i = 0; i < 128; ++i)
                for (int ch = 0; ch < 2; ++ch) {
                    int d = abs(pcm[2 * i + ch] - expected_out(expect(0, (uint64_t)(at + i), (unsigned)ch), 0.7f));
                    if (d > worst) worst = d;
                }
        }
        printf("  through the plugin: 38400 frames, worst %d LSB\n", worst);
        CHECK(worst <= 1);
        plugin_close(&p);
    }

    puts("flac test passed");
    return 0;
}
