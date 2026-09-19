#include "preset_model.h"

#include "dspreset_parser.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define MAX_PRESET_BYTES (16 * 1024 * 1024)
#define MAX_BINDINGS 2048
#define MAX_CHOICES 512
#define MAX_CCS 128
#define MAX_GROUPS 4096

static void fail(char *out, unsigned n, const char *message) { if (n) snprintf(out, n, "%s", message); }

static int attr(const char *a, const char *e, const char *name, char *out, unsigned n) {
    return ds_xml_attribute(a, e, name, out, n);
}

static int attr_num(const char *a, const char *e, const char *name, float *out) {
    char text[64]; char *tail;
    double v;
    if (!attr(a, e, name, text, sizeof(text))) return 0;
    v = strtod(text, &tail);
    if (tail == text) return 0;
    *out = (float)v;
    return 1;
}

/* "0.5", "-6dB" -> linear */
static int attr_volume(const char *a, const char *e, float *out) {
    char text[64]; char *tail;
    double v;
    if (!attr(a, e, "volume", text, sizeof(text))) return 0;
    v = strtod(text, &tail);
    if (tail == text) return 0;
    while (isspace((unsigned char)*tail)) ++tail;
    *out = !strncasecmp(tail, "db", 2) ? (float)pow(10.0, v / 20.0) : (float)v;
    return 1;
}

static int is_name_char(char c) { return isalnum((unsigned char)c) || c == '_' || c == '-'; }
static int tag_is(const char *tag, const char *name) {
    size_t n = strlen(name);
    return !strncmp(tag, name, n) && !is_name_char(tag[n]);
}

uint64_t ds_preset_model_tag_mask(ds_preset_model_t *m, const char *list) {
    uint64_t mask = 0;
    const char *p = list;
    while (p && *p) {
        char name[32];
        unsigned n = 0, i;
        while (*p == ',' || isspace((unsigned char)*p)) ++p;
        while (*p && *p != ',' && n + 1 < sizeof(name)) name[n++] = *p++;
        while (n && isspace((unsigned char)name[n - 1])) --n;
        name[n] = '\0';
        while (*p && *p != ',') ++p;
        if (!n) continue;
        for (i = 0; i < m->tag_count && strcmp(m->tag_names[i], name); ++i) {}
        if (i == m->tag_count) {
            if (m->tag_count == DS_MAX_TAGS) continue;
            snprintf(m->tag_names[m->tag_count++], sizeof(m->tag_names[0]), "%s", name);
        }
        mask |= 1ull << i;
    }
    return mask;
}

float ds_binding_translate(const ds_binding_t *b, float in_min, float in_max, float v) {
    float lo = in_min < in_max ? in_min : in_max, hi = in_min < in_max ? in_max : in_min;
    float t;
    if (b->translation == DS_TRANSLATE_FIXED) return b->fixed;
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    t = hi > lo ? (v - lo) / (hi - lo) : 0.0f;
    if (b->reversed) t = 1.0f - t;
    if (b->translation == DS_TRANSLATE_TABLE && b->table_n >= 2) {
        /* Same reading as the Multisampler converter, which was tuned against
         * real libraries: the knob's position scales the table's key axis. */
        float key = t * b->table_in[b->table_n - 1];
        if (key <= b->table_in[0]) v = b->table_out[0];
        else if (key >= b->table_in[b->table_n - 1]) v = b->table_out[b->table_n - 1];
        else {
            int i = 0;
            while (i < b->table_n - 1 && key > b->table_in[i + 1]) ++i;
            float span = b->table_in[i + 1] - b->table_in[i];
            float f = span != 0 ? (key - b->table_in[i]) / span : 0;
            v = b->table_out[i] + f * (b->table_out[i + 1] - b->table_out[i]);
        }
    } else if (b->has_range) {
        v = b->out_min + t * (b->out_max - b->out_min);
    } else if (b->reversed) {
        v = lo + t * (hi - lo);
    }
    if (b->has_factor) v *= b->factor;
    return v;
}

