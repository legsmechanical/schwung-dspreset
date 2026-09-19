/* Controls driving controls, and the ways a binding names what it drives:
 * an <xyPad> becomes an X and a Y knob (an axis nothing is bound to is no
 * knob, BassForge's filter pads); a CC names a control by its parameterName
 * and, with no range of its own, spans the control's range (BassForge's CC
 * maps); a pad does not shift DecentSampler's numbering of the controls after
 * it; controlTags / modulatorTags / sampleTags; a binding with
 * enabled="false"; and triggerOnLoad="false" held back at load AND at a
 * project restore through the real plugin. */
#include "test_support.h"

#include "../src/dsp/dspreset/native_engine.h"

#define SR 44100

static float sig(uint64_t f) { return test_signal24(f, 0) / 8388607.0f; }

static void load(ds_native_engine_t *e, const char *dir, const char *xml) {
    char path[512], error[128];
    snprintf(path, sizeof(path), "%s/uib/p.dspreset", dir);
    write_text(path, xml);
    CHECK(ds_native_engine_load(e, path, SR, NULL, NULL, error, sizeof(error)) == 0);
    for (int i = 0; i < 4; ++i) e->amp_override[i] = -1;
}

static float fx_param(ds_native_engine_t *e, unsigned x, const char *name) { return ds_fx_param(&e->model.effects[x], name, -1); }

static void settle(plugin_t *p) {
    for (int i = 0; i < 500; ++i) { usleep(10000); if (i > 20 && !plugin_uint(p, "is_loading")) break; }
}

