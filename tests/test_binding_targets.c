/* Knobs on where and when a sample plays: SAMPLE_START / SAMPLE_END,
 * LOOP_START / LOOP_END (past the resident head, so the worker's stream
 * follows them), LO/HI_NOTE and LO/HI_VEL (a binding wins over the sample's
 * own range), ROOT_NOTE, AMP_ENV_ENABLED and GROUP_VOLUME — each sample-exact
 * through the engine. */
#include "test_support.h"

#include "../src/dsp/dspreset/native_engine.h"

#define SR 44100

static float sig(uint64_t f) { return test_signal24(f, 0) / 8388607.0f; }

static void load(ds_native_engine_t *e, const char *dir, const char *xml) {
    char path[512], error[128];
    snprintf(path, sizeof(path), "%s/bt/p.dspreset", dir);
    write_text(path, xml);
    CHECK(ds_native_engine_load(e, path, SR, NULL, NULL, error, sizeof(error)) == 0);
    for (int i = 0; i < 4; ++i) e->amp_override[i] = -1;
}

static void block(ds_native_engine_t *e, float *out) {
    while (ds_native_engine_service(e)) {}
    memset(out, 0, 256 * sizeof(float));
    ds_native_engine_render(e, out, 128);
}

#define KNOB(param, lo, hi, value) \
    "<labeled-knob minValue=\"" #lo "\" maxValue=\"" #hi "\" value=\"" #value "\">" \
    "<binding type=\"general\" level=\"group\" position=\"0\" parameter=\"" param "\"/></labeled-knob>"