static int target_for(const char *type, const char *level, const char *param) {
    if (!strcmp(type, "effect")) return DS_TARGET_EFFECT;
    if (!strcmp(type, "note") && !strcmp(param, "ENABLED")) return DS_TARGET_MIDI_ENABLED;
    if (!strcmp(type, "modulator")) return DS_TARGET_MODULATOR;
    if ((!strcmp(param, "VALUE") || !strcmp(param, "X_VALUE") || !strcmp(param, "Y_VALUE")) &&
        (!strcmp(level, "ui") || !strcmp(type, "control"))) return DS_TARGET_CONTROL_VALUE;
    if (!strcmp(type, "control")) {
        /* A control named by its parameterName (mixed case: DS's own tokens
         * are capitals); the other UI properties (text, visibility) have no sound. */
        for (const char *c = param; *c; ++c) if (islower((unsigned char)*c)) return DS_TARGET_CONTROL_VALUE;
        return DS_TARGET_NONE;
    }
    if (!strcmp(param, "AMP_VOLUME") || !strcmp(param, "TAG_VOLUME")) return DS_TARGET_VOLUME;
    if (!strcmp(param, "GLOBAL_TUNING") || !strcmp(param, "GROUP_TUNING") || !strcmp(param, "TUNING")) return DS_TARGET_TUNING;
    if (!strcmp(param, "PAN")) return DS_TARGET_PAN;
    if (!strcmp(param, "AMP_VEL_TRACK")) return DS_TARGET_VEL_TRACK;
    if (!strcmp(param, "ENV_ATTACK")) return DS_TARGET_ATTACK;
    if (!strcmp(param, "ENV_DECAY")) return DS_TARGET_DECAY;
    if (!strcmp(param, "ENV_SUSTAIN")) return DS_TARGET_SUSTAIN;
    if (!strcmp(param, "ENV_RELEASE")) return DS_TARGET_RELEASE;
    if (!strcmp(param, "ENABLED") || !strcmp(param, "TAG_ENABLED")) return DS_TARGET_ENABLED;
    if (!strcmp(param, "PITCH_KEY_TRACK")) return DS_TARGET_KEY_TRACK;
    if (!strcmp(param, "SILENCING_MODE")) return DS_TARGET_SILENCING_MODE;
    if (!strcmp(param, "SILENCING_DECAY")) return DS_TARGET_SILENCING_DECAY;
    if (!strcmp(param, "TAG_POLYPHONY")) return DS_TARGET_TAG_POLYPHONY;
    if (!strcmp(param, "SAMPLE_START")) return DS_TARGET_SAMPLE_START;
    if (!strcmp(param, "SAMPLE_END")) return DS_TARGET_SAMPLE_END;
    if (!strcmp(param, "LOOP_START")) return DS_TARGET_LOOP_START;
    if (!strcmp(param, "LOOP_END")) return DS_TARGET_LOOP_END;
    if (!strcmp(param, "ROOT_NOTE")) return DS_TARGET_ROOT_NOTE;
    if (!strcmp(param, "LO_NOTE")) return DS_TARGET_LO_NOTE;
    if (!strcmp(param, "HI_NOTE")) return DS_TARGET_HI_NOTE;
    if (!strcmp(param, "LO_VEL")) return DS_TARGET_LO_VEL;
    if (!strcmp(param, "HI_VEL")) return DS_TARGET_HI_VEL;
    if (!strcmp(param, "AMP_ENV_ENABLED")) return DS_TARGET_AMP_ENV_ENABLED;
    if (!strcmp(param, "GROUP_VOLUME")) return DS_TARGET_VOLUME;
    if (!strcmp(param, "GLIDE_TIME")) return DS_TARGET_GLIDE_TIME;
    if (!strcmp(param, "GLIDE_MODE")) return DS_TARGET_GLIDE_MODE;
    if (!strcmp(type, "note") && !strcmp(param, "ENABLED")) return DS_TARGET_MIDI_ENABLED;
    return DS_TARGET_NONE;
}

