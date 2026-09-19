/* Voices that stop other voices: silencedByTags (an open hi-hat cut by the
 * closed one) with silencingMode fast / normal and silencingDecay, read from
 * the VICTIM; one key's own layers never cutting each other; a knob moving
 * the decay; and per-tag voice limits from <tags><tag polyphony> (DecenTron's
 * mono tape-noise layer), with TAG_POLYPHONY moving the limit and <tag>
 * setting a tag's starting volume and on/off. */
#include "test_support.h"

#include "../src/dsp/dspreset/native_engine.h"

#define SR 44100

static float coef(float seconds) { return expf(logf(0.001f) / (seconds * SR)); }

static void blocks(ds_native_engine_t *e, int n) {
    float out[256];
    while (n--) { memset(out, 0, sizeof(out)); ds_native_engine_render(e, out, 128); }
}

static void load(ds_native_engine_t *e, const char *dir, const char *name, const char *xml) {
    char path[512], error[128];
    snprintf(path, sizeof(path), "%s/choke/%s", dir, name);
    write_text(path, xml);
    CHECK(ds_native_engine_load(e, path, SR, NULL, NULL, error, sizeof(error)) == 0);
    for (int i = 0; i < 4; ++i) e->amp_override[i] = -1;
}

/* The one sounding voice of `note`, or NULL. */
static ds_voice_t *voice_of(ds_native_engine_t *e, int note) {
    ds_voice_t *found = NULL;
    for (int i = 0; i < DS_MAX_VOICES; ++i)
        if (e->voices[i].active && e->voices[i].note == note) { CHECK(!found); found = &e->voices[i]; }
    return found;
}

static int tag_index(ds_native_engine_t *e, const char *name) {
    for (unsigned t = 0; t < e->model.tag_count; ++t) if (!strcmp(e->model.tag_names[t], name)) return (int)t;
    CHECK(0);
    return -1;
}

/* Voices carrying tag `name`, not silenced. */
static int live_with_tag(ds_native_engine_t *e, const char *name) {
    uint64_t bit = 0;
    int n = 0;
    for (unsigned t = 0; t < e->model.tag_count; ++t) if (!strcmp(e->model.tag_names[t], name)) bit = 1ull << t;
    CHECK(bit);
    for (int i = 0; i < DS_MAX_VOICES; ++i)
        if (e->voices[i].active && !e->voices[i].choked && (e->voices[i].zone->tag_mask & bit)) n++;
    return n;
}

