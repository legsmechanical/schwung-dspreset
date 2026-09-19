/* What a note knows about the notes before it: trigger first / legato /
 * continuous, previousNotes (numbers or names: C3 = 60) and legatoInterval;
 * glide (constant time, geometric in pitch; legato / always / off; a knob on
 * GLIDE_TIME); releaseTriggerDecay (dB per second held, or linear gain lost
 * per second); and keyswitches, <midi><note> — bindings before the note
 * plays, ranges, note_off listeners, swallowNotes (also through the real
 * plugin), and a listener switched off by a binding. */
#include "test_support.h"

#include "../src/dsp/dspreset/native_engine.h"

#define SR 44100

static float sig(uint64_t f) { return test_signal24(f, 0) / 8388607.0f; }

static void load(ds_native_engine_t *e, const char *dir, const char *xml) {
    char path[512], error[128];
    snprintf(path, sizeof(path), "%s/nc/p.dspreset", dir);
    write_text(path, xml);
    CHECK(ds_native_engine_load(e, path, SR, NULL, NULL, error, sizeof(error)) == 0);
    for (int i = 0; i < 4; ++i) e->amp_override[i] = -1;
}

static void blocks(ds_native_engine_t *e, int n) {
    float out[256];
    while (n--) { memset(out, 0, sizeof(out)); ds_native_engine_render(e, out, 128); }
}

/* Which files note `note` started (live, not silenced), as a string "a,d". */
static const char *played(ds_native_engine_t *e, int note) {
    static char list[64];
    list[0] = '\0';
    for (char f = 'a'; f <= 'f'; ++f)
        for (int i = 0; i < DS_MAX_VOICES; ++i) {
            ds_voice_t *v = &e->voices[i];
            if (v->active && !v->choked && v->note == note && v->zone->def.path[0] == f) {
                size_t n = strlen(list);
                snprintf(list + n, sizeof(list) - n, "%s%c", n ? "," : "", f);
                break;
            }
        }
    return list;
}

static ds_voice_t *voice_of(ds_native_engine_t *e, int note) {
    for (int i = 0; i < DS_MAX_VOICES; ++i) if (e->voices[i].active && e->voices[i].note == note) return &e->voices[i];
    CHECK(0);
    return NULL;
}

/* One frame's advance of `v`. */
static double step(ds_native_engine_t *e, ds_voice_t *v) {
    float out[2] = {0, 0};
    double before = v->pos;
    ds_native_engine_render(e, out, 1);
    return v->pos - before;
}

static void settle(plugin_t *p) {
    for (int i = 0; i < 500; ++i) { usleep(10000); if (i > 20 && !plugin_uint(p, "is_loading")) break; }
}

