#include "dspreset_parser.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define MAX_PRESET_BYTES (16 * 1024 * 1024)

/* DecentSampler leaves these undocumented. Release is the one audible guess:
 * the old Multisampler found a near-zero release cut pianos off, and chose
 * 0.5 s; a preset that sets its own release (most do) is unaffected. */
#define DEFAULT_ATTACK  0.0f
#define DEFAULT_DECAY   0.0f
#define DEFAULT_SUSTAIN 1.0f
#define DEFAULT_RELEASE 0.5f

static void fail(char *out, unsigned n, const char *message) {
    if (n) snprintf(out, n, "%s", message);
}

static int is_name_char(char c) {
    return isalnum((unsigned char)c) || c == '_' || c == '-' || c == ':' || c == '.';
}

int ds_xml_attribute(const char *attrs, const char *end, const char *name,
                     char *out, unsigned out_size) {
    size_t name_len = strlen(name);
    const char *p = attrs;
    while (p < end) {
        const char *key, *value;
        size_t key_len;
        char quote;
        unsigned n = 0;
        while (p < end && !is_name_char(*p)) ++p;
        key = p;
        while (p < end && is_name_char(*p)) ++p;
        key_len = (size_t)(p - key);
        while (p < end && isspace((unsigned char)*p)) ++p;
        if (p >= end || *p != '=') continue;
        ++p;
        while (p < end && isspace((unsigned char)*p)) ++p;
        if (p >= end || (*p != '"' && *p != '\'')) continue;
        quote = *p++;
        value = p;
        while (p < end && *p != quote) ++p;
        if (p >= end) return 0;
        if (key_len == name_len && !strncmp(key, name, name_len)) {
            for (const char *v = value; v < p; ++v) {
                char c = *v;
                if (c == '&') {
                    static const struct { const char *entity; char c; } entities[] = {
                        {"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'}, {"&quot;", '"'}, {"&apos;", '\''}};
                    for (unsigned e = 0; e < sizeof(entities) / sizeof(entities[0]); ++e) {
                        size_t len = strlen(entities[e].entity);
                        if ((size_t)(p - v) >= len && !strncmp(v, entities[e].entity, len)) {
                            c = entities[e].c; v += len - 1; break;
                        }
                    }
                }
                if (n + 1 >= out_size) return 0;
                out[n++] = c;
            }
            out[n] = '\0';
            return 1;
        }
        ++p;
    }
    return 0;
}

/* One open element's attribute span. Scopes are consulted innermost first. */
typedef struct { const char *attrs, *end; } scope_t;

static int lookup(const scope_t *scopes, int depth, const char *name, char *out, unsigned size) {
    for (int i = depth - 1; i >= 0; --i)
        if (scopes[i].attrs && ds_xml_attribute(scopes[i].attrs, scopes[i].end, name, out, size)) return 1;
    return 0;
}

static int parse_double(const char *text, double *out) {
    char *tail;
    double v = strtod(text, &tail);
    if (tail == text) return 0;
    *out = v;
    return 1;
}

static double number(const scope_t *s, int depth, const char *name, double fallback) {
    char text[64]; double v;
    return lookup(s, depth, name, text, sizeof(text)) && parse_double(text, &v) ? v : fallback;
}

int ds_note_number(const char *text) {
    static const int pitch[7] = {9, 11, 0, 2, 4, 5, 7};          /* A B C D E F G */
    char *tail;
    double v;
    int pc, octave;
    while (isspace((unsigned char)*text)) ++text;
    v = strtod(text, &tail);
    if (tail != text) return (int)v;
    if (toupper((unsigned char)*text) < 'A' || toupper((unsigned char)*text) > 'G') return DS_NO_NOTE;
    pc = pitch[toupper((unsigned char)*text) - 'A'];
    ++text;
    if (*text == '#') { pc++; ++text; } else if (*text == 'b') { pc--; ++text; }
    octave = (int)strtol(text, &tail, 10);
    if (tail == text) return DS_NO_NOTE;
    return (octave + 2) * 12 + pc;                                  /* C3 = 60 */
}

/* A note attribute: a number or a note name. */
static int note_attr(const scope_t *s, int depth, const char *name, int fallback) {
    char text[32];
    int n;
    return lookup(s, depth, name, text, sizeof(text)) && (n = ds_note_number(text)) != DS_NO_NOTE ? n : fallback;
}

