/* <modulators>: envelopes, LFOs, CC and velocity sources, how each lands on its
 * target (add / multiply / set / modulate), per note or shared, and knobs that
 * move a modulator's own settings. Driven through the engine at block rate. */
#include "test_support.h"

#include "../src/dsp/dspreset/native_engine.h"

static double block_rms(ds_native_engine_t *e) {
    float out[256];
    double s = 0;
    memset(out, 0, sizeof(out));
    ds_native_engine_render(e, out, 128);
    for (int i = 0; i < 128; ++i) s += out[2 * i] * out[2 * i];
    return sqrt(s / 128);
}

static void blocks(ds_native_engine_t *e, int n) { while (n--) block_rms(e); }

static double voice_inc(ds_native_engine_t *e) {
    for (int i = 0; i < DS_MAX_VOICES; ++i) if (e->voices[i].active) return e->voices[i].inc;
    return -1;
}

static void load(ds_native_engine_t *e, const char *dir, const char *name, const char *xml) {
    char path[512], error[128];
    snprintf(path, sizeof(path), "%s/mod/%s.dspreset", dir, name);
    write_text(path, xml);
    CHECK(ds_native_engine_load(e, path, 44100, NULL, NULL, error, sizeof(error)) == 0);
}

int main(void) {
    const char *dir = getenv("TEST_TMP");
    char path[512];
    static ds_native_engine_t e;
    double open, closed, first;
    CHECK(dir);
    snprintf(path, sizeof(path), "mkdir -p '%s/mod'", dir); CHECK(system(path) == 0);
    snprintf(path, sizeof(path), "%s/mod/tone.wav", dir); write_sine24(path, 30000, 5000, 0.5);

    /* 1. A per-note envelope ADDS to a group filter resting at 33 Hz (CS-20M's
     * shape): open at the attack, shut once it decays to sustain 0. */
    load(&e, dir, "env_filter",
        "<DecentSampler><groups attack=\"0\" release=\"0.05\"><group>"
        "<sample path=\"tone.wav\" rootNote=\"60\"/>"
        "<effects><effect type=\"lowpass\" frequency=\"33\"/></effects></group></groups>"
        "<modulators><envelope attack=\"0\" decay=\"0.3\" sustain=\"0\" release=\"0.1\" modAmount=\"1\">"
        "<binding type=\"effect\" level=\"group\" groupIndex=\"0\" effectIndex=\"0\" parameter=\"FX_FILTER_FREQUENCY\" modBehavior=\"add\""
        " translation=\"table\" translationTable=\"0,33;0.5,1100;1.0001,22000\"/></envelope></modulators></DecentSampler>");
    CHECK(e.model.modulator_count == 1 && e.model.modulators[0].voice_scope && e.fx_modulated[0]);
    ds_native_engine_note_on(&e, 60, 127);
    open = block_rms(&e);
    blocks(&e, 400);                                   /* ~1.2 s: long past the decay */
    closed = block_rms(&e);
    printf("  envelope on the filter: %.4f at the attack, %.4f once decayed\n", open, closed);
    CHECK(open > 0.3 && closed < 0.01);
    ds_native_engine_destroy(&e);

    /* 2. A shared LFO adds vibrato through -12..12 semitones ON TOP of the
     * group's own +12 tuning: at 1 Hz and depth 0.5 it starts at neutral (an
     * LFO is two-sided), and a quarter second in is its peak, +6. A knob bound
     * to its MOD_AMOUNT takes the vibrato away entirely. */
    load(&e, dir, "vibrato",
        "<DecentSampler><ui><tab><control parameterName=\"Depth\" minValue=\"0\" maxValue=\"1\" value=\"0.5\">"
        "<binding type=\"modulator\" level=\"instrument\" position=\"0\" parameter=\"MOD_AMOUNT\"/></control></tab></ui>"
        "<groups attack=\"0\"><group groupTuning=\"12\"><sample path=\"tone.wav\" rootNote=\"60\"/></group></groups>"
        "<modulators><lfo shape=\"sine\" frequency=\"1\" modAmount=\"0.5\">"
        "<binding type=\"amp\" level=\"group\" groupIndex=\"0\" parameter=\"GROUP_TUNING\" modBehavior=\"add\""
        " translation=\"linear\" translationOutputMin=\"-12\" translationOutputMax=\"12\"/></lfo></modulators></DecentSampler>");
    CHECK(!e.model.modulators[0].voice_scope);          /* LFOs are shared by default */
    CHECK(!strcmp(e.model.controls[0].name, "Depth"));
    ds_native_engine_note_on(&e, 60, 127);
    block_rms(&e);
    CHECK(fabs(voice_inc(&e) - 2.0) < 1e-9);           /* phase 0 is neutral: just the +12 */
    blocks(&e, 85);                                    /* 86 x 128 = 0.2496 s */
    block_rms(&e);
    printf("  vibrato at its peak: rate %.5f (want %.5f)\n", voice_inc(&e), pow(2.0, 18.0 / 12.0));
    CHECK(fabs(voice_inc(&e) - pow(2.0, 18.0 / 12.0)) < 2e-3);   /* +12 +6: it ADDS */
    ds_native_engine_set_control(&e, 0, 0);
    block_rms(&e);
    CHECK(fabs(voice_inc(&e) - 2.0) < 1e-9);           /* no depth, no vibrato: exact */
    ds_native_engine_destroy(&e);

    /* 3. set (the default) and multiply, on volume: the envelope scales a note's
     * level straight to its sustain; a CC source sets the instrument volume. */
    load(&e, dir, "env_volume",
        "<DecentSampler><groups attack=\"0\"><group><sample path=\"tone.wav\" rootNote=\"60\"/></group></groups>"
        "<modulators><envelope attack=\"0\" decay=\"0.01\" sustain=\"0.5\">"
        "<binding type=\"amp\" level=\"group\" groupIndex=\"0\" parameter=\"AMP_VOLUME\" modBehavior=\"multiply\""
        " translation=\"linear\" translationOutputMin=\"0\" translationOutputMax=\"1\"/></envelope>"
        "<midiCC number=\"11\" scope=\"global\"><binding type=\"amp\" level=\"instrument\" parameter=\"AMP_VOLUME\""
        " translation=\"linear\" translationOutputMin=\"0\" translationOutputMax=\"1\"/></midiCC></modulators></DecentSampler>");
    ds_native_engine_cc(&e, 11, 127);
    ds_native_engine_note_on(&e, 60, 127);
    first = block_rms(&e);                             /* the envelope starts at the top */
    blocks(&e, 40);
    printf("  envelope x volume: %.4f then %.4f (sustain 0.5)\n", first, block_rms(&e));
    CHECK(fabs(block_rms(&e) - 0.5 * 0.5 * 0.7071) < 0.01);
    ds_native_engine_cc(&e, 11, 0);                    /* CC 11 at zero sets the instrument volume to 0 */
    CHECK(block_rms(&e) < 1e-6);
    ds_native_engine_destroy(&e);

    /* 4. An LFO's delayTime holds it at neutral; a per-note one starts at phase 0. */
    load(&e, dir, "delayed",
        "<DecentSampler><groups attack=\"0\"><group><sample path=\"tone.wav\" rootNote=\"60\"/></group></groups>"
        "<modulators><lfo shape=\"square\" frequency=\"2\" modAmount=\"1\" delayTime=\"0.2\" scope=\"voice\">"
        "<binding type=\"amp\" level=\"instrument\" parameter=\"GLOBAL_TUNING\" modBehavior=\"add\""
        " translation=\"linear\" translationOutputMin=\"-12\" translationOutputMax=\"12\"/></lfo></modulators></DecentSampler>");
    ds_native_engine_note_on(&e, 60, 127);
    blocks(&e, 30);                                    /* 0.087 s: still inside the delay */
    block_rms(&e);
    CHECK(voice_inc(&e) == 1.0);
    blocks(&e, 45);                                    /* past 0.2 s: the square wave's top half */
    block_rms(&e);
    CHECK(fabs(voice_inc(&e) - 2.0) < 1e-9);           /* +12 semitones */
    ds_native_engine_destroy(&e);

    /* 5. A SHARED envelope keys on the first note down and releases on the last up. */
    load(&e, dir, "global_env",
        "<DecentSampler><groups attack=\"0\"><group><sample path=\"tone.wav\" rootNote=\"60\" loNote=\"0\" hiNote=\"127\"/></group></groups>"
        "<modulators><envelope scope=\"global\" attack=\"0\" decay=\"0\" sustain=\"1\" release=\"0.05\">"
        "<binding type=\"amp\" level=\"instrument\" parameter=\"AMP_VOLUME\"/></envelope></modulators></DecentSampler>");
    CHECK(e.mod_global[0].stage == DS_ENV_DONE);
    ds_native_engine_note_on(&e, 60, 100);
    ds_native_engine_note_on(&e, 64, 100);
    CHECK(e.mod_global[0].stage != DS_ENV_DONE && e.mod_global[0].stage != DS_ENV_RELEASE);
    ds_native_engine_note_off(&e, 60);
    CHECK(e.mod_global[0].stage != DS_ENV_RELEASE);    /* one key still down */
    ds_native_engine_note_off(&e, 64);
    CHECK(e.mod_global[0].stage == DS_ENV_RELEASE);
    ds_native_engine_destroy(&e);

    puts("modulators test passed");
    return 0;
}
