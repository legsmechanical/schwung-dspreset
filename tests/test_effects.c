/* Filter, EQ and gain: measured responses, exact pass-through when an effect
 * is wide open or off, and in the engine that each binding reaches the effect
 * it names — instrument-level on the mix, group-level inside each note. */
#include "test_support.h"

#include "../src/dsp/dspreset/effects.h"
#include "../src/dsp/dspreset/native_engine.h"

/* Steady-state gain of `fx` at `hz`: the RMS out over the RMS in, after the
 * filter has settled. */
static double response(const char *type, const char *p1, float v1, const char *p2, float v2, double hz) {
    ds_effect_t fx;
    ds_fx_coeffs_t c;
    ds_fx_state_t st;
    static float buf[2 * 44100];
    double in = 0, out = 0;
    memset(&fx, 0, sizeof(fx));
    memset(&st, 0, sizeof(st));
    snprintf(fx.type, sizeof(fx.type), "%s", type);
    fx.enabled = 1;
    if (p1) { snprintf(fx.param_names[fx.param_count], 24, "%s", p1); fx.param_values[fx.param_count++] = v1; }
    if (p2) { snprintf(fx.param_names[fx.param_count], 24, "%s", p2); fx.param_values[fx.param_count++] = v2; }
    ds_fx_prepare(&c, &fx, 44100);
    for (int i = 0; i < 44100; ++i) buf[2 * i] = buf[2 * i + 1] = (float)sin(2 * M_PI * hz * i / 44100.0);
    for (int i = 22050; i < 44100; ++i) in += buf[2 * i] * buf[2 * i];
    for (int b = 0; b < 44100; b += 128) ds_fx_process(&c, &st, buf + 2 * b, 44100 - b < 128 ? (unsigned)(44100 - b) : 128);
    for (int i = 22050; i < 44100; ++i) out += buf[2 * i] * buf[2 * i];
    return sqrt(out / in);
}

static void near(double got, double want, double tol, const char *what) {
    printf("  %-34s %.4f (want %.4f)\n", what, got, want);
    if (fabs(got - want) > tol) { fprintf(stderr, "FAIL %s\n", what); exit(1); }
}

static const char *PRESET =
    "<DecentSampler>\n"
    "<ui><tab>\n"
    "  <labeled-knob label=\"Cut\" minValue=\"0\" maxValue=\"1\" value=\"1\">\n"
    "    <binding type=\"effect\" level=\"instrument\" position=\"1\" parameter=\"FX_FILTER_FREQUENCY\" translation=\"linear\" translationOutputMin=\"100\" translationOutputMax=\"22000\"/></labeled-knob>\n"
    "  <labeled-knob label=\"Group Cut\" minValue=\"0\" maxValue=\"1\" value=\"1\">\n"
    "    <binding type=\"effect\" level=\"group\" groupIndex=\"1\" effectIndex=\"0\" parameter=\"FX_FILTER_FREQUENCY\" translation=\"linear\" translationOutputMin=\"100\" translationOutputMax=\"22000\"/></labeled-knob>\n"
    "  <button value=\"0\" parameterName=\"Trim\">\n"
    "    <state name=\"On\"><binding type=\"effect\" level=\"instrument\" position=\"0\" parameter=\"ENABLED\" translation=\"fixed_value\" translationValue=\"true\"/></state>\n"
    "    <state name=\"Off\"><binding type=\"effect\" level=\"instrument\" position=\"0\" parameter=\"ENABLED\" translation=\"fixed_value\" translationValue=\"false\"/></state>\n"
    "  </button>\n"
    "</tab></ui>\n"
    "<groups attack=\"0\" release=\"0.01\">\n"
    "  <group><sample path=\"hi.wav\" rootNote=\"60\" loNote=\"60\" hiNote=\"60\"/></group>\n"
    "  <group><sample path=\"hi.wav\" rootNote=\"62\" loNote=\"62\" hiNote=\"62\"/>\n"
    "    <effects><effect type=\"lowpass\" frequency=\"22000\"/></effects></group>\n"
    "</groups>\n"
    "<effects><effect type=\"gain\" level=\"-6\"/><effect type=\"lowpass\" frequency=\"22000\"/></effects>\n"
    "</DecentSampler>\n";

/* RMS of the left channel over ~0.3 s of one held note, after 0.1 s to settle. */
static double note_rms(ds_native_engine_t *e, int note) {
    float out[256];
    double sum = 0;
    int n = 0;
    ds_native_engine_note_on(e, note, 127);
    for (int b = 0; b < 140; ++b) {
        memset(out, 0, sizeof(out));
        ds_native_engine_render(e, out, 128);
        if (b >= 35) for (int i = 0; i < 128; ++i, ++n) sum += out[2 * i] * out[2 * i];
    }
    ds_native_engine_cc(e, 120, 0);
    memset(out, 0, sizeof(out));
    ds_native_engine_render(e, out, 128);
    return sqrt(sum / n);
}