int main(void) {
    const char *dir = getenv("TEST_TMP");
    char path[512];
    static ds_native_engine_t e;
    CHECK(dir);
    snprintf(path, sizeof(path), "mkdir -p '%s/choke'", dir); CHECK(system(path) == 0);
    snprintf(path, sizeof(path), "%s/choke/t.wav", dir); write_sine24(path, 88200, 440, 0.5);

    /* fast (the default): the closed hat cuts the open one in 5 ms */
    load(&e, dir, "hat.dspreset",
         "<DecentSampler><groups attack=\"0\" release=\"1.0\">"
         "<group tags=\"open\" silencedByTags=\"closed\"><sample path=\"t.wav\" rootNote=\"62\" loNote=\"62\" hiNote=\"62\"/></group>"
         "<group tags=\"closed\"><sample path=\"t.wav\" rootNote=\"60\" loNote=\"60\" hiNote=\"60\"/></group>"
         "</groups></DecentSampler>");
    ds_native_engine_note_on(&e, 62, 127); blocks(&e, 2);
    ds_native_engine_note_on(&e, 60, 127);
    {
        ds_voice_t *open = voice_of(&e, 62);
        CHECK(open && open->choked && open->env_stage == DS_ENV_RELEASE && open->env_release_coef == coef(0.005f));
        CHECK(!voice_of(&e, 60)->choked);
    }
    blocks(&e, 3);
    CHECK(!voice_of(&e, 62) && voice_of(&e, 60));
    /* ...and not the other way round: the open hat does not cut the closed one */
    ds_native_engine_note_on(&e, 62, 127);
    CHECK(!voice_of(&e, 60)->choked);
    ds_native_engine_destroy(&e);

    /* normal: the victim's own 1 s release; silencingDecay overrides the mode;
     * the settings are the VICTIM's (the closed hat says fast, and is ignored) */
    load(&e, dir, "normal.dspreset",
         "<DecentSampler><groups attack=\"0\" release=\"1.0\">"
         "<group tags=\"open\" silencedByTags=\"closed\" silencingMode=\"normal\"><sample path=\"t.wav\" rootNote=\"62\" loNote=\"62\" hiNote=\"62\"/></group>"
         "<group tags=\"ride\" silencedByTags=\"closed\" silencingMode=\"normal\" silencingDecay=\"0.05\"><sample path=\"t.wav\" rootNote=\"64\" loNote=\"64\" hiNote=\"64\"/></group>"
         "<group tags=\"closed\" silencingMode=\"fast\"><sample path=\"t.wav\" rootNote=\"60\" loNote=\"60\" hiNote=\"60\"/></group>"
         "</groups></DecentSampler>");
    ds_native_engine_note_on(&e, 62, 127);
    ds_native_engine_note_on(&e, 64, 127); blocks(&e, 2);
    ds_native_engine_note_on(&e, 60, 127);
    CHECK(voice_of(&e, 62)->choked && voice_of(&e, 62)->env_release_coef == coef(1.0f));
    CHECK(voice_of(&e, 64)->choked && voice_of(&e, 64)->env_release_coef == coef(0.05f));
    blocks(&e, 20);
    CHECK(voice_of(&e, 62) && !voice_of(&e, 64));              /* 1 s still ringing; 50 ms gone */
    ds_native_engine_destroy(&e);

    /* one key's layers, each silencing the other's tag, both keep playing; a
     * knob (SILENCING_DECAY, group level) reaches the next silencing */
    load(&e, dir, "layers.dspreset",
         "<DecentSampler><ui><tab><labeled-knob minValue=\"0\" maxValue=\"1\" value=\"0\">"
         "<binding type=\"general\" level=\"group\" position=\"0\" parameter=\"SILENCING_DECAY\"/></labeled-knob>"
         "<button value=\"0\"><state name=\"Fast\"><binding type=\"general\" level=\"group\" position=\"1\" parameter=\"SILENCING_MODE\" translation=\"fixed_value\" translationValue=\"fast\"/></state>"
         "<state name=\"Normal\"><binding type=\"general\" level=\"group\" position=\"1\" parameter=\"SILENCING_MODE\" translation=\"fixed_value\" translationValue=\"normal\"/></state></button>"
         "</tab></ui>"
         "<groups attack=\"0\" release=\"1.0\">"
         "<group tags=\"hh\" silencedByTags=\"hh\"><sample path=\"t.wav\" rootNote=\"60\"/></group>"
         "<group tags=\"hh\" silencedByTags=\"hh\"><sample path=\"t.wav\" rootNote=\"60\"/></group>"
         "</groups></DecentSampler>");
    ds_native_engine_note_on(&e, 60, 127);
    CHECK(ds_native_engine_active_voices(&e) == 2 && !e.voices[0].choked && !e.voices[1].choked);
    ds_native_engine_set_control(&e, 0, 0.25f);
    ds_native_engine_set_control(&e, 1, 1);                    /* group 1: "normal" (its 1 s release) */
    ds_native_engine_note_on(&e, 62, 127);                     /* the next key cuts both of 60's */
    for (int i = 0; i < DS_MAX_VOICES; ++i)
        if (e.voices[i].active && e.voices[i].note == 60) {
            CHECK(e.voices[i].choked);
            /* group 0 took the knob's 0.25 s; group 1 the button's "normal" */
            CHECK(e.voices[i].env_release_coef == coef(e.voices[i].zone->def.group_index == 0 ? 0.25f : 1.0f));
        }
    ds_native_engine_destroy(&e);

    /* per-tag polyphony: DecenTron's shape. Tape is mono, Tron is not; <tag>
     * also sets a starting volume (and an off tag does not sound). */
    load(&e, dir, "tags.dspreset",
         "<DecentSampler><ui><tab><button value=\"0\">"
         "<state name=\"1\"><binding type=\"general\" level=\"tag\" identifier=\"Tape\" parameter=\"TAG_POLYPHONY\" translation=\"fixed_value\" translationValue=\"1\"/></state>"
         "<state name=\"2\"><binding type=\"general\" level=\"tag\" identifier=\"Tape\" parameter=\"TAG_POLYPHONY\" translation=\"fixed_value\" translationValue=\"2\"/></state>"
         "</button></tab></ui>"
         "<groups attack=\"0\" release=\"1.0\">"
         "<group tags=\"Tron\"><sample path=\"t.wav\" rootNote=\"60\"/></group>"
         "<group tags=\"Tape\"><sample path=\"t.wav\" rootNote=\"55\" pitchKeyTrack=\"0\"/><sample path=\"t.wav\" rootNote=\"55\" pitchKeyTrack=\"0\"/></group>"
         "<group tags=\"Off\"><sample path=\"t.wav\" rootNote=\"60\"/></group>"
         "</groups>"
         "<tags><tag name=\"Tron\" volume=\"0.5\"/><tag name=\"Tape\" polyphony=\"1\" enabled=\"true\" volume=\"1\"/>"
         "<tag name=\"Off\" enabled=\"false\"/></tags></DecentSampler>");
    CHECK(e.tag_volume[tag_index(&e, "Tron")] == 0.5f && e.tag_polyphony[tag_index(&e, "Tape")] == 1);
    CHECK(!e.tag_enabled[tag_index(&e, "Off")] && e.tag_polyphony[tag_index(&e, "Tron")] == -1);
    ds_native_engine_note_on(&e, 60, 127);
    CHECK(live_with_tag(&e, "Tape") == 2 && live_with_tag(&e, "Tron") == 1);   /* one key's two tape layers both play */
    CHECK(live_with_tag(&e, "Off") == 0);
    blocks(&e, 2);
    ds_native_engine_note_on(&e, 62, 127);
    CHECK(live_with_tag(&e, "Tape") == 2 && live_with_tag(&e, "Tron") == 2);  /* 60's tape layers cut, 62's two play */
    for (int i = 0; i < DS_MAX_VOICES; ++i)
        if (e.voices[i].active && e.voices[i].note == 60 && e.voices[i].zone->def.group_index == 1) CHECK(e.voices[i].choked);
    ds_native_engine_cc(&e, 120, 0); blocks(&e, 1);
    /* limit 1 with one layer per key: the OLDEST goes */
    ds_native_engine_set_control(&e, 0, 1);                    /* state "2": two tape voices */
    CHECK(e.tag_polyphony[tag_index(&e, "Tape")] == 2);
    ds_native_engine_set_control(&e, 0, 0);
    CHECK(e.tag_polyphony[tag_index(&e, "Tape")] == 1);
    ds_native_engine_destroy(&e);

    load(&e, dir, "oldest.dspreset",
         "<DecentSampler><groups attack=\"0\" release=\"1.0\">"
         "<group tags=\"Tape\"><sample path=\"t.wav\" rootNote=\"55\"/></group></groups>"
         "<tags><tag name=\"Tape\" polyphony=\"2\"/></tags></DecentSampler>");
    ds_native_engine_note_on(&e, 60, 127); blocks(&e, 1);
    ds_native_engine_note_on(&e, 62, 127); blocks(&e, 1);
    ds_native_engine_note_on(&e, 64, 127);
    CHECK(voice_of(&e, 60)->choked && !voice_of(&e, 62)->choked && !voice_of(&e, 64)->choked);
    ds_native_engine_destroy(&e);

    puts("choke test passed");
    return 0;
}
