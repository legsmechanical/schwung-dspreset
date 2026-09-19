/* With the worker held back, a streamed voice must WAIT, not skip: the first
 * sound it makes once data arrives is its first frame. Drives the engine
 * directly so the worker's timing is under the test's control. */
#include "test_support.h"

#include "../src/dsp/dspreset/native_engine.h"

int main(void) {
    const char *dir = getenv("TEST_TMP");
    char path[512], error[128] = {0};
    static ds_native_engine_t engine;
    float out[256];
    unsigned silent_blocks = 0;
    CHECK(dir);
    snprintf(path, sizeof(path), "mkdir -p '%s/stall'", dir); CHECK(system(path) == 0);
    snprintf(path, sizeof(path), "%s/stall/s.wav", dir); write_wav24(path, 44100, 1, 60000, -1, -1);
    snprintf(path, sizeof(path), "%s/stall/p.dspreset", dir);
    /* start beyond the resident head: nothing to play until the worker reads */
    write_text(path, "<DecentSampler><groups><group>"
                     "<sample path=\"s.wav\" rootNote=\"60\" start=\"20000\" release=\"0.1\"/>"
                     "</group></groups></DecentSampler>");
    CHECK(ds_native_engine_load(&engine, path, 44100, NULL, NULL, error, sizeof(error)) == 0);
    CHECK(engine.zones[0].b.streams && engine.zones[0].b.start == 20000);

    ds_native_engine_note_on(&engine, 60, 127);
    for (int b = 0; b < 10; ++b) {                   /* worker starved for 10 blocks */
        memset(out, 0, sizeof(out));
        ds_native_engine_render(&engine, out, 128);
        for (int i = 0; i < 256; ++i) CHECK(out[i] == 0);
        silent_blocks++;
    }
    CHECK(atomic_load(&engine.underruns) > 0);
    CHECK(ds_native_engine_service(&engine) > 0);
    memset(out, 0, sizeof(out));
    ds_native_engine_render(&engine, out, 128);
    for (int i = 0; i < 128; ++i) {
        float want = test_signal24(20000 + (uint64_t)i, 0) / 8388608.0f;
        if (out[2 * i] != want) {
            fprintf(stderr, "FAIL frame %d after stall: got %f want %f\n", i, out[2 * i], want);
            return 1;
        }
    }
    printf("  held %u blocks, then resumed at frame 20000 exactly\n", silent_blocks);
    ds_native_engine_destroy(&engine);
    puts("engine stall test passed");
    return 0;
}