static int boolean(const scope_t *s, int depth, const char *name, int fallback) {
    char text[16];
    if (!lookup(s, depth, name, text, sizeof(text))) return fallback;
    if (!strcasecmp(text, "true") || !strcmp(text, "1")) return 1;
    if (!strcasecmp(text, "false") || !strcmp(text, "0")) return 0;
    return fallback;
}

/* "0.5", "-6dB", "3 dB" -> linear gain, read at ONE level: volumes multiply
 * across <groups>/<group>/<sample> rather than override. */
static int volume_of(const scope_t *scope, float *gain) {
    char text[64]; double v; char *tail;
    if (!scope->attrs || !ds_xml_attribute(scope->attrs, scope->end, "volume", text, sizeof(text))) return 0;
    v = strtod(text, &tail);
    if (tail == text) return 0;
    while (isspace((unsigned char)*tail)) ++tail;
    *gain = (!strncasecmp(tail, "db", 2)) ? (float)pow(10.0, v / 20.0) : (float)v;
    return 1;
}

static double own_number(const scope_t *scope, const char *name, double fallback) {
    char text[64]; double v;
    return scope->attrs && ds_xml_attribute(scope->attrs, scope->end, name, text, sizeof(text)) &&
           parse_double(text, &v) ? v : fallback;
}

static void strip_comments(char *xml) {
    char *read = xml, *write = xml;
    while (*read) {
        if (!strncmp(read, "<!--", 4)) {
            char *close = strstr(read + 4, "-->");
            if (!close) break;
            read = close + 3;
            continue;
        }
        *write++ = *read++;
    }
    *write = '\0';
}

static int tag_is(const char *tag, const char *name) {
    size_t len = strlen(name);
    return !strncmp(tag, name, len) && !is_name_char(tag[len]);
}

