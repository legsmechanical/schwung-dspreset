/* Modulators, the rest of the guide: an LFO's SHAPE and MOD_DELAY_TIME moved
 * by controls (shape words as fixed values); TRIGGER / trigger="attack"
 * restarting a global LFO at every note; an envelope's MOD_DELAY_TIME; the
 * <random> modulator (a value per note, or `frequency` times a second;
 * reproducible from its seed); and <midi><velocity>, whose bindings follow
 * each note's velocity scaled by the binding's own modAmount. */
#include "test_support.h"

#include "../src/dsp/dspreset/native_engine.h"

#define SR 44100

static float sig(uint64_t f) { return test_signal24(f, 0) / 8388607.0f; }

static void load(ds_native_engine_t *e, const char *dir, const char *xml) {
    char path[512], error[128];
    snprintf(path, sizeof(path), "%s/mt/p.dspreset", dir);
    write_text(path, xml);
    CHECK(ds_native_engine_load(e, path, SR, NULL, NULL, error, sizeof(error)) == 0);
    for (int i = 0; i < 4; ++i) e->amp_override[i] = -1;
}

static void block(ds_native_engine_t *e, float *out) {
    memset(out, 0, 256 * sizeof(float));
    ds_native_engine_render(e, out, 128);
}

/* One block's level against the file, for a note that started `blocks_in` blocks ago. */
static float level_at(ds_native_engine_t *e, int blocks_in) {
    float out[256];
    block(e, out);
    return out[2 * 50] / sig((uint64_t)blocks_in * 128 + 50);
}
static float level(ds_native_engine_t *e) { return level_at(e, 0); }

/* The per-note lowpass frequency note `note` carries. */
static float note_cutoff(ds_native_engine_t *e, int note) {
    for (int i = 0; i < DS_MAX_VOICES; ++i) {
        ds_voice_t *v = &e->voices[i];
        if (!v->active || v->note != note) continue;
        for (unsigned p = 0; p < e->model.effects[v->fx_index[0]].param_count; ++p)
            if (!strcmp(e->model.effects[v->fx_index[0]].param_names[p], "frequency")) return v->fx_built[0].values[p];
    }
    CHECK(0);
    return 0;
}

