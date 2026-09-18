/* Drives the REAL plugin (worker thread + streaming) at real-time pace on
 * synthetic samples whose every frame is known, so the output can be checked
 * sample-for-sample across the resident-head -> stream handoff and through
 * loop wraps. */
#include "test_support.h"

#include "../src/dsp/dspreset/native_engine.h"

#define BLOCK 128
#define GAIN 0.7f

static float sig(uint64_t frame, unsigned channel) { return test_signal24(frame, channel) / 8388608.0f; }

static void note(plugin_t *p, int on, int n) { plugin_midi(p, on ? 0x90 : 0x80, (uint8_t)n, on ? 127 : 0); }

/* Renders `seconds`, comparing each frame against `expect(frame, channel)`. */
typedef float (*expect_fn)(uint64_t frame, unsigned channel);
static void render_and_compare(plugin_t *p, double seconds, expect_fn expect, int tolerance, const char *what) {
    int16_t out[BLOCK * 2];
    unsigned blocks = (unsigned)(seconds * 44100 / BLOCK);
    int worst = 0;
    for (unsigned b = 0; b < blocks; ++b) {
        plugin_render(p, out);
        for (unsigned i = 0; i < BLOCK; ++i) for (unsigned c = 0; c < 2; ++c) {
            uint64_t frame = (uint64_t)b * BLOCK + i;
            int diff = abs(out[2 * i + c] - expected_out(expect(frame, c), GAIN));
            if (diff > worst) worst = diff;
            if (diff > tolerance) {
                fprintf(stderr, "FAIL %s: frame %llu ch %u got %d want %d\n", what,
                        (unsigned long long)frame, c, out[2 * i + c], expected_out(expect(frame, c), GAIN));
                exit(1);
            }
        }
    }
    printf("  %s: %u frames match (worst diff %d LSB)\n", what, blocks * BLOCK, worst);
}

static float expect_long(uint64_t f, unsigned c) { (void)c; return sig(f, 0); }       /* mono -> both */
static float expect_pad(uint64_t f, unsigned c) {
    return sig(f < 50000 ? f : 40000 + (f - 40000) % 10000, c);
}
static float expect_48k(uint64_t f, unsigned c) {
    double pos = (double)f * 48000.0 / 44100.0;
    uint64_t i = (uint64_t)pos;
    float frac = (float)(pos - (double)i);
    return sig(i, c) + (sig(i + 1, c) - sig(i, c)) * frac;
}

static void wait_silent(plugin_t *p) {
    int16_t out[BLOCK * 2];
    for (int i = 0; i < 2000 && plugin_uint(p, "voices"); ++i) plugin_render(p, out);
    CHECK(plugin_uint(p, "voices") == 0);
}

static const char *PRESET =
    "<DecentSampler>\n<groups attack=\"0\" decay=\"0\" sustain=\"1\" release=\"0.05\">\n<group>\n"
    "<sample path = \"Samples/long.wav\" rootNote = \"60\" loNote = \"60\" hiNote = \"60\"/>\n"
    "<sample path=\"Samples/pad.wav\" rootNote=\"62\" loNote=\"62\" hiNote=\"62\"/>\n"
    "<sample path=\"Samples/hi48.wav\" rootNote=\"64\" loNote=\"64\" hiNote=\"64\"/>\n"
    "<sample path=\"Samples/gone.wav\" rootNote=\"66\" loNote=\"66\" hiNote=\"66\"/>\n"
    "</group>\n</groups>\n</DecentSampler>\n";

