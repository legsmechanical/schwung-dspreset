/* The module's own amp envelope: four stepped knobs whose first step is
 * "Preset". Any other step REPLACES the preset's value — including one a
 * sample sets itself — so a release can be made longer than the preset's. */
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
static void none(ds_native_engine_t *e) { for (int i = 0; i < 4; ++i) e->amp_override[i] = -1; }

static const char *PRESET =
    "<DecentSampler><groups attack=\"0\" decay=\"0\" sustain=\"1\" release=\"0.05\"><group>"
    "<sample path=\"tone.wav\" rootNote=\"60\" loNote=\"60\" hiNote=\"60\"/>"
    "<sample path=\"tone.wav\" rootNote=\"62\" loNote=\"62\" hiNote=\"62\" release=\"0.02\"/>"
    "</group></groups></DecentSampler>";

int main(void) {
    const char *dir = getenv("TEST_TMP");
    char path[512], error[128], value[8192], state[2048];
    static ds_native_engine_t e;
    const double full = 0.5 * 0.7071;
    double first, later;
    CHECK(dir);
    snprintf(path, sizeof(path), "mkdir -p '%s/amp/instruments/Amp'", dir); CHECK(system(path) == 0);
    snprintf(path, sizeof(path), "%s/amp/instruments/Amp/tone.wav", dir); write_sine24(path, 30000, 5000, 0.5);
    snprintf(path, sizeof(path), "%s/amp/instruments/Amp/p.dspreset", dir); write_text(path, PRESET);
    CHECK(ds_native_engine_load(&e, path, 44100, NULL, NULL, error, sizeof(error)) == 0);

    /* "Preset" everywhere: the preset's 0.05 s release ends the note well inside 0.2 s */
    none(&e);
    ds_native_engine_note_on(&e, 60, 127); blocks(&e, 10);
    ds_native_engine_note_off(&e, 60); blocks(&e, 70);
    CHECK(ds_native_engine_active_voices(&e) == 0);

    /* Release overridden to 2 s: LONGER than the preset allows, still ringing */
    e.amp_override[3] = 2.0f;
    ds_native_engine_note_on(&e, 60, 127); blocks(&e, 10);
    ds_native_engine_note_off(&e, 60); blocks(&e, 70);
    CHECK(ds_native_engine_active_voices(&e) == 1);
    ds_native_engine_cc(&e, 120, 0); blocks(&e, 1);
    /* ...and it wins over a release the SAMPLE sets itself (note 62: 0.02 s) */
    ds_native_engine_note_on(&e, 62, 127); blocks(&e, 10);
    ds_native_engine_note_off(&e, 62); blocks(&e, 70);
    CHECK(ds_native_engine_active_voices(&e) == 1);
    ds_native_engine_cc(&e, 120, 0); blocks(&e, 1);
    none(&e);

    /* Attack overridden to 0.5 s: the first block is a sliver of the steady level */
    e.amp_override[0] = 0.5f;
    ds_native_engine_note_on(&e, 60, 127);
    first = block_rms(&e);
    blocks(&e, 200);                                   /* 0.58 s: past the attack, inside the 0.68 s tone */
    later = block_rms(&e);
    printf("  attack 0.5 s: %.4f in the first block, %.4f after\n", first, later);
    CHECK(first < full * 0.02 && fabs(later - full) < 0.005);
    ds_native_engine_cc(&e, 120, 0); blocks(&e, 1);
    none(&e);

    /* Sustain overridden to 50%, decay 0: the note settles exactly there... */
    e.amp_override[2] = 0.5f;
    ds_native_engine_note_on(&e, 60, 127); blocks(&e, 5);
    CHECK(fabs(block_rms(&e) - full * 0.5) < 0.002);
    /* ...and moving it while the note is held glides there, never jumps */
    e.amp_override[2] = 1.0f;
    first = block_rms(&e);
    blocks(&e, 60);
    later = block_rms(&e);
    printf("  sustain 50%% -> 100%% while held: %.4f, then %.4f\n", first, later);
    CHECK(first < full * 0.9 && fabs(later - full) < 0.003);
    ds_native_engine_destroy(&e);

    /* Through the plugin: an Override switch then four NUMERIC knobs, adjacent
     * and named *_attack.._release (what both hosts draw as an envelope).
     * Off: the preset plays, and the knobs show ITS envelope. On: they replace it. */
    {
        plugin_t p, q;
        int16_t out[256];
        char mod[512];
        snprintf(mod, sizeof(mod), "%s/amp", dir);
        plugin_open_in(&p, mod);
        usleep(700000);
        for (int i = 0; i < 300 && plugin_uint(&p, "load_count") == 0; ++i) usleep(10000);
        plugin_get(&p, "chain_params", value, sizeof(value));
        CHECK(strstr(value, "{\"key\":\"amp_override\",\"name\":\"Override\",\"type\":\"enum\",\"options\":[\"Off\",\"On\"]"));
        CHECK(strstr(value, "{\"key\":\"amp_release\",\"name\":\"Release\",\"type\":\"float\",\"min\":0,\"max\":20,\"step\":0.001,\"unit\":\"sec\""));
        CHECK(strstr(value, "{\"key\":\"amp_sustain\",\"name\":\"Sustain\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01,\"unit\":\"%\""));
        plugin_get(&p, "ui_hierarchy", value, sizeof(value));
        CHECK(strstr(value, "{\"level\":\"amp\",\"label\":\"Amp Envelope\"}"));
        CHECK(strstr(value, "\"knobs\":[\"amp_attack\",\"amp_decay\",\"amp_sustain\",\"amp_release\",\"amp_override\"]"));
        /* Off by default, knobs mirroring the preset: release 0.05 s, sustain 100% */
        CHECK(plugin_uint(&p, "amp_override") == 0);
        plugin_get(&p, "amp_release", value, sizeof(value)); CHECK(!strcmp(value, "0.0500"));
        plugin_get(&p, "amp_sustain", value, sizeof(value)); CHECK(!strcmp(value, "1.0000"));
        /* a knob turned while Off is kept but not applied: the preset's release still ends the note */
        p.api->set_param(p.instance, "amp_release", "5");
        clock_gettime(CLOCK_MONOTONIC, &p.next);
        plugin_midi(&p, 0x90, 60, 127);
        for (int b = 0; b < 10; ++b) plugin_render(&p, out);
        plugin_midi(&p, 0x80, 60, 0);
        for (int b = 0; b < 70; ++b) plugin_render(&p, out);
        CHECK(plugin_uint(&p, "voices") == 0);
        /* On: the 5 s release applies */
        p.api->set_param(p.instance, "amp_override", "1");
        plugin_midi(&p, 0x90, 60, 127);
        for (int b = 0; b < 10; ++b) plugin_render(&p, out);
        plugin_midi(&p, 0x80, 60, 0);
        for (int b = 0; b < 70; ++b) plugin_render(&p, out);
        CHECK(plugin_uint(&p, "voices") == 1);
        plugin_midi(&p, 0xb0, 120, 0);
        /* turned while a note is held, with no MIDI arriving: it still reaches the note */
        {
            double half = 0, whole = 0;
            p.api->set_param(p.instance, "amp_sustain", "0.5");
            plugin_midi(&p, 0x90, 60, 127);
            for (int b = 0; b < 10; ++b) plugin_render(&p, out);
            for (int i = 0; i < 128; ++i) half += (double)out[2 * i] * out[2 * i];
            p.api->set_param(p.instance, "amp_sustain", "1");
            for (int b = 0; b < 30; ++b) plugin_render(&p, out);
            for (int i = 0; i < 128; ++i) whole += (double)out[2 * i] * out[2 * i];
            printf("  sustain turned 50%% -> 100%% on a held note: %.0f -> %.0f (rms, int16)\n", sqrt(half / 128), sqrt(whole / 128));
            CHECK(sqrt(whole / half) > 1.9 && sqrt(whole / half) < 2.1);
            plugin_midi(&p, 0x80, 60, 0);
            plugin_midi(&p, 0xb0, 120, 0);
        }
        /* saved with the project, and restored: switch and values */
        plugin_get(&p, "state", state, sizeof(state));
        CHECK(strstr(state, "\"amp\":\"1;0;0;1;5\""));
        plugin_open_in(&q, mod);
        q.api->set_param(q.instance, "state", state);
        CHECK(plugin_uint(&q, "amp_override") == 1);
        plugin_get(&q, "amp_release", value, sizeof(value)); CHECK(!strcmp(value, "5.0000"));
        /* Off again: the preset's own envelope comes back */
        p.api->set_param(p.instance, "amp_override", "0");
        plugin_midi(&p, 0x90, 60, 127);
        for (int b = 0; b < 10; ++b) plugin_render(&p, out);
        plugin_midi(&p, 0x80, 60, 0);
        for (int b = 0; b < 70; ++b) plugin_render(&p, out);
        CHECK(plugin_uint(&p, "voices") == 0);
        plugin_close(&q);
        plugin_close(&p);
    }
    puts("amp envelope test passed");
    return 0;
}