int main(void) {
    const char *dir = getenv("TEST_TMP");
    char path[512], error[128];
    static ds_native_engine_t e;
    double open_level;
    CHECK(dir);

    /* responses, RBJ/JUCE shapes */
    near(response("lowpass", "frequency", 1000, "resonance", 0.7071f, 100), 1.0, 0.02, "lowpass 1k @ 100 Hz");
    near(response("lowpass", "frequency", 1000, "resonance", 0.7071f, 1000), 0.7071, 0.02, "lowpass 1k @ 1 kHz (-3 dB)");
    near(response("lowpass", "frequency", 1000, "resonance", 0.7071f, 10000), 0.0, 0.02, "lowpass 1k @ 10 kHz");
    near(response("lowpass_4pl", "frequency", 1000, "resonance", 0.7071f, 1000), 0.7071, 0.02, "lowpass_4pl is lowpass");
    near(response("highpass", "frequency", 1000, "resonance", 0.7071f, 100), 0.0, 0.02, "highpass 1k @ 100 Hz");
    near(response("highpass", "frequency", 1000, "resonance", 0.7071f, 10000), 1.0, 0.03, "highpass 1k @ 10 kHz");
    near(response("bandpass", "frequency", 1000, "resonance", 1, 1000), 1.0, 0.02, "bandpass 1k @ 1 kHz");
    near(response("notch", "frequency", 1000, "q", 0.7f, 1000), 0.0, 0.03, "notch 1k @ 1 kHz");
    near(response("notch", "frequency", 1000, "q", 0.7f, 100), 1.0, 0.03, "notch 1k @ 100 Hz");
    near(response("peak", "frequency", 1000, "gain", 2, 1000), 2.0, 0.03, "peak gain 2 @ centre (+6 dB)");
    near(response("peak", "frequency", 1000, "gain", 0.5f, 1000), 0.5, 0.02, "peak gain 0.5 @ centre");
    near(response("lowpass_1pl", "frequency", 1000, NULL, 0, 1000), 0.7071, 0.04, "one-pole 1k @ 1 kHz");
    near(response("gain", "level", -6, NULL, 0, 440), 0.5012, 0.001, "gain -6 dB");
    near(response("gain", "level", 0.5f, "levelLinear", 1, 440), 0.5, 0.001, "gain 0.5 linear");
    /* wide open / unity / off are exact pass-throughs, not merely close */
    CHECK(response("lowpass", "frequency", 22000, NULL, 0, 15000) == 1.0);
    CHECK(response("peak", "frequency", 1000, "gain", 1, 1000) == 1.0);
    CHECK(response("reverb", "wetLevel", 1, NULL, 0, 1000) == 1.0);   /* not rendered yet: passes through */

    /* in the engine */
    snprintf(path, sizeof(path), "mkdir -p '%s/fx'", dir); CHECK(system(path) == 0);
    snprintf(path, sizeof(path), "%s/fx/hi.wav", dir); write_sine24(path, 30000, 5000, 0.5);   /* under DS_RESIDENT_FRAMES: no worker here to stream */
    snprintf(path, sizeof(path), "%s/fx/p.dspreset", dir); write_text(path, PRESET);
    CHECK(ds_native_engine_load(&e, path, 44100, NULL, NULL, error, sizeof(error)) == 0);
    /* document order: the group's lowpass, then the instrument's gain and lowpass */
    CHECK(e.model.effect_count == 3 && e.model.effects[0].group == 1 && e.model.effects[1].group == -1);
    CHECK(e.model.bindings[e.model.controls[0].first_binding].effect == 2);    /* Cut -> instrument effect #1 */
    CHECK(e.model.bindings[e.model.controls[1].first_binding].effect == 0);    /* Group Cut -> group 1, effect #0 */

    open_level = note_rms(&e, 60);
    near(open_level, 0.5 * 0.7071 * 0.5012, 0.005, "note through -6 dB trim");
    ds_native_engine_set_control(&e, 2, 1);                   /* Trim Off: its gain effect disabled */
    near(note_rms(&e, 60), 0.5 * 0.7071, 0.005, "trim switched off");
    ds_native_engine_set_control(&e, 0, 0);                   /* Cut to 100 Hz: the 5 kHz tone is gone */
    CHECK(note_rms(&e, 60) < 0.005);
    ds_native_engine_set_control(&e, 0, 1);
    ds_native_engine_set_control(&e, 1, 0);                   /* the GROUP's filter: only its notes */
    near(note_rms(&e, 60), 0.5 * 0.7071, 0.005, "other group untouched");
    CHECK(note_rms(&e, 62) < 0.005);
    ds_native_engine_destroy(&e);
    puts("effects test passed");
    return 0;
}