int main(void) {
    const char *dir = getenv("TEST_TMP");
    char path[1024], status[256], cmd[2048];
    int16_t out[BLOCK * 2];
    plugin_t p;
    CHECK(dir);
    snprintf(path, sizeof(path), "mkdir -p '%s/lib/Samples'", dir); CHECK(system(path) == 0);
    snprintf(path, sizeof(path), "%s/lib/Samples/long.wav", dir); write_wav24(path, 44100, 1, 200000, -1, -1);
    snprintf(path, sizeof(path), "%s/lib/Samples/pad.wav", dir);  write_wav24(path, 44100, 2, 60000, 40000, 49999);
    snprintf(path, sizeof(path), "%s/lib/Samples/hi48.wav", dir); write_wav24(path, 48000, 2, 30000, -1, -1);
    snprintf(path, sizeof(path), "%s/lib/main.dspreset", dir);     write_text(path, PRESET);

    plugin_open(&p);
    plugin_load(&p, path, status, sizeof(status));
    printf("  status: %s\n", status);
    CHECK(!strcmp(status, "main.dspreset: 3/4 zones, 1 files missing"));

    /* A: 4.5 s file, far past the 16384-frame resident head: exact through the
     * handoff to the stream and several ring wraps. Frame 0 is the attack. */
    note(&p, 1, 60);
    render_and_compare(&p, 3.0, expect_long, 1, "streamed sample");
    note(&p, 0, 60);
    wait_silent(&p);

    /* B: the file's own smpl loop (40000..49999), beyond the head, held 3 s. */
    note(&p, 1, 62);
    render_and_compare(&p, 3.0, expect_pad, 1, "smpl loop");

    /* C: release ends the voice (0.05 s here) */
    note(&p, 0, 62);
    for (int i = 0; i < 50; ++i) plugin_render(&p, out);
    CHECK(plugin_uint(&p, "voices") == 0);
    for (int i = 0; i < BLOCK * 2; ++i) CHECK(out[i] == 0);

    /* D: a 48 kHz file resampled to 44.1 kHz by linear interpolation */
    note(&p, 1, 64);
    render_and_compare(&p, 0.5, expect_48k, 2, "48k -> 44.1k");
    note(&p, 0, 64);
    wait_silent(&p);

    /* E: a zone whose file is missing is silent, not fatal */
    note(&p, 1, 66);
    plugin_render(&p, out);
    CHECK(plugin_uint(&p, "voices") == 0);
    note(&p, 0, 66);

    CHECK(plugin_uint(&p, "underruns") == 0);

    /* E2: past full scale the output bends instead of squaring off: exact up to
     * 0.9, then strictly between 0.9 and 1.0, and ordered like its input */
    {
        int bent = 0;
        p.api->set_param(p.instance, "gain", "3");
        note(&p, 1, 60);
        for (unsigned b = 0; b < 40; ++b) {
            plugin_render(&p, out);
            for (unsigned i = 0; i < BLOCK; ++i) {
                float x = sig((uint64_t)b * BLOCK + i, 0) * 3.0f;
                float o = out[2 * i] / 32767.0f;
                if (fabsf(x) <= 0.9f) CHECK(out[2 * i] == (int16_t)(x * 32767));
                else {
                    CHECK(fabsf(o) > 0.899f && fabsf(o) <= 1.0f && (o > 0) == (x > 0));
                    if (fabsf(x) > 1.0f && fabsf(x) < 1.05f) CHECK(fabsf(o) < 0.999f);   /* a hard clip would sit AT 1 */
                    bent++;
                }
            }
        }
        note(&p, 0, 60);
        wait_silent(&p);
        p.api->set_param(p.instance, "gain", "0.7");
        printf("  soft clip: %d samples over 0.9 bent, none squared off\n", bent);
        CHECK(bent > 100);
    }

    /* E3: a preset made on a Mac/PC names "samples/..." while the folder is
     * "Samples" (BassForge does): case-insensitive there, NOT on the Move's
     * Linux. Found anyway, streamed from the path as it really is. Only a
     * case-sensitive filesystem can fail this - which is why the suite runs
     * on Linux too. */
    {
        char cased[1024], resolved[1600];
        snprintf(cmd, sizeof(cmd), "mkdir -p '%s/cased/Samples/Sub'", dir); CHECK(system(cmd) == 0);
        snprintf(cased, sizeof(cased), "%s/cased/Samples/Sub/Long.wav", dir); write_wav24(cased, 44100, 1, 200000, -1, -1);
        snprintf(cased, sizeof(cased), "%s/cased/p.dspreset", dir);
        write_text(cased, "<DecentSampler><groups attack=\"0\"><group><sample path=\"samples/sub/long.WAV\" rootNote=\"60\"/>"
                          "</group></groups></DecentSampler>");
        snprintf(cased, sizeof(cased), "%s/cased/samples/sub/long.WAV", dir);
        CHECK(ds_resolve_path_case(cased, resolved, sizeof(resolved)) == 0);
        {   FILE *f = fopen(resolved, "rb"); CHECK(f); fclose(f); }
        snprintf(cased, sizeof(cased), "%s/cased/samples/sub/nope.wav", dir);
        CHECK(ds_resolve_path_case(cased, resolved, sizeof(resolved)) != 0);
        snprintf(cased, sizeof(cased), "%s/cased/p.dspreset", dir);
        plugin_load(&p, cased, status, sizeof(status));
        printf("  status: %s\n", status);
        CHECK(!strcmp(status, "p.dspreset: 1 zones"));
        note(&p, 1, 60);
        render_and_compare(&p, 1.0, expect_long, 1, "case-mismatched path, streamed");
        note(&p, 0, 60);
        wait_silent(&p);
    }

    /* F: a .dslibrary loads its natural-order FIRST preset ("2" before "10") */
    snprintf(cmd, sizeof(cmd),
             "cd '%s' && rm -rf pkg lib.dslibrary lib.dslibrary.unpacked && mkdir -p pkg/Lib && "
             "cp -R lib/Samples pkg/Lib/ && cp lib/main.dspreset 'pkg/Lib/10 - Ten.dspreset' && "
             "cp lib/main.dspreset 'pkg/Lib/2 - Two.dspreset' && cd pkg && python3 -m zipfile -c ../lib.dslibrary Lib", dir);
    CHECK(system(cmd) == 0);
    snprintf(path, sizeof(path), "%s/lib.dslibrary", dir);
    plugin_load(&p, path, status, sizeof(status));
    printf("  status: %s\n", status);
    CHECK(!strncmp(status, "2 - Two.dspreset: 3/4 zones", 27));
    note(&p, 1, 60);
    render_and_compare(&p, 0.5, expect_long, 1, "after engine swap");
    note(&p, 0, 60);

    /* G: a broken path keeps the loaded preset playing and says why */
    plugin_load(&p, "/nonexistent/x.dspreset", status, sizeof(status));
    printf("  status: %s\n", status);
    CHECK(!strncmp(status, "Error:", 6));

    plugin_close(&p);
    puts("render test passed");
    return 0;
}
