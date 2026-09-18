/* A preset's own controls: what each one is called, and that moving it moves
 * the sound it is bound to — by exact output level, by which file a note
 * plays, or by the value that lands in the target. */
#include "test_support.h"

#include "../src/dsp/dspreset/native_engine.h"

static const char *PRESET =
    "<DecentSampler>\n"
    "<ui><tab>\n"
    "  <labeled-knob label=\"Close\" minValue=\"0\" maxValue=\"1\" value=\"1\">\n"
    "    <binding type=\"amp\" level=\"group\" position=\"0\" parameter=\"AMP_VOLUME\" translation=\"linear\" translationOutputMin=\"0\" translationOutputMax=\"1\"/>\n"
    "  </labeled-knob>\n"
    "  <control minValue=\"0\" maxValue=\"100\" value=\"100\"><binding type=\"amp\" level=\"tag\" identifier=\"mic2\" parameter=\"AMP_VOLUME\" factor=\"0.01\"/></control>\n"
    "  <labeled-knob label=\"\" minValue=\"0\" maxValue=\"10\" value=\"3.5\" type=\"percent\">\n"
    "    <binding type=\"amp\" level=\"instrument\" position=\"0\" parameter=\"ENV_RELEASE\" translation=\"linear\" translationOutputMin=\"0\" translationOutputMax=\"10\"/>\n"
    "  </labeled-knob>\n"
    "  <button value=\"0\" parameterName=\"Layer\">\n"
    "    <state name=\"A\"><binding type=\"general\" level=\"group\" position=\"2\" parameter=\"ENABLED\" translation=\"fixed_value\" translationValue=\"false\"/>\n"
    "                    <binding type=\"general\" level=\"group\" position=\"3\" parameter=\"ENABLED\" translation=\"fixed_value\" translationValue=\"true\"/></state>\n"
    "    <state name=\"B\"><binding type=\"general\" level=\"group\" position=\"2\" parameter=\"ENABLED\" translation=\"fixed_value\" translationValue=\"true\"/>\n"
    "                    <binding type=\"general\" level=\"group\" position=\"3\" parameter=\"ENABLED\" translation=\"fixed_value\" translationValue=\"false\"/></state>\n"
    "  </button>\n"
    "  <menu value=\"1\">\n"
    "    <option name=\"Up\"><binding type=\"amp\" level=\"instrument\" parameter=\"GLOBAL_TUNING\" translation=\"fixed_value\" translationValue=\"0\"/></option>\n"
    "    <option name=\"Down\"><binding type=\"amp\" level=\"instrument\" parameter=\"GLOBAL_TUNING\" translation=\"fixed_value\" translationValue=\"-12\"/></option>\n"
    "  </menu>\n"
    "  <labeled-knob minValue=\"1\" maxValue=\"10\" value=\"10\">\n"
    "    <binding type=\"effect\" level=\"instrument\" position=\"0\" parameter=\"FX_FILTER_FREQUENCY\" translation=\"table\" translationTable=\"0,33;5,1000;10,22000\"/>\n"
    "  </labeled-knob>\n"
    "</tab></ui>\n"
    "<groups attack=\"0\" release=\"0.2\">\n"
    "  <group name=\"Close\" tags=\"mic1\"><sample path=\"a.wav\" rootNote=\"60\" loNote=\"60\" hiNote=\"60\"/></group>\n"
    "  <group name=\"Room\" tags=\"mic2\"><sample path=\"b.wav\" rootNote=\"60\" loNote=\"60\" hiNote=\"60\"/></group>\n"
    "  <group name=\"Layer B\" enabled=\"false\"><sample path=\"c.wav\" rootNote=\"62\" loNote=\"62\" hiNote=\"62\"/></group>\n"
    "  <group name=\"Layer A\"><sample path=\"d.wav\" rootNote=\"62\" loNote=\"62\" hiNote=\"62\"/></group>\n"
    "</groups>\n"
    "<effects><effect type=\"lowpass\" frequency=\"22000\"/></effects>\n"
    "<midi><cc number=\"1\"><binding level=\"ui\" type=\"control\" parameter=\"VALUE\" position=\"0\" translation=\"linear\" translationOutputMin=\"0\" translationOutputMax=\"1\"/></cc></midi>\n"
    "</DecentSampler>\n";