static void parse_binding(ds_preset_model_t *m, const char *a, const char *e, ds_binding_t *b) {
    char type[32] = "", level[32] = "", param[32] = "", text[1024];
    float f;
    memset(b, 0, sizeof(*b));
    attr(a, e, "type", type, sizeof(type));
    attr(a, e, "level", level, sizeof(level));
    attr(a, e, "parameter", param, sizeof(param));
    snprintf(b->name, sizeof(b->name), "%s", param);
    b->target = target_for(type, level, param);
    b->level = !strcmp(level, "group") ? DS_LEVEL_GROUP : !strcmp(level, "tag") ? DS_LEVEL_TAG :
               !strcmp(level, "ui") ? DS_LEVEL_UI : !strcmp(level, "sample") ? DS_LEVEL_SAMPLE :
               !strcmp(level, "instrument") || !level[0] ? DS_LEVEL_INSTRUMENT : DS_LEVEL_OTHER;
    b->axis = !strcmp(param, "X_VALUE") ? 0 : !strcmp(param, "Y_VALUE") ? 1 : -1;
    b->by_name = b->target == DS_TARGET_CONTROL_VALUE && strcmp(param, "VALUE") && b->axis < 0;
    b->trigger_on_load = !(attr(a, e, "triggerOnLoad", text, sizeof(text)) && !strcasecmp(text, "false"));
    if (!attr_num(a, e, "modAmount", &b->mod_amount)) b->mod_amount = 1;
    b->disabled = attr(a, e, "enabled", text, sizeof(text)) && (!strcasecmp(text, "false") || !strcmp(text, "0"));
    b->position = -1;
    if (attr_num(a, e, "position", &f) || attr_num(a, e, "groupIndex", &f) || attr_num(a, e, "effectIndex", &f) ||
        attr_num(a, e, "controlIndex", &f)) b->position = (int)f;
    /* An effect is addressed as (group, effect within it): groupIndex or
     * controlIndex name the group of a group-level effect, effectIndex or
     * position the effect. Resolved to model.effects once all are parsed. */
    b->effect = -1;
    b->effect_group = -1;
    b->effect_index = attr_num(a, e, "effectIndex", &f) ? (int)f : attr_num(a, e, "position", &f) ? (int)f : 0;
    if (b->level == DS_LEVEL_GROUP && (attr_num(a, e, "groupIndex", &f) || attr_num(a, e, "controlIndex", &f)))
        b->effect_group = (int)f;
    if (b->level == DS_LEVEL_TAG) {
        if (attr(a, e, "identifier", text, sizeof(text)) || attr(a, e, "tags", text, sizeof(text)))
            b->tag_mask = ds_preset_model_tag_mask(m, text);
    } else if (b->target == DS_TARGET_CONTROL_VALUE) {
        if (attr(a, e, "controlTags", text, sizeof(text)) || attr(a, e, "tags", text, sizeof(text))) b->tag_mask = ds_preset_model_tag_mask(m, text);
    } else if (b->target == DS_TARGET_MODULATOR) {
        if (attr(a, e, "modulatorTags", text, sizeof(text)) || attr(a, e, "tags", text, sizeof(text))) b->tag_mask = ds_preset_model_tag_mask(m, text);
    } else if (b->level == DS_LEVEL_SAMPLE) {
        if (attr(a, e, "sampleTags", text, sizeof(text)) || attr(a, e, "tags", text, sizeof(text))) b->tag_mask = ds_preset_model_tag_mask(m, text);
    } else if (attr(a, e, "groupTags", text, sizeof(text)) || attr(a, e, "effectTags", text, sizeof(text)) ||
               attr(a, e, "tags", text, sizeof(text))) {
        b->tag_mask = ds_preset_model_tag_mask(m, text);
    }
    if (attr(a, e, "translation", text, sizeof(text))) {
        if (!strcmp(text, "table")) b->translation = DS_TRANSLATE_TABLE;
        else if (!strcmp(text, "fixed_value")) b->translation = DS_TRANSLATE_FIXED;
    }
    b->has_range = attr_num(a, e, "translationOutputMin", &b->out_min) & attr_num(a, e, "translationOutputMax", &b->out_max);
    if (attr(a, e, "translationReversed", text, sizeof(text))) b->reversed = !strcasecmp(text, "true");
    b->has_factor = attr_num(a, e, "factor", &b->factor);
    if (attr(a, e, "translationTable", text, sizeof(text))) {
        const char *q = text;
        while (*q && b->table_n < DS_MAX_TABLE) {
            char *next;
            float k = strtof(q, &next);
            if (next == q || *next != ',') break;
            b->table_in[b->table_n] = k;
            b->table_out[b->table_n] = strtof(next + 1, &next);
            b->table_n++;
            q = strchr(next, ';');
            if (!q) break;
            ++q;
        }
    }
    b->mod_behavior = DS_MODB_SET;
    if (attr(a, e, "modBehavior", text, sizeof(text))) {
        if (!strcmp(text, "add")) b->mod_behavior = DS_MODB_ADD;
        else if (!strcmp(text, "multiply")) b->mod_behavior = DS_MODB_MULTIPLY;
        else if (!strcmp(text, "modulate")) b->mod_behavior = DS_MODB_MODULATE;
    }
    if (b->target == DS_TARGET_MODULATOR && attr_num(a, e, "modulatorIndex", &f)) b->position = (int)f;
    if (b->target == DS_TARGET_MIDI_ENABLED && attr_num(a, e, "midiElementIndex", &f)) b->position = (int)f;
    if (attr(a, e, "translationValue", text, sizeof(text))) {
        char *tail;
        /* words a fixed value may be: SILENCING_MODE, an LFO's SHAPE (DS_LFO_*), TRIGGER */
        if (!strcasecmp(text, "true") || !strcasecmp(text, "normal") || !strcasecmp(text, "attack")) b->fixed = 1;
        else if (!strcasecmp(text, "false") || !strcasecmp(text, "fast") || !strcasecmp(text, "none")) b->fixed = 0;
        else if (!strcasecmp(text, "sine")) b->fixed = DS_LFO_SINE;
        else if (!strcasecmp(text, "square")) b->fixed = DS_LFO_SQUARE;
        else if (!strcasecmp(text, "saw")) b->fixed = DS_LFO_SAW;
        else if (!strcasecmp(text, "triangle")) b->fixed = DS_LFO_TRIANGLE;
        else if (!strcasecmp(text, "off")) b->fixed = DS_GLIDE_OFF;              /* GLIDE_MODE */
        else if (!strcasecmp(text, "always")) b->fixed = DS_GLIDE_ALWAYS;
        else if (!strcasecmp(text, "legato")) b->fixed = DS_GLIDE_LEGATO;
        else {
            b->fixed = strtof(text, &tail);
            while (isspace((unsigned char)*tail)) ++tail;
            if (!strncasecmp(tail, "db", 2)) b->fixed = powf(10.0f, b->fixed / 20.0f);
        }
    }
}

static void group_settings(ds_group_settings_t *g, const char *a, const char *e, int instrument) {
    static const char *env_names[4] = {"attack", "decay", "sustain", "release"};
    static const float env_defaults[4] = {0.0f, 0.0f, 1.0f, 0.5f};   /* see dspreset_parser.c */
    char text[256];
    float f;
    memset(g, 0, sizeof(*g));
    g->volume = 1;
    if (a) attr_volume(a, e, &g->volume);
    /* modVolume: the group's level as the DecentSampler app saves it (the value
     * its AMP_VOLUME modulation starts from). Undocumented; CS-20M sets its
     * second oscillator to 0.53 and its noise layer to 0.01 with it. */
    if (a && attr_num(a, e, "modVolume", &f)) g->volume *= f < 0 ? 0 : f;
    g->tuning = a && attr_num(a, e, "groupTuning", &f) ? f : 0;
    g->has_pan = a && attr_num(a, e, "pan", &g->pan);
    g->pan /= 100.0f;
    g->vel_track = 1;
    g->has_vel_track = a && attr_num(a, e, "ampVelTrack", &g->vel_track);
    for (int i = 0; i < 4; ++i) {
        g->env[i] = env_defaults[i];
        g->has_env[i] = a && attr_num(a, e, env_names[i], &g->env[i]);
        if (instrument) g->has_env[i] = 1;
    }
    g->key_track = 1;
    g->has_key_track = a && attr_num(a, e, "pitchKeyTrack", &g->key_track);
    g->has_silencing_mode = a && attr(a, e, "silencingMode", text, sizeof(text));
    g->silencing_mode = g->has_silencing_mode && !strcasecmp(text, "normal") ? DS_SILENCE_NORMAL : DS_SILENCE_FAST;
    g->has_silencing_decay = a && attr_num(a, e, "silencingDecay", &g->silencing_decay);
    g->has_glide_time = a && attr_num(a, e, "glideTime", &g->glide_time);
    g->has_glide_mode = a && attr(a, e, "glideMode", text, sizeof(text));
    g->glide_mode = !g->has_glide_mode ? DS_GLIDE_LEGATO : !strcasecmp(text, "always") ? DS_GLIDE_ALWAYS :
                    !strcasecmp(text, "off") ? DS_GLIDE_OFF : DS_GLIDE_LEGATO;
    if (instrument) g->has_pan = g->has_vel_track = g->has_key_track = g->has_silencing_mode = g->has_silencing_decay =
                    g->has_glide_time = g->has_glide_mode = 1;
    g->enabled = !(a && attr(a, e, "enabled", text, sizeof(text)) && (!strcasecmp(text, "false") || !strcmp(text, "0")));
    if (a && attr(a, e, "name", text, sizeof(text))) snprintf(g->name, sizeof(g->name), "%.63s", text);
}

