/* pitchKeyTrack: 0 plays the root pitch on every key (DecenTron's tape noise),
 * 0.5 half the interval, at sample, group and instrument level; the output
 * sample-exact at 0; and PITCH_KEY_TRACK moving a sounding note. */
#include "test_support.h"

#include "../src/dsp/dspreset/native_engine.h"

#define SR 44100

static ds_voice_t *only_voice(ds_native_engine_t *e) {
    ds_voice_t *found = NULL;
    for (int i = 0; i < DS_MAX_VOICES; ++i) if (e->voices[i].active) { CHECK(!found); found = &e->voices[i]; }
    CHECK(found);
    return found;
}

static void load(ds_native_engine_t *e, const char *dir, const char *xml) {
    char path[512], error[128];
    snprintf(path, sizeof(path), "%s/pkt/p.dspreset", dir);
    write_text(path, xml);
    CHECK(ds_native_engine_load(e, path, SR, NULL, NULL, error, sizeof(error)) == 0);
    for (int i = 0; i < 4; ++i) e->amp_override[i] = -1;
}

int main(void) {
    const char *dir = getenv("TEST_TMP");
    char path[512];
    static ds_native_engine_t e;
    float out[256];
    CHECK(dir);
    snprintf(path, sizeof(path), "mkdir -p '%s/pkt'", dir); CHECK(system(path) == 0);
    snprintf(path, sizeof(path), "%s/pkt/tape.wav", dir); write_wav24(path, SR, 1, 20000, -1, -1);

    /* 0 on the sample: note 89 plays the file as it is, sample for sample */
    load(&e, dir, "<DecentSampler><groups><group tags=\"Tape\"><sample path=\"tape.wav\" rootNote=\"55\" loNote=\"55\" hiNote=\"89\""
                  " pitchKeyTrack=\"0\" ampEnvEnabled=\"false\"/></group></groups></DecentSampler>");
    ds_native_engine_note_on(&e, 89, 127);
    memset(out, 0, sizeof(out));
    ds_native_engine_render(&e, out, 128);
    CHECK(only_voice(&e)->inc == 1.0);
    for (int i = 0; i < 128; ++i) CHECK(fabsf(out[2 * i] - test_signal24(i, 0) / 8388607.0f) < 1e-5f);
    ds_native_engine_destroy(&e);

    /* 0.5 on the group, and on <groups>: half the interval */
    for (int level = 0; level < 2; ++level) {
        load(&e, dir, level ? "<DecentSampler><groups pitchKeyTrack=\"0.5\"><group><sample path=\"tape.wav\" rootNote=\"55\"/></group></groups></DecentSampler>"
                            : "<DecentSampler><groups><group pitchKeyTrack=\"0.5\"><sample path=\"tape.wav\" rootNote=\"55\"/></group></groups></DecentSampler>");
        ds_native_engine_note_on(&e, 67, 127);
        ds_native_engine_render(&e, out, 128);
        CHECK(fabs(only_voice(&e)->inc - pow(2.0, 0.5)) < 1e-9);
        ds_native_engine_destroy(&e);
    }

    /* a knob on PITCH_KEY_TRACK moves a sounding note; the default is 1 */
    load(&e, dir, "<DecentSampler><ui><tab><labeled-knob minValue=\"0\" maxValue=\"1\" value=\"1\">"
                  "<binding type=\"general\" level=\"group\" position=\"0\" parameter=\"PITCH_KEY_TRACK\"/></labeled-knob></tab></ui>"
                  "<groups><group><sample path=\"tape.wav\" rootNote=\"55\"/></group></groups></DecentSampler>");
    ds_native_engine_note_on(&e, 67, 127);
    ds_native_engine_render(&e, out, 128);
    CHECK(fabs(only_voice(&e)->inc - 2.0) < 1e-9);
    ds_native_engine_set_control(&e, 0, 0);
    ds_native_engine_render(&e, out, 128);
    CHECK(only_voice(&e)->inc == 1.0);
    ds_native_engine_destroy(&e);

    puts("pitch key track test passed");
    return 0;
}