static int build_sample(const scope_t *s, int depth, int group_index, ds_dspreset_sample_t *out) {
    char text[512], mode[32];
    float g;
    memset(out, 0, sizeof(*out));
    if (!lookup(s, depth, "path", text, sizeof(text)) || !text[0]) return -1;
    for (char *c = text; *c; ++c) if (*c == '\\') *c = '/';
    snprintf(out->path, sizeof(out->path), "%s", text);
    out->root_note = note_attr(s, depth, "rootNote", 60);
    out->lo_note = note_attr(s, depth, "loNote", 0);
    out->hi_note = note_attr(s, depth, "hiNote", 127);
    out->lo_vel = (int)number(s, depth, "loVel", 0);
    out->hi_vel = (int)number(s, depth, "hiVel", 127);
    out->seq_position = (int)number(s, depth, "seqPosition", 1);
    out->seq_mode = DS_SEQ_ALWAYS;
    if (lookup(s, depth, "seqMode", mode, sizeof(mode))) {
        if (!strcmp(mode, "round_robin")) out->seq_mode = DS_SEQ_ROUND_ROBIN;
        else if (!strcmp(mode, "random") || !strcmp(mode, "true_random")) out->seq_mode = DS_SEQ_RANDOM;
    }
    out->trigger = DS_TRIGGER_ATTACK;
    if (lookup(s, depth, "trigger", mode, sizeof(mode))) {
        if (!strcasecmp(mode, "release")) out->trigger = DS_TRIGGER_RELEASE;
        else if (!strcasecmp(mode, "first")) out->trigger = DS_TRIGGER_FIRST;
        else if (!strcasecmp(mode, "legato")) out->trigger = DS_TRIGGER_LEGATO;
        else if (!strcasecmp(mode, "continuous")) out->trigger = DS_TRIGGER_CONTINUOUS;
    }
    {   /* previousNotes (the true-legato guide writes previousNote, with names) */
        char list[256];
        out->previous_count = 0;
        if (lookup(s, depth, "previousNotes", list, sizeof(list)) || lookup(s, depth, "previousNote", list, sizeof(list))) {
            char *q = list;
            while (*q && out->previous_count < DS_MAX_PREVIOUS) {
                char *comma = strchr(q, ',');
                int n;
                if (comma) *comma = '\0';
                if ((n = ds_note_number(q)) >= 0 && n <= 127) out->previous_notes[out->previous_count++] = n;
                if (!comma) break;
                q = comma + 1;
            }
        }
    }
    out->legato_interval = lookup(s, depth, "legatoInterval", text, sizeof(text)) ? (int)strtol(text, NULL, 10) : DS_NO_INTERVAL;
    out->glide_time = (float)number(s, depth, "glideTime", 0);
    out->glide_mode = DS_GLIDE_LEGATO;
    if (lookup(s, depth, "glideMode", mode, sizeof(mode)))
        out->glide_mode = !strcasecmp(mode, "always") ? DS_GLIDE_ALWAYS : !strcasecmp(mode, "off") ? DS_GLIDE_OFF : DS_GLIDE_LEGATO;
    out->release_decay = 0;
    out->release_decay_db = 0;
    if (lookup(s, depth, "releaseTriggerDecay", text, sizeof(text))) {
        char *tail;
        double v = strtod(text, &tail);
        while (isspace((unsigned char)*tail)) ++tail;
        out->release_decay_db = !strncasecmp(tail, "db", 2);
        out->release_decay = (float)fabs(v);                /* a positive dB value means a decay too */
    }
    out->group_index = group_index;
    out->tuning = number(s, depth, "tuning", 0);
    for (int i = 0; i < depth; ++i) out->tuning += own_number(&s[i], "groupTuning", 0);
    out->gain = 1.0f;
    for (int i = 0; i < depth; ++i) if (volume_of(&s[i], &g)) out->gain *= g;
    out->pan = (float)(number(s, depth, "pan", 0) / 100.0);
    if (out->pan < -1) out->pan = -1;
    if (out->pan > 1) out->pan = 1;
    out->amp_vel_track = (float)number(s, depth, "ampVelTrack", 1.0);
    out->amp_env_enabled = boolean(s, depth, "ampEnvEnabled", 1);
    out->attack = (float)number(s, depth, "attack", DEFAULT_ATTACK);
    out->decay = (float)number(s, depth, "decay", DEFAULT_DECAY);
    out->sustain = (float)number(s, depth, "sustain", DEFAULT_SUSTAIN);
    out->release = (float)number(s, depth, "release", DEFAULT_RELEASE);
    out->start = (int64_t)number(s, depth, "start", -1);
    out->end = (int64_t)number(s, depth, "end", -1);
    out->loop_enabled = boolean(s, depth, "loopEnabled", -1);
    out->loop_start = (int64_t)number(s, depth, "loopStart", -1);
    out->loop_end = (int64_t)number(s, depth, "loopEnd", -1);
    out->playback_mode = DS_PLAYBACK_AUTO;
    if (lookup(s, depth, "playbackMode", mode, sizeof(mode))) {
        if (!strcmp(mode, "memory")) out->playback_mode = DS_PLAYBACK_MEMORY;
        else if (!strcmp(mode, "disk_streaming")) out->playback_mode = DS_PLAYBACK_DISK_STREAMING;
    }
    out->pitch_key_track = (float)number(s, depth, "pitchKeyTrack", 1.0);
    out->loop_crossfade = (int64_t)number(s, depth, "loopCrossfade", 0);
    if (out->loop_crossfade < 0) out->loop_crossfade = 0;
    out->loop_crossfade_equal_power = !(lookup(s, depth, "loopCrossfadeMode", mode, sizeof(mode)) && !strcasecmp(mode, "linear"));
    if (!lookup(s, depth, "silencedByTags", out->silenced_by, sizeof(out->silenced_by))) out->silenced_by[0] = '\0';
    out->silencing_mode = lookup(s, depth, "silencingMode", mode, sizeof(mode)) && !strcasecmp(mode, "normal")
                          ? DS_SILENCE_NORMAL : DS_SILENCE_FAST;
    out->silencing_decay = (float)number(s, depth, "silencingDecay", 0);
    {
        const scope_t *own = &s[depth - 1];
        static const struct { const char *name; unsigned bit; } owned[] = {
            {"pan", DS_OWN_PAN}, {"ampVelTrack", DS_OWN_VEL_TRACK}, {"attack", DS_OWN_ATTACK},
            {"decay", DS_OWN_DECAY}, {"sustain", DS_OWN_SUSTAIN}, {"release", DS_OWN_RELEASE},
            {"pitchKeyTrack", DS_OWN_KEY_TRACK}, {"silencingMode", DS_OWN_SILENCING_MODE},
            {"silencingDecay", DS_OWN_SILENCING_DECAY}, {"glideTime", DS_OWN_GLIDE_TIME}, {"glideMode", DS_OWN_GLIDE_MODE}};
        char tags[256], text2[64];
        for (unsigned i = 0; i < sizeof(owned) / sizeof(owned[0]); ++i)
            if (ds_xml_attribute(own->attrs, own->end, owned[i].name, text2, sizeof(text2))) out->own_mask |= owned[i].bit;
        out->own_volume = 1.0f;
        volume_of(own, &out->own_volume);
        out->base_tuning = number(s, depth, "tuning", 0);
        out->tags[0] = '\0';
        for (int i = 0; i < depth; ++i)
            if (s[i].attrs && ds_xml_attribute(s[i].attrs, s[i].end, "tags", tags, sizeof(tags)) && tags[0]) {
                size_t len = strlen(out->tags);
                if (len + strlen(tags) + 2 <= sizeof(out->tags))   /* a tag list that would not fit is dropped whole */
                    snprintf(out->tags + len, sizeof(out->tags) - len, "%s%.*s", len ? "," : "", (int)(sizeof(out->tags) - len - 2), tags);
            }
    }
    if (out->lo_note < 0) out->lo_note = 0;
    if (out->hi_note > 127) out->hi_note = 127;
    if (out->lo_vel < 0) out->lo_vel = 0;
    if (out->hi_vel > 127) out->hi_vel = 127;
    if (out->lo_note > out->hi_note || out->lo_vel > out->hi_vel) return -1;
    return 0;
}