static float sig(uint64_t f) { return test_signal24(f, 0) / 8388608.0f; }

static const char *playing_file(ds_native_engine_t *e) {
    for (int i = 0; i < DS_MAX_VOICES; ++i)
        if (e->voices[i].active && e->voices[i].age == e->age_counter)
            return strrchr(e->source_paths[e->voices[i].zone->source], '/') + 1;
    return "(none)";
}

/* Renders one block and checks out = level * sig(frame) exactly. */
static void expect_level(ds_native_engine_t *e, uint64_t *frame, float level, const char *what) {
    float out[256] = {0};
    ds_native_engine_render(e, out, 128);
    for (int i = 0; i < 128; ++i, ++*frame) {
        float want = level * sig(*frame);
        if (fabsf(out[2 * i] - want) > 1e-5f) {
            fprintf(stderr, "FAIL %s: frame %llu got %f want %f\n", what, (unsigned long long)*frame, out[2 * i], want);
            exit(1);
        }
    }
}

static void silence(ds_native_engine_t *e) {
    float out[256];
    ds_native_engine_cc(e, 120, 0);
    ds_native_engine_render(e, out, 128);
}

int main(void) {
    const char *dir = getenv("TEST_TMP");
    char path[512], error[128] = {0};
    static ds_native_engine_t e;
    const char *names[] = {"Close", "mic2", "Release", "Layer", "Tune", "Cutoff"};
    uint64_t frame = 0;
    CHECK(dir);
    snprintf(path, sizeof(path), "mkdir -p '%s/ctl'", dir); CHECK(system(path) == 0);
    for (const char *f = "abcd"; *f; ++f) {
        snprintf(path, sizeof(path), "%s/ctl/%c.wav", dir, *f); write_wav24(path, 44100, 1, 30000, -1, -1);
    }
    snprintf(path, sizeof(path), "%s/ctl/p.dspreset", dir); write_text(path, PRESET);
    CHECK(ds_native_engine_load(&e, path, 44100, NULL, NULL, error, sizeof(error)) == 0);

    /* names: label, parameterName, a tag, a binding target, an effect target */
    CHECK(e.model.control_count == 6);
    for (unsigned i = 0; i < 6; ++i)
        if (strcmp(e.model.controls[i].name, names[i])) { fprintf(stderr, "FAIL control %u named '%s', want '%s'\n", i, e.model.controls[i].name, names[i]); return 1; }
    CHECK(e.model.controls[3].kind == DS_CONTROL_BUTTON && e.model.controls[3].choice_count == 2);
    CHECK(e.model.controls[4].kind == DS_CONTROL_MENU && e.control_value[4] == 0);   /* menu value 1 = first option */

    /* group volume and tag volume, live on a sounding note: both groups play
     * the same signal, so the output is (close + room) x signal */
    ds_native_engine_note_on(&e, 60, 127);
    expect_level(&e, &frame, 2.0f, "both mics at full");
    ds_native_engine_set_control(&e, 0, 0.5f);
    expect_level(&e, &frame, 1.5f, "Close at half, while held");
    ds_native_engine_set_control(&e, 1, 25);
    expect_level(&e, &frame, 0.75f, "mic2 tag at 25");
    /* a MIDI CC drives the Close knob through the preset's own mapping */
    ds_native_engine_cc(&e, 1, 0);
    CHECK(e.control_value[0] == 0);
    expect_level(&e, &frame, 0.25f, "CC1 at 0 turns Close down");
    silence(&e);

    /* the layer button chooses which group a note plays */
    ds_native_engine_note_on(&e, 62, 100);
    CHECK(!strcmp(playing_file(&e), "d.wav"));
    silence(&e);
    ds_native_engine_set_control(&e, 3, 1);
    ds_native_engine_note_on(&e, 62, 100);
    CHECK(!strcmp(playing_file(&e), "c.wav"));
    silence(&e);

    /* the menu retunes: an octave down halves the playback rate */
    ds_native_engine_set_control(&e, 4, 1);
    ds_native_engine_note_on(&e, 60, 127);
    { float out[256]; ds_native_engine_render(&e, out, 128); }
    for (int i = 0; i < DS_MAX_VOICES; ++i) if (e.voices[i].active) CHECK(fabs(e.voices[i].inc - 0.5) < 1e-9);
    silence(&e);

    /* the release knob reaches a HELD note when it is let go */
    ds_native_engine_note_on(&e, 60, 127);
    ds_native_engine_set_control(&e, 2, 0);                /* release 0 -> the 2 ms floor */
    ds_native_engine_note_off(&e, 60);
    { float out[256]; for (int b = 0; b < 4; ++b) ds_native_engine_render(&e, out, 128); }
    CHECK(ds_native_engine_active_voices(&e) == 0);
    ds_native_engine_set_control(&e, 2, 3.5f);
    ds_native_engine_note_on(&e, 60, 127);
    ds_native_engine_note_off(&e, 60);
    { float out[256]; for (int b = 0; b < 40; ++b) ds_native_engine_render(&e, out, 128); }
    CHECK(ds_native_engine_active_voices(&e) > 0);        /* 3.5 s release still ringing at 0.1 s */
    silence(&e);

    /* an effect knob lands its table-translated value on the effect (heard in step 2) */
    CHECK(!strcmp(e.model.effects[0].type, "lowpass") && e.model.effects[0].param_values[0] == 22000);
    ds_native_engine_set_control(&e, 5, 1);
    CHECK(fabsf(e.model.effects[0].param_values[0] - 33) < 1e-3);
    ds_native_engine_set_control(&e, 5, 5.5f);              /* half way: key 5 -> 1000 Hz */
    CHECK(fabsf(e.model.effects[0].param_values[0] - 1000) < 1e-3);

    /* a long table survives whole: Capture's cutoff has 21 points, 0..1 -> 20..22000 Hz */
    {
        ds_preset_model_t m;
        char table_path[512];
        snprintf(table_path, sizeof(table_path), "%s/ctl/table.dspreset", dir);
        write_text(table_path, "<DecentSampler><ui><tab><labeled-knob minValue=\"0\" maxValue=\"1\" value=\"1\">"
            "<binding type=\"effect\" level=\"instrument\" position=\"0\" parameter=\"FX_FILTER_FREQUENCY\" translation=\"table\" "
            "translationTable=\"0.000,20;0.050,40;0.100,60;0.150,80;0.200,100;0.250,200;0.300,300;0.350,400;0.400,500;0.450,600;"
            "0.500,700;0.550,800;0.600,1000;0.650,1250;0.700,1500;0.750,2000;0.800,3000;0.850,4000;0.900,5000;0.950,7000;1.000,22000;\"/>"
            "</labeled-knob></tab></ui><groups><group><sample path=\"a.wav\"/></group></groups></DecentSampler>");
        CHECK(ds_preset_model_load(&m, table_path, error, sizeof(error)) == 0);
        CHECK(m.bindings[0].table_n == 21);
        CHECK(fabsf(ds_binding_translate(&m.bindings[0], 0, 1, 1.0f) - 22000) < 1e-2);
        CHECK(fabsf(ds_binding_translate(&m.bindings[0], 0, 1, 0.5f) - 700) < 1e-2);
        CHECK(fabsf(ds_binding_translate(&m.bindings[0], 0, 1, 0.0f) - 20) < 1e-2);
        ds_preset_model_free(&m);
    }

    ds_native_engine_destroy(&e);
    puts("controls test passed");
    return 0;
}