int main(void) {
    const char *dir = getenv("TEST_TMP");
    char path[1024];
    static ds_native_engine_t e;
    float out[256];
    CHECK(dir);
    snprintf(path, sizeof(path), "mkdir -p '%s/nc'", dir); CHECK(system(path) == 0);
    for (const char *f = "abcdef"; *f; ++f) { snprintf(path, sizeof(path), "%s/nc/%c.wav", dir, *f); write_wav24(path, SR, 1, 88200, -1, -1); }

    /* triggers and the previous note */
    load(&e, dir,
         "<DecentSampler><groups attack=\"0\" release=\"0.01\">"
         "<group trigger=\"first\"><sample path=\"a.wav\" rootNote=\"C3\"/></group>"
         "<group trigger=\"legato\"><sample path=\"b.wav\" rootNote=\"60\" previousNote=\"C3\"/></group>"
         "<group trigger=\"legato\"><sample path=\"c.wav\" rootNote=\"60\" previousNotes=\"64,65\"/></group>"
         "<group trigger=\"continuous\"><sample path=\"d.wav\" rootNote=\"60\"/></group>"
         "<group trigger=\"legato\"><sample path=\"e.wav\" rootNote=\"60\"/></group>"
         "<group trigger=\"legato\" legatoInterval=\"-2\"><sample path=\"f.wav\" rootNote=\"60\"/></group>"
         "</groups></DecentSampler>");
    CHECK(e.zones[0].def.root_note == 60 && e.zones[1].def.previous_notes[0] == 60);
    ds_native_engine_note_on(&e, 60, 127);
    CHECK(!strcmp(played(&e, 60), "a,d"));                    /* nothing held: first + continuous, no legato */
    ds_native_engine_note_on(&e, 62, 127);
    CHECK(!strcmp(played(&e, 62), "b,d,e"));                  /* after C3: b; 62-60 is +2, not -2 */
    ds_native_engine_note_off(&e, 60); ds_native_engine_note_off(&e, 62); blocks(&e, 10);
    ds_native_engine_note_on(&e, 64, 127);
    CHECK(!strcmp(played(&e, 64), "a,d"));                    /* keys all up: first again */
    ds_native_engine_note_on(&e, 62, 127);
    CHECK(!strcmp(played(&e, 62), "c,d,e,f"));                /* after 64: c; 62-64 = -2: f */
    ds_native_engine_destroy(&e);

    /* glide: 60 held, then 67, over 0.1 s */
    load(&e, dir,
         "<DecentSampler><ui><tab><labeled-knob minValue=\"0\" maxValue=\"1\" value=\"0.1\">"
         "<binding type=\"amp\" level=\"instrument\" parameter=\"GLIDE_TIME\"/></labeled-knob>"
         "<button value=\"0\"><state name=\"Legato\"><binding type=\"amp\" level=\"instrument\" parameter=\"GLIDE_MODE\" translation=\"fixed_value\" translationValue=\"legato\"/></state>"
         "<state name=\"Always\"><binding type=\"amp\" level=\"instrument\" parameter=\"GLIDE_MODE\" translation=\"fixed_value\" translationValue=\"always\"/></state>"
         "<state name=\"Off\"><binding type=\"amp\" level=\"instrument\" parameter=\"GLIDE_MODE\" translation=\"fixed_value\" translationValue=\"off\"/></state></button>"
         "</tab></ui><groups attack=\"0\" release=\"0.01\"><group><sample path=\"a.wav\" rootNote=\"60\"/></group></groups></DecentSampler>");
    ds_native_engine_note_on(&e, 60, 127); blocks(&e, 2);
    ds_native_engine_note_on(&e, 67, 127);
    {
        ds_voice_t *v = voice_of(&e, 67);
        /* the file's root is 60: the glide starts at a step of 1 and ends at 67's 2^(7/12) */
        double r = pow(2.0, 7.0 / 12 / 4410), expect_total, total = 0, d;
        CHECK(v->glide_left == 4410);
        d = step(&e, v); total += d;
        CHECK(fabs(d - 1.0) < 1e-9);                           /* starts at 60's pitch */
        for (int f = 1; f < 2205; ++f) total += step(&e, v);
        d = step(&e, v); total += d;
        CHECK(fabs(d - pow(2.0, 3.5 / 12)) < 1e-9);           /* halfway in time, halfway in pitch */
        for (int f = 2206; f < 4410; ++f) total += step(&e, v);
        CHECK(v->glide_left == 0);
        expect_total = (pow(r, 4410) - 1) / (r - 1);
        printf("  glide 60 -> 67 over 4410 frames: travelled %.6f (want %.6f)\n", total, expect_total);
        CHECK(fabs(total - expect_total) < 1e-6);
        CHECK(fabs(v->inc - pow(2.0, 7.0 / 12)) < 1e-12);
        CHECK(fabs(step(&e, v) - v->inc) < 1e-9 && fabs(step(&e, v) - v->inc) < 1e-9);   /* then 67 (to pos's precision) */
    }
    /* the same glide in whole blocks (the per-frame path): where it has got to
     * after 40 blocks = the glide's travel, then 67's pitch for the rest */
    ds_native_engine_note_off(&e, 60); ds_native_engine_note_off(&e, 67); blocks(&e, 10);
    ds_native_engine_note_on(&e, 60, 127); blocks(&e, 2);      /* nothing held: no glide into it */
    ds_native_engine_note_on(&e, 67, 127);
    {
        ds_voice_t *v = voice_of(&e, 67);
        double r = pow(2.0, 7.0 / 12 / 4410), want;
        CHECK(v->glide_left == 4410);
        for (int b = 0; b < 40; ++b) {
            blocks(&e, 1);
            if (b == 16) {                                     /* 2176 frames in */
                want = (pow(r, 2176) - 1) / (r - 1);
                CHECK(fabs(v->pos - want) < 1e-6);
            }
        }
        want = (pow(r, 4410) - 1) / (r - 1) + (40 * 128 - 4410) * pow(2.0, 7.0 / 12);
        printf("  the same in blocks, after 40: at %.6f (want %.6f)\n", v->pos, want);
        CHECK(fabs(v->pos - want) < 1e-6);
    }
    ds_native_engine_note_off(&e, 60); ds_native_engine_note_off(&e, 67); blocks(&e, 10);
    ds_native_engine_note_on(&e, 64, 127);                     /* legato mode, nothing held: no glide */
    CHECK(voice_of(&e, 64)->glide_left == 0);
    ds_native_engine_note_off(&e, 64); blocks(&e, 10);
    ds_native_engine_set_control(&e, 1, 1);                    /* always: glides from the released 64 */
    ds_native_engine_note_on(&e, 60, 127);
    CHECK(voice_of(&e, 60)->glide_left == 4410);
    ds_native_engine_set_control(&e, 1, 2);                    /* off */
    ds_native_engine_note_on(&e, 62, 127);
    CHECK(voice_of(&e, 62)->glide_left == 0);
    ds_native_engine_set_control(&e, 1, 1);
    ds_native_engine_set_control(&e, 0, 0);                    /* always, but no time */
    ds_native_engine_note_on(&e, 65, 127);
    CHECK(voice_of(&e, 65)->glide_left == 0);
    ds_native_engine_destroy(&e);

    /* releaseTriggerDecay: held 344 blocks (0.9985 s) */
    for (int db = 0; db < 2; ++db) {
        char xml[1024];
        float held = 344 * 128.0f / SR, want = db ? powf(10.0f, -6 * held / 20) : 1 - 0.3f * held;
        snprintf(xml, sizeof(xml),
                 "<DecentSampler><groups><group volume=\"0\"><sample path=\"a.wav\" rootNote=\"60\"/></group>"
                 "<group trigger=\"release\" releaseTriggerDecay=\"%s\"><sample path=\"b.wav\" rootNote=\"60\" ampEnvEnabled=\"false\"/></group>"
                 "</groups></DecentSampler>", db ? "6dB" : "0.3");
        load(&e, dir, xml);
        blocks(&e, 50);                                        /* the key goes down well after the load */
        ds_native_engine_note_on(&e, 60, 127); blocks(&e, 344);
        ds_native_engine_note_off(&e, 60);
        memset(out, 0, sizeof(out));
        ds_native_engine_render(&e, out, 128);
        printf("  release trigger after %.4f s held, %s: gain %.4f (want %.4f)\n", held, db ? "6dB" : "0.3", out[2 * 7] / sig(7), want);
        for (int i = 0; i < 128; ++i) CHECK(fabsf(out[2 * i] - want * sig(i)) < 1e-5f);
        ds_native_engine_destroy(&e);
    }
    /* none set: full level */
    load(&e, dir, "<DecentSampler><groups><group trigger=\"release\"><sample path=\"b.wav\" rootNote=\"60\" ampEnvEnabled=\"false\"/></group></groups></DecentSampler>");
    ds_native_engine_note_on(&e, 60, 127); blocks(&e, 100);
    ds_native_engine_note_off(&e, 60);
    memset(out, 0, sizeof(out));
    ds_native_engine_render(&e, out, 128);
    CHECK(fabsf(out[2 * 7] - sig(7)) < 1e-5f);
    ds_native_engine_destroy(&e);

    /* keyswitches */
    {
        static const char *PRESET =
            "<DecentSampler><ui><tab><button value=\"0\"><state name=\"On\"/><state name=\"Off\">"
            "<binding type=\"note\" level=\"midi\" midiElementIndex=\"3\" parameter=\"ENABLED\" translation=\"fixed_value\" translationValue=\"false\"/>"
            "</state></button></tab></ui>"
            "<groups><group volume=\"1\"><sample path=\"a.wav\" rootNote=\"60\" ampEnvEnabled=\"false\"/></group>"
            "<group volume=\"0.5\" enabled=\"false\"><sample path=\"b.wav\" rootNote=\"60\" ampEnvEnabled=\"false\"/></group></groups>"
            "<midi>"
            "<note note=\"11\" swallowNotes=\"true\">"
            "<binding type=\"general\" level=\"group\" position=\"0\" parameter=\"ENABLED\" translation=\"fixed_value\" translationValue=\"true\"/>"
            "<binding type=\"general\" level=\"group\" position=\"1\" parameter=\"ENABLED\" translation=\"fixed_value\" translationValue=\"false\"/></note>"
            "<note note=\"12\" enabled=\"true\" eventType=\"note_on\" swallowNotes=\"true\">"
            "<binding type=\"general\" level=\"group\" position=\"0\" parameter=\"ENABLED\" translation=\"fixed_value\" translationValue=\"false\"/>"
            "<binding type=\"general\" level=\"group\" position=\"1\" parameter=\"ENABLED\" translation=\"fixed_value\" translationValue=\"true\"/></note>"
            "<cc number=\"1\"/>"
            "<note note=\"24-35\"><binding type=\"amp\" level=\"instrument\" parameter=\"AMP_VOLUME\" translation=\"fixed_value\" translationValue=\"0.25\"/></note>"
            "<note note=\"40\" eventType=\"note_off\"><binding type=\"amp\" level=\"instrument\" parameter=\"AMP_VOLUME\" translation=\"fixed_value\" translationValue=\"0.75\"/></note>"
            "</midi></DecentSampler>";
        char mod[512];
        plugin_t p;
        load(&e, dir, PRESET);
        ds_native_engine_note_on(&e, 12, 100);                 /* switch to group 1, and stay silent */
        CHECK(ds_native_engine_active_voices(&e) == 0 && !e.groups_rt[0].enabled && e.groups_rt[1].enabled);
        ds_native_engine_note_off(&e, 12);
        ds_native_engine_note_on(&e, 60, 127);
        memset(out, 0, sizeof(out)); ds_native_engine_render(&e, out, 128);
        CHECK(fabsf(out[2 * 9] - 0.5f * sig(9)) < 1e-5f);      /* group 1's level */
        ds_native_engine_cc(&e, 120, 0);
        ds_native_engine_note_on(&e, 11, 100); ds_native_engine_note_off(&e, 11);
        CHECK(e.groups_rt[0].enabled && !e.groups_rt[1].enabled);
        /* a range, both edges; the notes in it still play (not swallowed) */
        ds_native_engine_note_on(&e, 23, 100); CHECK(e.instrument_rt.volume == 1.0f);
        ds_native_engine_note_on(&e, 36, 100); CHECK(e.instrument_rt.volume == 1.0f);
        ds_native_engine_note_on(&e, 24, 100); CHECK(e.instrument_rt.volume == 0.25f);
        CHECK(ds_native_engine_active_voices(&e) == 3);
        e.instrument_rt.volume = 1.0f;
        ds_native_engine_note_on(&e, 35, 100); CHECK(e.instrument_rt.volume == 0.25f);
        /* a note_off listener: nothing at the press, the binding at the release */
        e.instrument_rt.volume = 1.0f;
        ds_native_engine_note_on(&e, 40, 100); CHECK(e.instrument_rt.volume == 1.0f);
        ds_native_engine_note_off(&e, 40); CHECK(e.instrument_rt.volume == 0.75f);
        /* the button switches the 24-35 listener (the <midi> element at index 3) off */
        e.instrument_rt.volume = 1.0f;
        ds_native_engine_set_control(&e, 0, 1);
        ds_native_engine_note_on(&e, 30, 100); CHECK(e.instrument_rt.volume == 1.0f);
        ds_native_engine_destroy(&e);

        /* through the plugin: a swallowed key sounds nothing */
        snprintf(mod, sizeof(mod), "%s/nc-module", dir);
        snprintf(path, sizeof(path), "rm -rf '%s' && mkdir -p '%s/instruments/K'", mod, mod); CHECK(system(path) == 0);
        for (const char *f = "ab"; *f; ++f) { snprintf(path, sizeof(path), "%s/instruments/K/%c.wav", mod, *f); write_wav24(path, SR, 1, 88200, -1, -1); }
        snprintf(path, sizeof(path), "%s/instruments/K/1 K.dspreset", mod); write_text(path, PRESET);
        plugin_open_in(&p, mod);
        usleep(600000);
        settle(&p);
        {
            int16_t pcm[256];
            plugin_midi(&p, 0x90, 11, 100); plugin_render(&p, pcm);
            CHECK(plugin_uint(&p, "voices") == 0);
            plugin_midi(&p, 0x80, 11, 0);
            plugin_midi(&p, 0x90, 60, 100); plugin_render(&p, pcm);
            CHECK(plugin_uint(&p, "voices") == 1);
        }
        plugin_close(&p);
    }

    puts("note context test passed");
    return 0;
}
