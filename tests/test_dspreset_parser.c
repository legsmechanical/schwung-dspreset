/* The parser against XML written the ways real libraries write it. */
#include "test_support.h"

#include "../src/dsp/dspreset/dspreset_parser.h"

#define MAX_SEEN 16
typedef struct { ds_dspreset_sample_t s[MAX_SEEN]; int n; } seen_t;

static int keep(const ds_dspreset_sample_t *sample, void *opaque) {
    seen_t *seen = opaque;
    CHECK(seen->n < MAX_SEEN);
    seen->s[seen->n++] = *sample;
    return 0;
}

static int near(double a, double b) { return fabs(a - b) < 1e-4; }

int main(void) {
    const char *dir = getenv("TEST_TMP");
    char path[512], error[128] = {0};
    char attr[64];
    seen_t seen = {0};
    const char *tag = " path = \"a &amp; b.wav\"  rootNote='60'/";
    CHECK(dir);

    /* attribute scanning: spaces around '=', single quotes, entities, and a
     * name that is a suffix of another must not match it */
    CHECK(ds_xml_attribute(tag, tag + strlen(tag), "path", attr, sizeof(attr)) && !strcmp(attr, "a & b.wav"));
    CHECK(ds_xml_attribute(tag, tag + strlen(tag), "rootNote", attr, sizeof(attr)) && !strcmp(attr, "60"));
    CHECK(!ds_xml_attribute(tag, tag + strlen(tag), "Note", attr, sizeof(attr)));

    snprintf(path, sizeof(path), "%s/parser.dspreset", dir);
    write_text(path,
        "<?xml version=\"1.0\"?>\n<DecentSampler>\n"
        "<!-- <sample path=\"commented.wav\"/> must be ignored -->\n"
        "<ui><tab><labeled-knob><binding path=\"not-a-sample\"/></labeled-knob></tab></ui>\n"
        "<groups volume=\"-6dB\" release = \"0.25\" seqMode=\"round_robin\" start=\"100\">\n"
        "  <group volume=\"0.5\" groupTuning=\"-2\" attack=\"0.01\" pan=\"-50\">\n"
        "    <sample path = \"Samples\\low.wav\" rootNote = \"33\" loNote=\"30\" hiNote=\"35\" seqPosition=\"2\" tuning=\"0.5\"/>\n"
        "    <sample path=\"Samples/own.wav\" rootNote=\"40\" release=\"1.5\" volume=\"2\" loopEnabled=\"true\" loopStart=\"10\" loopEnd=\"20\"/>\n"
        "  </group>\n"
        "  <group enabled=\"false\"><sample path=\"Samples/off.wav\" rootNote=\"50\"/></group>\n"
        "  <group trigger=\"release\" ampVelTrack=\"0\"><sample path=\"Samples/rel.wav\" rootNote=\"60\" playbackMode=\"memory\"/></group>\n"
        "</groups>\n<effects><effect type=\"lowpass\"/></effects>\n</DecentSampler>\n");
    CHECK(ds_dspreset_visit_samples(path, keep, &seen, error, sizeof(error)) == 0);
    CHECK(seen.n == 3);

    /* inherited from <groups> and <group>, backslashes normalised */
    CHECK(!strcmp(seen.s[0].path, "Samples/low.wav"));
    CHECK(seen.s[0].root_note == 33 && seen.s[0].lo_note == 30 && seen.s[0].hi_note == 35);
    CHECK(seen.s[0].seq_mode == DS_SEQ_ROUND_ROBIN && seen.s[0].seq_position == 2);
    CHECK(near(seen.s[0].tuning, -1.5));                  /* 0.5 + groupTuning -2 */
    CHECK(near(seen.s[0].gain, 0.5 * pow(10, -6 / 20.0))); /* volumes multiply */
    CHECK(near(seen.s[0].release, 0.25) && near(seen.s[0].attack, 0.01));
    CHECK(near(seen.s[0].pan, -0.5) && seen.s[0].start == 100);
    CHECK(seen.s[0].loop_enabled == -1);

    /* the sample's own value beats the group's */
    CHECK(near(seen.s[1].release, 1.5) && near(seen.s[1].gain, 2 * 0.5 * pow(10, -6 / 20.0)));
    CHECK(seen.s[1].lo_note == 0 && seen.s[1].hi_note == 127);   /* guide defaults */
    CHECK(seen.s[1].loop_enabled == 1 && seen.s[1].loop_start == 10 && seen.s[1].loop_end == 20);

    /* a disabled group contributes nothing; release triggers are marked */
    CHECK(!strcmp(seen.s[2].path, "Samples/rel.wav"));
    CHECK(seen.s[2].trigger == DS_TRIGGER_RELEASE && near(seen.s[2].amp_vel_track, 0));
    CHECK(seen.s[2].playback_mode == DS_PLAYBACK_MEMORY && seen.s[2].group_index == 2);

    /* a preset with no samples reports it */
    snprintf(path, sizeof(path), "%s/empty.dspreset", dir);
    write_text(path, "<DecentSampler><groups><group></group></groups></DecentSampler>");
    CHECK(ds_dspreset_visit_samples(path, keep, &seen, error, sizeof(error)) != 0);
    CHECK(strstr(error, "no samples"));
    puts("parser test passed");
    return 0;
}