int main(void) {
    const char *dir = getenv("TEST_TMP");
    char path[512];
    static ds_native_engine_t e;
    CHECK(dir);
    snprintf(path, sizeof(path), "mkdir -p '%s/mt'", dir); CHECK(system(path) == 0);
    snprintf(path, sizeof(path), "%s/mt/a.wav", dir); write_wav24(path, SR, 1, 60000, -1, -1);

    /* SHAPE: an LFO at phase 0 sets the volume — sine 0 -> 0.5, square +1 -> 1 */
    load(&e, dir,
         "<DecentSampler><ui><tab><button value=\"0\">"
         "<state name=\"Sine\"><binding type=\"modulator\" level=\"instrument\" modulatorIndex=\"0\" parameter=\"SHAPE\" translation=\"fixed_value\" translationValue=\"sine\"/></state>"
         "<state name=\"Square\"><binding type=\"modulator\" level=\"instrument\" modulatorIndex=\"0\" parameter=\"SHAPE\" translation=\"fixed_value\" translationValue=\"square\"/></state>"
         "</button>"
         "<labeled-knob minValue=\"0\" maxValue=\"5\" value=\"0\"><binding type=\"modulator\" level=\"instrument\" modulatorIndex=\"0\" parameter=\"MOD_DELAY_TIME\"/></labeled-knob>"
         "<button value=\"0\"><state name=\"Free\"><binding type=\"modulator\" level=\"instrument\" modulatorIndex=\"0\" parameter=\"TRIGGER\" translation=\"fixed_value\" translationValue=\"none\"/></state>"
         "<state name=\"Retrigger\"><binding type=\"modulator\" level=\"instrument\" modulatorIndex=\"0\" parameter=\"TRIGGER\" translation=\"fixed_value\" translationValue=\"attack\"/></state></button>"
         "</tab></ui><groups><group><sample path=\"a.wav\" rootNote=\"60\" ampEnvEnabled=\"false\"/></group></groups>"
         "<modulators><lfo frequency=\"0.5\" modAmount=\"1\"><binding type=\"amp\" level=\"group\" position=\"0\" parameter=\"AMP_VOLUME\""
         " translation=\"linear\" translationOutputMin=\"0\" translationOutputMax=\"1\"/></lfo></modulators></DecentSampler>");
    ds_native_engine_note_on(&e, 60, 127);
    CHECK(fabsf(level(&e) - 0.5f) < 1e-4f);
    ds_native_engine_set_control(&e, 0, 1);
    CHECK(e.model.modulators[0].shape == DS_LFO_SQUARE);
    ds_native_engine_set_control(&e, 1, 2.5f);
    CHECK(e.model.modulators[0].delay == 2.5f);
    /* TRIGGER: free-running, a note does not restart the LFO; retriggered, it does */
    for (int i = 0; i < 20; ++i) level(&e);
    ds_native_engine_note_on(&e, 62, 127);
    CHECK(e.mod_global[0].phase > 0.01f);
    ds_native_engine_set_control(&e, 2, 1);
    ds_native_engine_note_on(&e, 64, 127);
    CHECK(e.mod_global[0].phase == 0.0f && e.mod_global[0].delay_left == 2.5f);
    ds_native_engine_destroy(&e);

    /* trigger="attack" written on the <lfo> itself */
    load(&e, dir, "<DecentSampler><groups><group><sample path=\"a.wav\" rootNote=\"60\"/></group></groups>"
                  "<modulators><lfo frequency=\"3\" trigger=\"attack\"/></modulators></DecentSampler>");
    CHECK(e.model.modulators[0].trigger);
    ds_native_engine_destroy(&e);

    /* an envelope's delay: nothing for 0.1 s, then its attack */
    load(&e, dir,
         "<DecentSampler><groups><group><sample path=\"a.wav\" rootNote=\"60\" ampEnvEnabled=\"false\"/></group></groups>"
         "<modulators><envelope attack=\"0\" sustain=\"1\" delayTime=\"0.1\">"
         "<binding type=\"amp\" level=\"group\" position=\"0\" parameter=\"AMP_VOLUME\" translation=\"linear\" translationOutputMin=\"0\" translationOutputMax=\"1\"/>"
         "</envelope></modulators></DecentSampler>");
    ds_native_engine_note_on(&e, 60, 127);
    for (int b = 0; b < 35; ++b) CHECK(fabsf(level_at(&e, b)) < 1e-6f);   /* 35 x 128 = 4480: the 0.1 s (4410) runs out in the 35th */
    CHECK(fabsf(level_at(&e, 35) - 1.0f) < 1e-4f);
    ds_native_engine_destroy(&e);

    /* <random>: one value per note, -1..1, reproducible from the seed */
    {
        float first[3][6];
        for (int run = 0; run < 3; ++run) {
            char xml[1024];
            snprintf(xml, sizeof(xml),
                 "<DecentSampler><groups><group><sample path=\"a.wav\" rootNote=\"60\" ampEnvEnabled=\"false\"/></group></groups>"
                 "<modulators><random mode=\"note_on\" seed=\"%s\" scope=\"voice\">"
                 "<binding type=\"amp\" level=\"group\" position=\"0\" parameter=\"AMP_VOLUME\" translation=\"linear\" translationOutputMin=\"0\" translationOutputMax=\"1\"/>"
                 "</random></modulators></DecentSampler>", run < 2 ? "12345" : "999");
            load(&e, dir, xml);
            for (int n = 0; n < 6; ++n) {
                ds_native_engine_note_on(&e, 60, 127);
                first[run][n] = level(&e);
                CHECK(first[run][n] >= -1e-6f && first[run][n] <= 1.0f + 1e-6f);
                ds_native_engine_cc(&e, 120, 0);
            }
            ds_native_engine_destroy(&e);
        }
        CHECK(!memcmp(first[0], first[1], sizeof(first[0])));   /* the same seed, the same values */
        CHECK(memcmp(first[0], first[2], sizeof(first[0])));    /* another seed, others */
        CHECK(first[0][0] != first[0][1] && first[0][1] != first[0][2]);
    }
    /* periodic: a new value 10 times a second, held in between */
    load(&e, dir,
         "<DecentSampler><groups><group><sample path=\"a.wav\" rootNote=\"60\"/></group></groups>"
         "<modulators><random mode=\"periodic\" frequency=\"10\" seed=\"7\"/></modulators></DecentSampler>");
    {
        float out[256], was = e.mod_global[0].level;
        int changes = 0;
        for (int b = 0; b < 344; ++b) {                        /* one second */
            block(&e, out);
            if (e.mod_global[0].level != was) { changes++; was = e.mod_global[0].level; }
        }
        printf("  periodic random at 10 Hz: %d new values in 1 s\n", changes);
        CHECK(changes == 9 || changes == 10);
    }
    ds_native_engine_destroy(&e);

    /* <midi><velocity>: a per-note cutoff of 500 + 2000 x velocity, halved by modAmount */
    for (int half = 0; half < 2; ++half) {
        char xml[1024];
        snprintf(xml, sizeof(xml),
                 "<DecentSampler><groups><group><sample path=\"a.wav\" rootNote=\"60\"/>"
                 "<effects><effect type=\"lowpass\" frequency=\"500\"/></effects></group></groups>"
                 "<midi><velocity><binding type=\"effect\" level=\"group\" groupIndex=\"0\" effectIndex=\"0\" parameter=\"FX_FILTER_FREQUENCY\""
                 " modBehavior=\"add\" translation=\"linear\" translationOutputMin=\"0\" translationOutputMax=\"2000\"%s/></velocity></midi></DecentSampler>",
                 half ? " modAmount=\"0.5\"" : "");
        load(&e, dir, xml);
        ds_native_engine_note_on(&e, 60, 127);
        ds_native_engine_note_on(&e, 62, 64);
        level(&e);
        CHECK(fabsf(note_cutoff(&e, 60) - (500 + 2000 * (half ? 0.5f : 1.0f))) < 0.01f);
        CHECK(fabsf(note_cutoff(&e, 62) - (500 + 2000 * (half ? 0.5f : 1.0f) * 64 / 127)) < 0.01f);
        ds_native_engine_destroy(&e);
    }

    puts("modulator targets test passed");
    return 0;
}