int main(void) {
    const char *dir = getenv("TEST_TMP");
    char path[512];
    static ds_native_engine_t e;
    float out[256];
    CHECK(dir);
    snprintf(path, sizeof(path), "mkdir -p '%s/bt'", dir); CHECK(system(path) == 0);
    snprintf(path, sizeof(path), "%s/bt/long.wav", dir); write_wav24(path, SR, 1, 60000, -1, -1);

    /* start and end: the note plays frames 1000..1999 and stops */
    load(&e, dir, "<DecentSampler><ui><tab>" KNOB("SAMPLE_START", 0, 50000, 0) KNOB("SAMPLE_END", 0, 59999, 59999) "</tab></ui>"
                  "<groups><group><sample path=\"long.wav\" rootNote=\"60\" ampEnvEnabled=\"false\"/></group></groups></DecentSampler>");
    ds_native_engine_set_control(&e, 0, 1000);
    ds_native_engine_set_control(&e, 1, 1999);
    ds_native_engine_note_on(&e, 60, 127);
    for (int at = 0; at < 1024; at += 128) {
        block(&e, out);
        for (int i = 0; i < 128; ++i)
            CHECK(fabsf(out[2 * i] - (at + i < 1000 ? sig(1000 + at + i) : 0.0f)) < 1e-5f);
    }
    CHECK(ds_native_engine_active_voices(&e) == 0);
    ds_native_engine_destroy(&e);

    /* loop points moved by knobs, past the head: the stream loops 40000..44999 */
    load(&e, dir, "<DecentSampler><ui><tab>" KNOB("LOOP_START", 0, 59999, 0) KNOB("LOOP_END", 0, 59999, 59999) "</tab></ui>"
                  "<groups><group><sample path=\"long.wav\" rootNote=\"60\" ampEnvEnabled=\"false\" loopEnabled=\"true\""
                  " loopStart=\"100\" loopEnd=\"59999\"/></group></groups></DecentSampler>");
    ds_native_engine_set_control(&e, 0, 40000);
    ds_native_engine_set_control(&e, 1, 44999);
    ds_native_engine_note_on(&e, 60, 127);
    {
        double worst = 0;
        for (unsigned at = 0; at < 60000; at += 128) {
            block(&e, out);
            for (int i = 0; i < 128; ++i) {
                uint64_t vf = at + i, f = vf >= 45000 ? 40000 + (vf - 40000) % 5000 : vf;
                double d = fabs(out[2 * i] - sig(f));
                if (d > worst) worst = d;
            }
        }
        printf("  loop 40000..44999 by knob, streamed: worst %.2e\n", worst);
        CHECK(worst < 1e-5);
    }
    ds_native_engine_destroy(&e);

    /* key and velocity ranges: a button narrows them over the sample's own */
    load(&e, dir, "<DecentSampler><ui><tab><button value=\"0\">"
                  "<state name=\"All\"/>"
                  "<state name=\"Narrow\">"
                  "<binding type=\"general\" level=\"group\" position=\"0\" parameter=\"LO_NOTE\" translation=\"fixed_value\" translationValue=\"60\"/>"
                  "<binding type=\"general\" level=\"group\" position=\"0\" parameter=\"HI_NOTE\" translation=\"fixed_value\" translationValue=\"64\"/>"
                  "<binding type=\"general\" level=\"group\" position=\"0\" parameter=\"LO_VEL\" translation=\"fixed_value\" translationValue=\"50\"/>"
                  "<binding type=\"general\" level=\"group\" position=\"0\" parameter=\"HI_VEL\" translation=\"fixed_value\" translationValue=\"100\"/>"
                  "</state></button></tab></ui>"
                  "<groups><group><sample path=\"long.wav\" rootNote=\"60\" loNote=\"0\" hiNote=\"127\"/></group></groups></DecentSampler>");
    ds_native_engine_note_on(&e, 70, 127); CHECK(ds_native_engine_active_voices(&e) == 1);
    ds_native_engine_cc(&e, 120, 0);
    ds_native_engine_set_control(&e, 0, 1);
    ds_native_engine_note_on(&e, 70, 80); CHECK(ds_native_engine_active_voices(&e) == 0);   /* above HI_NOTE */
    ds_native_engine_note_on(&e, 59, 80); CHECK(ds_native_engine_active_voices(&e) == 0);   /* below LO_NOTE */
    ds_native_engine_note_on(&e, 62, 49); CHECK(ds_native_engine_active_voices(&e) == 0);   /* below LO_VEL */
    ds_native_engine_note_on(&e, 62, 101); CHECK(ds_native_engine_active_voices(&e) == 0);  /* above HI_VEL */
    ds_native_engine_note_on(&e, 60, 50); CHECK(ds_native_engine_active_voices(&e) == 1);   /* edges are inclusive */
    ds_native_engine_note_on(&e, 64, 100); CHECK(ds_native_engine_active_voices(&e) == 2);
    ds_native_engine_destroy(&e);

    /* ROOT_NOTE 48: note 60 plays an octave up; AMP_ENV_ENABLED false skips a
     * 1 s attack; GROUP_VOLUME halves it */
    load(&e, dir, "<DecentSampler><ui><tab>" KNOB("ROOT_NOTE", 0, 127, 60) KNOB("GROUP_VOLUME", 0, 1, 1)
                  "<button value=\"0\"><state name=\"On\"/><state name=\"Off\"><binding type=\"amp\" level=\"group\" position=\"0\""
                  " parameter=\"AMP_ENV_ENABLED\" translation=\"fixed_value\" translationValue=\"false\"/></state></button></tab></ui>"
                  "<groups attack=\"1\"><group><sample path=\"long.wav\" rootNote=\"60\"/></group></groups></DecentSampler>");
    ds_native_engine_set_control(&e, 0, 48);
    ds_native_engine_note_on(&e, 60, 127);
    block(&e, out);
    for (int i = 0; i < DS_MAX_VOICES; ++i) if (e.voices[i].active) CHECK(fabs(e.voices[i].inc - 2.0) < 1e-12);
    CHECK(fabsf(out[2 * 100]) < fabsf(sig(200)) * 0.01f + 1e-6f);      /* the 1 s attack: nearly silent */
    ds_native_engine_cc(&e, 120, 0);
    ds_native_engine_set_control(&e, 0, 60);
    ds_native_engine_set_control(&e, 1, 0.5f);
    ds_native_engine_set_control(&e, 2, 1);                          /* envelope off */
    ds_native_engine_note_on(&e, 60, 127);
    block(&e, out);
    for (int i = 0; i < 128; ++i) CHECK(fabsf(out[2 * i] - 0.5f * sig(i)) < 1e-5f);
    ds_native_engine_destroy(&e);

    puts("binding targets test passed");
    return 0;
}