int ds_dspreset_visit_samples(const char *preset_path,
                              ds_dspreset_sample_visitor_t visitor, void *context,
                              char *error, unsigned error_len) {
    FILE *file;
    long length;
    char *xml, *p;
    scope_t scopes[3];  /* <groups>, <group>, <sample> */
    int in_groups = 0, in_group = 0, group_index = -1;
    unsigned visited = 0, rejected = 0;
    int rc = -1;
    if (!preset_path || !visitor) return -1;
    file = fopen(preset_path, "rb");
    if (!file || fseek(file, 0, SEEK_END) || (length = ftell(file)) < 0 ||
        length > MAX_PRESET_BYTES || fseek(file, 0, SEEK_SET)) {
        fail(error, error_len, "cannot read DSPreset");
        if (file) fclose(file);
        return -1;
    }
    xml = malloc((size_t)length + 1);
    if (!xml || fread(xml, 1, (size_t)length, file) != (size_t)length) {
        fail(error, error_len, "cannot read DSPreset");
        free(xml); fclose(file); return -1;
    }
    fclose(file);
    xml[length] = '\0';
    strip_comments(xml);
    memset(scopes, 0, sizeof(scopes));

    for (p = xml; (p = strchr(p, '<')) != NULL; ) {
        char *tag = p + 1, *end = strchr(tag, '>');
        int closing = 0, self_closing;
        if (!end) break;
        p = end + 1;
        if (*tag == '?' || *tag == '!') continue;
        if (*tag == '/') { closing = 1; ++tag; }
        self_closing = end > tag && end[-1] == '/';
        if (tag_is(tag, "groups")) {
            if (closing) { in_groups = 0; scopes[0].attrs = NULL; }
            else if (!self_closing) { in_groups = 1; scopes[0] = (scope_t){tag + 6, end}; }
        } else if (tag_is(tag, "group")) {
            /* Numbered in document order, empty self-closing groups included:
             * bindings address groups by this index. */
            if (closing) { in_group = 0; scopes[1].attrs = NULL; }
            else if (self_closing) group_index++;
            else { in_group = 1; group_index++; scopes[1] = (scope_t){tag + 5, end}; }
        } else if (!closing && tag_is(tag, "sample") && in_groups && in_group) {
            /* A disabled group still loads: a layer button or menu may switch
             * it on. Whether it SOUNDS is the preset model's runtime state. */
            ds_dspreset_sample_t sample;
            scopes[2] = (scope_t){tag + 6, end};
            if (build_sample(scopes, 3, group_index, &sample)) { rejected++; continue; }
            if (visitor(&sample, context)) { rc = 0; goto done; }
            visited++;
        }
    }
    if (!visited) {
        fail(error, error_len, rejected ? "DSPreset samples are all malformed" : "DSPreset contains no samples");
        goto done;
    }
    rc = 0;
done:
    free(xml);
    return rc;
}