int main(void) {
    const char *dir = getenv("TEST_TMP");
    char path[2048];
    static ds_native_engine_t e;
    float out[256];
    CHECK(dir);
    snprintf(path, sizeof(path), "mkdir -p '%s/uib'", dir); CHECK(system(path) == 0);
    snprintf(path, sizeof(path), "%s/uib/a.wav", dir); write_wav24(path, SR, 1, 30000, -1, -1);

    /* pads, CC by name, numbering after a pad */
    load(&e, dir,
         "<DecentSampler><ui><tab>"
         "<xyPad parameterName=\"LowpassXY\" xValue=\"0.0\" yValue=\"0.25\"><x></x>"
         "<y><binding type=\"effect\" level=\"instrument\" effectIndex=\"0\" parameter=\"FX_FILTER_FREQUENCY\" translation=\"linear\""
         " translationOutputMin=\"100\" translationOutputMax=\"10100\"/></y></xyPad>"
         "<xyPad parameterName=\"DriveXY\" xValue=\"0.5\" yValue=\"0.0\">"
         "<x><binding type=\"effect\" level=\"instrument\" effectIndex=\"1\" parameter=\"FX_FILTER_FREQUENCY\" translation=\"linear\""
         " translationOutputMin=\"0\" translationOutputMax=\"1000\"/></x>"
         "<y><binding type=\"effect\" level=\"instrument\" effectIndex=\"1\" parameter=\"FX_FILTER_Q\" translation=\"linear\""
         " translationOutputMin=\"0\" translationOutputMax=\"10\"/></y></xyPad>"
         "<labeled-knob label=\"Vol\" parameterName=\"DriveVolume\" minValue=\"0\" maxValue=\"2\" value=\"1\">"
         "<binding type=\"amp\" level=\"group\" position=\"0\" parameter=\"AMP_VOLUME\" translation=\"linear\" translationOutputMin=\"0\" translationOutputMax=\"2\"/>"
         "</labeled-knob></tab></ui>"
         "<groups><group><sample path=\"a.wav\" rootNote=\"60\" ampEnvEnabled=\"false\"/></group></groups>"
         "<effects><effect type=\"phaser\" frequency=\"22000\"/><effect type=\"peak\" frequency=\"1000\"/></effects>"   /* both pass audio through: only their settings are checked */
         "<midi><cc number=\"1\"><binding level=\"ui\" type=\"control\" parameter=\"DriveVolume\" translation=\"linear\"/></cc>"
         "<cc number=\"2\"><binding level=\"ui\" type=\"control\" parameter=\"DriveXY\" translation=\"linear\"/></cc>"
         "<cc number=\"3\"><binding level=\"ui\" type=\"control\" parameter=\"Y_VALUE\" position=\"1\" translation=\"linear\"/></cc>"
         "<cc number=\"4\"><binding level=\"ui\" type=\"control\" parameter=\"VALUE\" position=\"2\" translation=\"linear\"/></cc>"
         "<cc number=\"5\"><binding level=\"ui\" type=\"control\" parameter=\"LowpassXY\" translation=\"linear\"/></cc>"
         "</midi></DecentSampler>");
    CHECK(e.model.control_count == 4);                       /* Lowpass (Y only), Drive X, Drive Y, Vol */
    CHECK(!strcmp(e.model.controls[0].name, "Lowpass") && !strcmp(e.model.controls[1].name, "Drive X"));
    CHECK(!strcmp(e.model.controls[2].name, "Drive Y") && !strcmp(e.model.controls[3].name, "Vol"));
    CHECK(e.control_value[0] == 0.25f && e.control_value[1] == 0.5f && e.control_value[2] == 0.0f);   /* yValue / xValue */
    CHECK(fx_param(&e, 0, "frequency") == 2600.0f && fx_param(&e, 1, "frequency") == 500.0f);
    ds_native_engine_set_control(&e, 2, 0.5f);               /* Drive Y moves Q only */
    CHECK(fx_param(&e, 1, "q") == 5.0f && fx_param(&e, 1, "frequency") == 500.0f);
    ds_native_engine_cc(&e, 1, 127); CHECK(e.control_value[3] == 2.0f);        /* the knob's whole range */
    ds_native_engine_cc(&e, 1, 64);  CHECK(fabsf(e.control_value[3] - 2.0f * 64 / 127) < 1e-6f);
    ds_native_engine_note_on(&e, 60, 127);
    memset(out, 0, sizeof(out));
    ds_native_engine_render(&e, out, 128);
    for (int i = 0; i < 128; ++i) CHECK(fabsf(out[2 * i] - (2.0f * 64 / 127) * sig(i)) < 1e-5f);   /* and it sounds */
    ds_native_engine_cc(&e, 2, 127); CHECK(e.control_value[1] == 1.0f);        /* a pad by name: its X */
    ds_native_engine_cc(&e, 3, 0);   CHECK(e.control_value[2] == 0.0f);        /* Y_VALUE of DS control 1 */
    ds_native_engine_cc(&e, 4, 0);   CHECK(e.control_value[3] == 0.0f);        /* DS control 2 is Vol, not Drive Y */
    ds_native_engine_cc(&e, 5, 127); CHECK(e.control_value[0] == 1.0f);        /* a one-axis pad: its only knob */
    ds_native_engine_destroy(&e);

    /* controlTags, modulatorTags, sampleTags; a disabled binding */
    load(&e, dir,
         "<DecentSampler><ui><tab>"
         "<labeled-knob minValue=\"0\" maxValue=\"1\" value=\"1\" tags=\"vol\"/>"
         "<labeled-knob minValue=\"0\" maxValue=\"1\" value=\"1\" tags=\"vol\"/>"
         "<labeled-knob minValue=\"0\" maxValue=\"1\" value=\"1\"/>"
         "<button value=\"0\"><state name=\"Off\"/><state name=\"Quarter\">"
         "<binding type=\"control\" level=\"ui\" controlTags=\"vol\" parameter=\"VALUE\" translation=\"fixed_value\" translationValue=\"0.25\"/>"
         "<binding type=\"control\" level=\"ui\" position=\"2\" parameter=\"VALUE\" translation=\"fixed_value\" translationValue=\"0.25\" enabled=\"false\"/>"
         "</state></button>"
         "<labeled-knob minValue=\"0\" maxValue=\"10\" value=\"1\"><binding type=\"modulator\" level=\"instrument\" modulatorTags=\"vib\" parameter=\"FREQUENCY\"/></labeled-knob>"
         "<labeled-knob minValue=\"0\" maxValue=\"1\" value=\"1\"><binding type=\"amp\" level=\"sample\" sampleTags=\"mic1\" parameter=\"AMP_VOLUME\"/></labeled-knob>"
         "</tab></ui>"
         "<groups><group><sample path=\"a.wav\" rootNote=\"60\" ampEnvEnabled=\"false\" tags=\"mic1\"/>"
         "<sample path=\"a.wav\" rootNote=\"60\" ampEnvEnabled=\"false\" tags=\"mic2\"/></group></groups>"
         "<modulators><lfo frequency=\"1\" modAmount=\"0\" tags=\"vib\"/><lfo frequency=\"1\" modAmount=\"0\" tags=\"vib\"/>"
         "<lfo frequency=\"1\" modAmount=\"0\"/></modulators></DecentSampler>");
    ds_native_engine_set_control(&e, 3, 1);
    CHECK(e.control_value[0] == 0.25f && e.control_value[1] == 0.25f && e.control_value[2] == 1.0f);
    ds_native_engine_set_control(&e, 4, 7);
    CHECK(e.model.modulators[0].frequency == 7.0f && e.model.modulators[1].frequency == 7.0f && e.model.modulators[2].frequency == 1.0f);
    ds_native_engine_set_control(&e, 5, 0.5f);               /* mic1 at half, mic2 untouched: 1.5 x */
    ds_native_engine_note_on(&e, 60, 127);
    memset(out, 0, sizeof(out));
    ds_native_engine_render(&e, out, 128);
    for (int i = 0; i < 128; ++i) CHECK(fabsf(out[2 * i] - 1.5f * sig(i)) < 1e-5f);
    ds_native_engine_destroy(&e);

    /* triggerOnLoad="false": the button (AFTER the knob, so a restore sets the
     * knob first) must not overwrite the knob at load nor at a restore, but
     * does when someone presses it */
    {
        static const char *PRESET =
            "<DecentSampler><ui><tab>"
            "<labeled-knob minValue=\"0\" maxValue=\"1\" value=\"0.7\"><binding type=\"amp\" level=\"group\" position=\"0\" parameter=\"AMP_VOLUME\"/></labeled-knob>"
            "<button value=\"0\"><state name=\"A\"><binding type=\"control\" level=\"ui\" position=\"0\" parameter=\"VALUE\""
            " translation=\"fixed_value\" translationValue=\"0.1\" triggerOnLoad=\"false\"/></state>"
            "<state name=\"B\"><binding type=\"control\" level=\"ui\" position=\"0\" parameter=\"VALUE\""
            " translation=\"fixed_value\" translationValue=\"0.1\" triggerOnLoad=\"false\"/></state></button>"
            "</tab></ui><groups><group><sample path=\"a.wav\" rootNote=\"60\"/></group></groups></DecentSampler>";
        char mod[512], value[64], state[2048];
        plugin_t p, q;
        int16_t pcm[256];
        load(&e, dir, PRESET);
        CHECK(e.control_value[0] == 0.7f);
        ds_native_engine_set_control(&e, 1, 1);
        CHECK(e.control_value[0] == 0.1f);
        ds_native_engine_destroy(&e);

        snprintf(mod, sizeof(mod), "%s/uib-module", dir);
        snprintf(path, sizeof(path), "rm -rf '%s' && mkdir -p '%s/instruments/T'", mod, mod); CHECK(system(path) == 0);
        snprintf(path, sizeof(path), "%s/instruments/T/a.wav", mod); write_wav24(path, SR, 1, 30000, -1, -1);
        snprintf(path, sizeof(path), "%s/instruments/T/1 T.dspreset", mod); write_text(path, PRESET);
        plugin_open_in(&p, mod);
        usleep(600000);
        settle(&p);
        p.api->set_param(p.instance, "ctl_0", "0.3");
        plugin_render(&p, pcm);
        plugin_get(&p, "ctl_0", value, sizeof(value));
        CHECK(fabs(atof(value) - 0.3) < 1e-6);
        plugin_get(&p, "state", state, sizeof(state));
        plugin_open_in(&q, mod);
        q.api->set_param(q.instance, "state", state);
        usleep(600000);
        settle(&q);
        plugin_render(&q, pcm);
        plugin_get(&q, "ctl_0", value, sizeof(value));
        printf("  restored knob: %s (the button's 0.1 held back)\n", value);
        CHECK(fabs(atof(value) - 0.3) < 1e-6);
        plugin_close(&p); plugin_close(&q);
    }

    puts("ui bindings test passed");
    return 0;
}