/* ---- labels for controls that bring none (image-skinned presets) -------- */

static const char *alias(const char *p) {
    static const struct { const char *param, *label; } table[] = {
        {"FX_FILTER_FREQUENCY", "Cutoff"}, {"FX_FILTER_RESONANCE", "Reso"}, {"FX_CENTER_FREQUENCY", "Center"},
        {"FX_REVERB_WET_LEVEL", "Reverb"}, {"FX_REVERB_ROOM_SIZE", "Room"}, {"FX_REVERB_DAMPING", "Damp"},
        {"FX_DELAY_TIME", "Dly Time"}, {"FX_FEEDBACK", "Feedback"}, {"FX_WET_LEVEL", "Wet"}, {"FX_MIX", "Mix"},
        {"FX_MOD_RATE", "Rate"}, {"FX_MOD_DEPTH", "Depth"}, {"FX_STEREO_OFFSET", "Width"}, {"FX_FILTER_GAIN", "Gain"},
        {"FX_DRIVE", "Drive"}, {"FX_OUTPUT_LEVEL", "Level"}, {"ENV_ATTACK", "Attack"}, {"ENV_DECAY", "Decay"},
        {"ENV_SUSTAIN", "Sustain"}, {"ENV_RELEASE", "Release"}, {"AMP_VOLUME", "Volume"}, {"TAG_VOLUME", "Volume"},
        {"GLOBAL_TUNING", "Tune"}, {"GROUP_TUNING", "Tune"}, {"TUNING", "Tune"}, {"PAN", "Pan"},
        {"ENABLED", "On"}, {"TAG_ENABLED", "On"}, {"AMP_VEL_TRACK", "Vel Sens"},
        {"MOD_AMOUNT", "Mod Amt"}, {"FREQUENCY", "Rate"}, {"DELAY_TIME", "Delay"}};
    for (unsigned i = 0; i < sizeof(table) / sizeof(table[0]); ++i) if (!strcmp(p, table[i].param)) return table[i].label;
    return NULL;
}

static const char *effect_prefix(const char *type) {
    if (!strcmp(type, "delay")) return "Dly";
    if (!strcmp(type, "chorus")) return "Chr";
    if (!strcmp(type, "phaser")) return "Phs";
    if (!strcmp(type, "reverb") || !strcmp(type, "convolution")) return "Rev";
    if (!strcmp(type, "peak") || !strcmp(type, "lowpass") || !strcmp(type, "highpass") || !strcmp(type, "bandpass") ||
        !strcmp(type, "lowpass_4pl") || !strcmp(type, "notch")) return "EQ";
    return NULL;
}

static void name_control(ds_preset_model_t *m, ds_control_t *c, unsigned index) {
    const ds_binding_t *b;
    unsigned first = c->first_binding, count = c->binding_count;
    if (c->name[0]) return;
    if (!count && c->choice_count) { first = m->choices[c->first_choice].first_binding; count = m->choices[c->first_choice].binding_count; }
    b = count ? &m->bindings[first] : NULL;
    if (b && b->target == DS_TARGET_VOLUME && b->level == DS_LEVEL_GROUP && b->position >= 0 &&
        b->position < (int)m->group_count && m->groups[b->position].name[0]) {
        snprintf(c->name, sizeof(c->name), "%.31s", m->groups[b->position].name);
    } else if (b && b->level == DS_LEVEL_TAG && b->tag_mask) {
        for (unsigned t = 0; t < m->tag_count; ++t)
            if (b->tag_mask & (1ull << t)) { snprintf(c->name, sizeof(c->name), "%.31s", m->tag_names[t]); break; }
    } else if (b && b->target == DS_TARGET_EFFECT && b->effect >= 0) {
        const ds_effect_t *fx = &m->effects[b->effect];
        const char *label = alias(b->name), *prefix = effect_prefix(fx->type);
        float freq = 0;
        for (unsigned i = 0; i < fx->param_count; ++i) if (!strcmp(fx->param_names[i], "frequency")) freq = fx->param_values[i];
        if (!strcmp(fx->type, "peak") && freq > 0 && !strcmp(b->name, "FX_FILTER_GAIN"))
            snprintf(c->name, sizeof(c->name), freq >= 1000 ? "EQ %.3gk" : "EQ %.0f", freq >= 1000 ? freq / 1000 : freq);
        else if (label && prefix && (!strcmp(label, "Wet") || !strcmp(label, "Mix") || !strcmp(label, "Rate") ||
                                     !strcmp(label, "Depth") || !strcmp(label, "Feedback") || !strcmp(label, "On")))
            snprintf(c->name, sizeof(c->name), "%s %s", prefix, label);
        else if (label) snprintf(c->name, sizeof(c->name), "%s", label);
    } else if (b && alias(b->name)) {
        snprintf(c->name, sizeof(c->name), "%s", alias(b->name));
    }
    if (!c->name[0]) snprintf(c->name, sizeof(c->name), "%s %u", c->kind == DS_CONTROL_KNOB ? "Knob" : c->kind == DS_CONTROL_BUTTON ? "Button" : "Menu", index + 1);
}

