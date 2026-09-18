/* Round robin cycles through EVERY position, one step per note-on — checked by
 * which file each hit actually plays. Release triggers do not advance it. */
#include "test_support.h"

#include "../src/dsp/dspreset/native_engine.h"

static const char *played(ds_native_engine_t *e) {
    for (int i = 0; i < DS_MAX_VOICES; ++i)
        if (e->voices[i].active && e->voices[i].age == e->age_counter && !e->voices[i].one_shot)
            return strrchr(e->source_paths[e->voices[i].zone->source], '/') + 1;
    return "(none)";
}

int main(void) {
    const char *dir = getenv("TEST_TMP");
    char path[512], error[128];
    static ds_native_engine_t e;
    const char *want[] = {"rr1.wav", "rr2.wav", "rr3.wav", "rr4.wav", "rr1.wav", "rr2.wav"};
    float out[256];
    CHECK(dir);
    snprintf(path, sizeof(path), "mkdir -p '%s/rr'", dir); CHECK(system(path) == 0);
    for (int i = 1; i <= 4; ++i) {
        snprintf(path, sizeof(path), "%s/rr/rr%d.wav", dir, i); write_wav24(path, 44100, 1, 1000, -1, -1);
    }
    snprintf(path, sizeof(path), "%s/rr/rel.wav", dir); write_wav24(path, 44100, 1, 1000, -1, -1);
    snprintf(path, sizeof(path), "%s/rr/p.dspreset", dir);
    write_text(path, "<DecentSampler><groups seqMode=\"round_robin\" release=\"0.01\"><group>"
                     "<sample path=\"rr1.wav\" rootNote=\"60\" seqPosition=\"1\"/>"
                     "<sample path=\"rr2.wav\" rootNote=\"60\" seqPosition=\"2\"/>"
                     "<sample path=\"rr3.wav\" rootNote=\"60\" seqPosition=\"3\"/>"
                     "<sample path=\"rr4.wav\" rootNote=\"60\" seqPosition=\"4\"/>"
                     "</group><group trigger=\"release\" seqMode=\"always\"><sample path=\"rel.wav\" rootNote=\"60\"/></group>"
                     "</groups></DecentSampler>");
    CHECK(ds_native_engine_load(&e, path, 44100, NULL, NULL, error, sizeof(error)) == 0);
    for (int hit = 0; hit < 6; ++hit) {
        ds_native_engine_note_on(&e, 60, 100);
        if (strcmp(played(&e), want[hit])) {
            fprintf(stderr, "FAIL hit %d played %s, want %s\n", hit + 1, played(&e), want[hit]);
            return 1;
        }
        ds_native_engine_note_off(&e, 60);          /* fires the release sample */
        for (int b = 0; b < 100; ++b) ds_native_engine_render(&e, out, 128);
    }
    ds_native_engine_destroy(&e);
    puts("round robin test passed");
    return 0;
}