/* ---- the walk ----------------------------------------------------------- */

int ds_preset_model_load(ds_preset_model_t *m, const char *path, char *error, unsigned error_len) {
    FILE *file;
    long length;
    char *xml, *p, *w;
    int in_ui = 0, in_midi = 0, in_mod = 0, in_effects = 0, in_group = 0;
    int ctrl = -1, choice = -1, cc = -1, group = -1, modulator = -1, pad = -1, ui_count = 0, in_velocity = 0;
    int note_map = -1, midi_index = 0;
    memset(m, 0, sizeof(*m));
    group_settings(&m->instrument, NULL, NULL, 1);
    for (unsigned t = 0; t < DS_MAX_TAGS; ++t) { m->tag_volume[t] = 1; m->tag_enabled[t] = 1; m->tag_polyphony[t] = -1; }
    if (!(file = fopen(path, "rb")) || fseek(file, 0, SEEK_END) || (length = ftell(file)) < 0 ||
        length > MAX_PRESET_BYTES || fseek(file, 0, SEEK_SET)) {
        if (file) fclose(file);
        fail(error, error_len, "cannot read DSPreset"); return -1;
    }
    xml = malloc((size_t)length + 1);
    m->groups = calloc(MAX_GROUPS, sizeof(ds_group_settings_t));
    m->choices = calloc(MAX_CHOICES, sizeof(ds_choice_t));
    m->bindings = calloc(MAX_BINDINGS, sizeof(ds_binding_t));
    m->ccs = calloc(MAX_CCS, sizeof(ds_cc_map_t));
    m->effects = calloc(DS_MAX_EFFECTS, sizeof(ds_effect_t));
    if (!xml || !m->groups || !m->choices || !m->bindings || !m->ccs || !m->effects ||
        fread(xml, 1, (size_t)length, file) != (size_t)length) {
        fclose(file); free(xml); ds_preset_model_free(m);
        fail(error, error_len, "cannot read DSPreset"); return -1;
    }
    fclose(file);
    xml[length] = '\0';
    for (p = w = xml; *p; ) {                                /* drop comments */
        if (!strncmp(p, "<!--", 4)) { char *c = strstr(p + 4, "-->"); if (!c) break; p = c + 3; continue; }
        *w++ = *p++;
    }
    *w = '\0';

    for (p = xml; (p = strchr(p, '<')) != NULL; ) {
        char *tag = p + 1, *end = strchr(tag, '>'), *a;
        int closing = 0, self_closing;
        if (!end) break;
        p = end + 1;
        if (*tag == '?' || *tag == '!') continue;
        if (*tag == '/') { closing = 1; ++tag; }
        self_closing = end > tag && end[-1] == '/';
        a = tag;
        while (*a && is_name_char(*a)) ++a;               /* attributes start after the name */

        if (tag_is(tag, "ui")) { in_ui = !closing && !self_closing; continue; }
        if (tag_is(tag, "midi")) { in_midi = !closing && !self_closing; continue; }
        if (tag_is(tag, "modulators")) { in_mod = !closing && !self_closing; modulator = -1; continue; }
        if (in_midi && tag_is(tag, "velocity")) {
            /* <midi><velocity>: its bindings follow each note's velocity, as a
             * per-note <midiVelocity> modulator's would */
            if (closing) { modulator = -1; in_velocity = 0; continue; }
            midi_index++;
            if (m->modulator_count == DS_MAX_MODULATORS || self_closing) continue;
            memset(&m->modulators[m->modulator_count], 0, sizeof(ds_modulator_t));
            m->modulators[m->modulator_count].kind = DS_MOD_VELOCITY;
            m->modulators[m->modulator_count].voice_scope = 1;
            m->modulators[m->modulator_count].mod_amount = 1;
            m->modulators[m->modulator_count].first_binding = m->binding_count;
            modulator = (int)m->modulator_count++;
            in_velocity = 1;
            continue;
        }
        if (in_mod && (tag_is(tag, "lfo") || tag_is(tag, "envelope") || tag_is(tag, "midiCC") || tag_is(tag, "midiVelocity") ||
                       tag_is(tag, "random"))) {
            ds_modulator_t *mod;
            char text[32];
            float v;
            if (closing) { modulator = -1; continue; }
            if (m->modulator_count == DS_MAX_MODULATORS) continue;
            mod = &m->modulators[m->modulator_count];
            memset(mod, 0, sizeof(*mod));
            mod->kind = tag_is(tag, "lfo") ? DS_MOD_LFO : tag_is(tag, "envelope") ? DS_MOD_ENVELOPE :
                        tag_is(tag, "midiCC") ? DS_MOD_CC : tag_is(tag, "random") ? DS_MOD_RANDOM : DS_MOD_VELOCITY;
            mod->trigger = attr(a, end, "trigger", text, sizeof(text)) && !strcasecmp(text, "attack");
            mod->periodic = attr(a, end, "mode", text, sizeof(text)) && !strcasecmp(text, "periodic");
            mod->seed = attr_num(a, end, "seed", &v) ? (uint32_t)(int64_t)v : 0;
            { char tags[256]; if (attr(a, end, "tags", tags, sizeof(tags))) mod->tag_mask = ds_preset_model_tag_mask(m, tags); }
            /* scope: LFOs (and random) default to one shared instance, the rest to one per note */
            mod->voice_scope = mod->kind != DS_MOD_LFO && mod->kind != DS_MOD_RANDOM;
            if (attr(a, end, "scope", text, sizeof(text))) mod->voice_scope = !strcmp(text, "voice");
            mod->shape = DS_LFO_SINE;
            if (attr(a, end, "shape", text, sizeof(text))) {
                if (!strcmp(text, "square")) mod->shape = DS_LFO_SQUARE;
                else if (!strcmp(text, "saw")) mod->shape = DS_LFO_SAW;
                else if (!strcmp(text, "triangle")) mod->shape = DS_LFO_TRIANGLE;
            }
            mod->frequency = attr_num(a, end, "frequency", &v) ? v : 1;
            mod->mod_amount = attr_num(a, end, "modAmount", &v) ? v : 1;
            mod->delay = attr_num(a, end, "delayTime", &v) ? v : 0;
            mod->attack = attr_num(a, end, "attack", &v) ? v : 0;
            mod->decay = attr_num(a, end, "decay", &v) ? v : 0;
            mod->sustain = attr_num(a, end, "sustain", &v) ? v : 1;
            mod->release = attr_num(a, end, "release", &v) ? v : 0;
            mod->cc = attr_num(a, end, "number", &v) ? (int)v : -1;
            mod->first_binding = m->binding_count;
            modulator = self_closing ? -1 : (int)m->modulator_count;
            m->modulator_count++;
            continue;
        }
        if (!closing && tag_is(tag, "tag")) {                 /* <tags><tag name volume enabled polyphony> */
            char name[64], text[32];
            uint64_t bit;
            float v;
            if (!attr(a, end, "name", name, sizeof(name)) || !(bit = ds_preset_model_tag_mask(m, name))) continue;
            for (unsigned t = 0; t < DS_MAX_TAGS; ++t) {
                if (!(bit & (1ull << t))) continue;
                if (attr_volume(a, end, &v)) m->tag_volume[t] = v < 0 ? 0 : v;
                if (attr(a, end, "enabled", text, sizeof(text))) m->tag_enabled[t] = !(!strcasecmp(text, "false") || !strcmp(text, "0"));
                if (attr_num(a, end, "polyphony", &v)) m->tag_polyphony[t] = v >= 1 ? (int)lrintf(v) : -1;
            }
            continue;
        }
        if (tag_is(tag, "effects")) { in_effects = !closing && !self_closing; continue; }
        if (tag_is(tag, "groups")) { if (!closing) group_settings(&m->instrument, a, end, 1); continue; }
        if (tag_is(tag, "group")) {
            if (closing) { in_group = 0; continue; }
            if (m->group_count == MAX_GROUPS) continue;
            group = (int)m->group_count++;
            group_settings(&m->groups[group], a, end, 0);
            { char tags[256]; if (attr(a, end, "tags", tags, sizeof(tags))) m->groups[group].tag_mask = ds_preset_model_tag_mask(m, tags); }
            in_group = !self_closing;
            continue;
        }
        if (in_effects && !closing && tag_is(tag, "effect") && m->effect_count < DS_MAX_EFFECTS) {
            ds_effect_t *fx = &m->effects[m->effect_count++];
            const char *q = a;
            char text[256];
            attr(a, end, "type", fx->type, sizeof(fx->type));
            fx->group = in_group ? group : -1;
            fx->enabled = !(attr(a, end, "enabled", text, sizeof(text)) && !strcasecmp(text, "false"));
            if (attr(a, end, "tags", text, sizeof(text))) fx->tag_mask = ds_preset_model_tag_mask(m, text);
            if (attr(a, end, "levelUnit", text, sizeof(text)) && !strcasecmp(text, "linear")) {
                snprintf(fx->param_names[fx->param_count], sizeof(fx->param_names[0]), "levelLinear");
                fx->param_values[fx->param_count++] = 1;
            }
            if (attr(a, end, "autoBypass", text, sizeof(text)) && !strcasecmp(text, "true")) {
                snprintf(fx->param_names[fx->param_count], sizeof(fx->param_names[0]), "autoBypass");
                fx->param_values[fx->param_count++] = 1;
            }
            if (attr(a, end, "delayTimeFormat", text, sizeof(text)) && !strcasecmp(text, "musical_time")) {
                snprintf(fx->param_names[fx->param_count], sizeof(fx->param_names[0]), "musicalTime");
                fx->param_values[fx->param_count++] = 1;
            }
            while (q < end && fx->param_count < DS_MAX_EFFECT_PARAMS) {    /* every numeric attribute */
                char key[24]; unsigned n = 0; float v;
                while (q < end && !is_name_char(*q)) ++q;
                while (q < end && is_name_char(*q) && n + 1 < sizeof(key)) key[n++] = *q++;
                key[n] = '\0';
                while (q < end && *q != '"' && *q != '\'') ++q;
                if (q >= end) break;
                { char quote = *q++; while (q < end && *q != quote) ++q; ++q; }
                if (n && strcmp(key, "type") && strcmp(key, "tags") && attr_num(a, end, key, &v)) {
                    snprintf(fx->param_names[fx->param_count], sizeof(fx->param_names[0]), "%s", !strcmp(key, "Q") ? "q" : key);
                    fx->param_values[fx->param_count++] = v;
                }
            }
            continue;
        }
        if (in_ui) {
            int knob = tag_is(tag, "labeled-knob") || tag_is(tag, "control");
            if (tag_is(tag, "xyPad")) {
                /* Two knobs, X then Y, each with the bindings of its <x> / <y>. */
                if (closing || self_closing) { pad = -1; ctrl = -1; if (!self_closing) continue; }
                if (m->control_count + 2 <= DS_MAX_CONTROLS) {
                    char text[64] = "", base[32];
                    float v;
                    size_t n;
                    if (!(attr(a, end, "label", text, sizeof(text)) && text[0])) attr(a, end, "parameterName", text, sizeof(text));
                    snprintf(base, sizeof(base), "%.24s", text);
                    n = strlen(base);                                   /* "LowpassXY" -> "Lowpass" */
                    if (n > 2 && !strcasecmp(base + n - 2, "xy")) base[n -= 2] = '\0';
                    while (n && base[n - 1] == ' ') base[--n] = '\0';
                    for (int axis = 0; axis < 2; ++axis) {
                        ds_control_t *c = &m->controls[m->control_count + axis];
                        memset(c, 0, sizeof(*c));
                        c->kind = DS_CONTROL_KNOB;
                        c->min = 0; c->max = 1;
                        c->def = attr_num(a, end, axis ? "yValue" : "xValue", &v) ? v : 0;
                        if (base[0]) snprintf(c->name, sizeof(c->name), "%s %c", base, axis ? 'Y' : 'X');
                        attr(a, end, "parameterName", c->param_name, sizeof(c->param_name));
                        { char tags[256]; if (attr(a, end, "tags", tags, sizeof(tags))) c->tag_mask = ds_preset_model_tag_mask(m, tags); }
                        c->ds_index = ui_count;
                        c->xy_axis = axis;
                        c->first_binding = m->binding_count;
                        c->first_choice = m->choice_count;
                    }
                    pad = self_closing ? -1 : (int)m->control_count;
                    m->control_count += 2;
                }
                ui_count++;
                ctrl = -1;
                continue;
            }
            if (pad >= 0 && (tag_is(tag, "x") || tag_is(tag, "y"))) {
                if (closing || self_closing) { ctrl = -1; continue; }
                ctrl = pad + (tag_is(tag, "y") ? 1 : 0);
                m->controls[ctrl].first_binding = m->binding_count;
                continue;
            }
            if ((knob || tag_is(tag, "button") || tag_is(tag, "menu")) && closing) { ctrl = -1; continue; }
            if ((knob || tag_is(tag, "button") || tag_is(tag, "menu")) && m->control_count < DS_MAX_CONTROLS) {
                ds_control_t *c = &m->controls[m->control_count];
                char text[64];
                float v;
                memset(c, 0, sizeof(*c));
                c->kind = knob ? DS_CONTROL_KNOB : tag_is(tag, "button") ? DS_CONTROL_BUTTON : DS_CONTROL_MENU;
                if ((attr(a, end, "label", text, sizeof(text)) && text[0]) ||
                    (attr(a, end, "parameterName", text, sizeof(text)) && text[0]))
                    snprintf(c->name, sizeof(c->name), "%.31s", text);
                attr(a, end, "parameterName", c->param_name, sizeof(c->param_name));
                { char tags[256]; if (attr(a, end, "tags", tags, sizeof(tags))) c->tag_mask = ds_preset_model_tag_mask(m, tags); }
                c->ds_index = ui_count++;
                c->xy_axis = -1;
                c->min = attr_num(a, end, "minValue", &v) ? v : 0;
                c->max = attr_num(a, end, "maxValue", &v) ? v : 1;
                c->def = attr_num(a, end, "value", &v) ? v : c->min;
                c->integer = attr(a, end, "type", text, sizeof(text)) && !strcmp(text, "integer");
                if (c->kind == DS_CONTROL_MENU) c->def = c->def >= 1 ? c->def - 1 : 0;   /* DS menus are 1-based */
                c->first_binding = m->binding_count;
                c->first_choice = m->choice_count;
                ctrl = (int)m->control_count++;
                choice = -1;
                if (self_closing) ctrl = -1;
                continue;
            }
            if ((tag_is(tag, "state") || tag_is(tag, "option")) && ctrl >= 0) {
                if (closing) { choice = -1; continue; }
                if (m->choice_count < MAX_CHOICES) {
                    ds_choice_t *ch = &m->choices[m->choice_count];
                    char text[64] = "";
                    attr(a, end, "name", text, sizeof(text));
                    snprintf(ch->name, sizeof(ch->name), "%.31s", text[0] ? text : "Option");
                    ch->first_binding = m->binding_count;
                    m->controls[ctrl].choice_count++;
                    choice = (int)m->choice_count++;
                    if (self_closing) choice = -1;
                }
                continue;
            }
        }
        if (in_midi && tag_is(tag, "note")) {
            char text[64];
            if (closing) { note_map = -1; continue; }
            midi_index++;
            if (m->note_count < DS_MAX_NOTE_MAPS && attr(a, end, "note", text, sizeof(text))) {
                ds_note_map_t *n = &m->notes[m->note_count];
                char *dash = strchr(text + 1, '-');         /* "24-35" ("-1" alone is not a range) */
                memset(n, 0, sizeof(*n));
                if (dash) { *dash = '\0'; n->hi = ds_note_number(dash + 1); }
                n->lo = ds_note_number(text);
                if (!dash) n->hi = n->lo;
                if (n->lo == DS_NO_NOTE || n->hi == DS_NO_NOTE) continue;
                n->event = !attr(a, end, "eventType", text, sizeof(text)) ? DS_NOTE_EVENT_ON :
                           !strcasecmp(text, "note_off") ? DS_NOTE_EVENT_OFF : !strcasecmp(text, "any") ? DS_NOTE_EVENT_ANY : DS_NOTE_EVENT_ON;
                n->enabled = !(attr(a, end, "enabled", text, sizeof(text)) && (!strcasecmp(text, "false") || !strcmp(text, "0")));
                n->swallow = attr(a, end, "swallowNotes", text, sizeof(text)) && !strcasecmp(text, "true");
                n->midi_index = midi_index - 1;
                n->first_binding = m->binding_count;
                note_map = self_closing ? -1 : (int)m->note_count;
                m->note_count++;
            }
            continue;
        }
        if (in_midi && tag_is(tag, "cc")) {
            float v;
            if (closing) { cc = -1; continue; }
            midi_index++;
            if (m->cc_count < MAX_CCS && attr_num(a, end, "number", &v)) {
                m->ccs[m->cc_count].cc = (int)v;
                m->ccs[m->cc_count].first_binding = m->binding_count;
                cc = self_closing ? -1 : (int)m->cc_count;
                m->cc_count++;
            }
            continue;
        }
        if (!closing && tag_is(tag, "binding") && (in_mod || in_velocity) && modulator >= 0 && m->binding_count < MAX_BINDINGS) {
            parse_binding(m, a, end, &m->bindings[m->binding_count++]);
            m->modulators[modulator].binding_count++;
            continue;
        }
        if (!closing && tag_is(tag, "binding") && !in_mod && m->binding_count < MAX_BINDINGS) {
            if (in_ui && ctrl >= 0 && choice >= 0) { parse_binding(m, a, end, &m->bindings[m->binding_count++]); m->choices[choice].binding_count++; }
            else if (in_ui && ctrl >= 0 && m->controls[ctrl].kind == DS_CONTROL_KNOB) { parse_binding(m, a, end, &m->bindings[m->binding_count++]); m->controls[ctrl].binding_count++; }
            else if (in_midi && cc >= 0) { parse_binding(m, a, end, &m->bindings[m->binding_count++]); m->ccs[cc].binding_count++; }
            else if (in_midi && note_map >= 0) { parse_binding(m, a, end, &m->bindings[m->binding_count++]); m->notes[note_map].binding_count++; }
        }
    }
    free(xml);
    /* A pad axis nothing is bound to is no knob (BassForge's filter pads move
     * only Y): the other axis keeps the pad's plain name. */
    for (unsigned i = 0; i < m->control_count; ) {
        ds_control_t *c = &m->controls[i];
        if (c->xy_axis >= 0 && !c->binding_count) {
            int partner = c->xy_axis ? (int)i - 1 : (int)i + 1;
            if (partner >= 0 && partner < (int)m->control_count && m->controls[partner].xy_axis >= 0 &&
                m->controls[partner].ds_index == c->ds_index && m->controls[partner].binding_count) {
                char *name = m->controls[partner].name;
                size_t n = strlen(name);
                if (n > 2 && name[n - 2] == ' ') name[n - 2] = '\0';
                memmove(c, c + 1, (m->control_count - i - 1) * sizeof(*c));
                m->control_count--;
                continue;
            }
        }
        ++i;
    }
    /* Controls a binding names: by DecentSampler's index (a pad's axes share
     * one; VALUE is its X), or by parameterName. */
    for (unsigned i = 0; i < m->binding_count; ++i) {
        ds_binding_t *b = &m->bindings[i];
        int found = -1;
        if (b->target != DS_TARGET_CONTROL_VALUE || b->tag_mask) continue;
        for (unsigned k = 0; k < m->control_count && found < 0; ++k) {
            const ds_control_t *c = &m->controls[k];
            int hit = b->by_name ? (c->param_name[0] && !strcmp(c->param_name, b->name)) : c->ds_index == b->position;
            if (hit && (c->xy_axis < 0 || b->axis < 0 || c->xy_axis == b->axis)) found = (int)k;
        }
        if (found < 0) b->target = DS_TARGET_NONE;
        else b->position = found;
    }
    for (unsigned i = 0; i < m->binding_count; ++i) {
        ds_binding_t *b = &m->bindings[i];
        int k = 0;
        if (b->target != DS_TARGET_EFFECT || b->tag_mask) continue;
        for (unsigned x = 0; x < m->effect_count; ++x) {
            if (m->effects[x].group != b->effect_group) continue;
            if (k++ == b->effect_index) { b->effect = (int)x; break; }
        }
    }
    for (unsigned i = 0; i < m->control_count; ++i) {
        ds_control_t *c = &m->controls[i];
        if (c->kind != DS_CONTROL_KNOB) { c->min = 0; c->max = c->choice_count ? (float)(c->choice_count - 1) : 0; c->integer = 1; }
        if (c->def < c->min) c->def = c->min;
        if (c->def > c->max) c->def = c->max;
        name_control(m, c, i);
    }
    /* Two knobs both called "Volume" say nothing; number the repeats. */
    for (unsigned i = 0; i < m->control_count; ++i) {
        unsigned seen = 1;
        for (unsigned j = i + 1; j < m->control_count; ++j)
            if (!strcmp(m->controls[i].name, m->controls[j].name)) {
                char base[32];
                snprintf(base, sizeof(base), "%.20s", m->controls[j].name);
                snprintf(m->controls[j].name, sizeof(m->controls[j].name), "%.20s %u", base, ++seen % 1000u);
            }
    }
    return 0;
}

void ds_preset_model_free(ds_preset_model_t *m) {
    if (!m) return;
    free(m->groups); free(m->choices); free(m->bindings); free(m->ccs); free(m->effects);
    memset(m, 0, sizeof(*m));
}
