#include "native_engine.h"

#include <dirent.h>
#include <fcntl.h>
#include <math.h>
#include <strings.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define RING_MASK (DS_RING_FRAMES - 1)
#define MAX_MEMORY_FRAMES (60u * 48000u)   /* playbackMode="memory" is honoured up to this */
#define SILENT 0.001f                      /* -60 dB: an envelope below this has finished */

static void fail(char *out, unsigned n, const char *message) {
    if (n) snprintf(out, n, "%s", message);
}

/* ---- stream word -------------------------------------------------------- */

static uint64_t pack(uint32_t generation, uint32_t zone_plus_one, uint32_t produced) {
    return ((uint64_t)(generation & 0xffff) << 48) | ((uint64_t)(zone_plus_one & 0xffff) << 32) | produced;
}
static uint32_t packed_zone(uint64_t word) { return (uint32_t)(word >> 32) & 0xffff; }
static uint32_t packed_produced(uint64_t word) { return (uint32_t)word; }

/* Virtual frame (position along the play path, loops unrolled) -> file frame. */
static uint64_t map_frame(const ds_zone_t *zone, uint64_t v) {
    if (zone->loop && v >= zone->loop_end)
        return zone->loop_start + (v - zone->loop_start) % (zone->loop_end - zone->loop_start);
    return v;
}

/* ---- loading ------------------------------------------------------------ */

typedef struct { ds_native_engine_t *engine; char *error; unsigned error_len; } collect_t;

static int collect_zone(const ds_dspreset_sample_t *sample, void *opaque) {
    collect_t *c = opaque;
    ds_native_engine_t *e = c->engine;
    if (e->zone_count == DS_MAX_ZONES) { fail(c->error, c->error_len, "DSPreset has too many samples"); return 1; }
    memset(&e->zones[e->zone_count], 0, sizeof(ds_zone_t));
    e->zones[e->zone_count].def = *sample;
    e->zones[e->zone_count].source = -1;
    if ((unsigned)sample->group_index + 1 > e->group_count) e->group_count = (unsigned)sample->group_index + 1;
    e->zone_count++;
    return 0;
}

/* A path as a preset writes it may differ in CASE from the files: made on a
 * Mac or PC, where names are case-insensitive, it plays in DecentSampler and
 * finds nothing on the Move's Linux (BassForge: "samples/…" against a
 * "Samples" folder). Resolve it one component at a time, matching each
 * directory entry case-insensitively; an exact name is kept as it is.
 * Returns 0 and the path as it exists on disk, or -1. Worker only. */
int ds_resolve_path_case(const char *path, char *out, size_t out_len) {
    char built[1600];
    const char *p = path;
    size_t len = 0;
    built[0] = '\0';
    if (*p == '/') { built[0] = '/'; built[1] = '\0'; len = 1; while (*p == '/') ++p; }
    while (*p) {
        char part[256];
        size_t n = 0;
        struct stat st;
        while (*p && *p != '/' && n + 1 < sizeof(part)) part[n++] = *p++;
        part[n] = '\0';
        while (*p == '/') ++p;
        if (!strcmp(part, ".") || !n) continue;
        if (len + n + 2 >= sizeof(built)) return -1;
        snprintf(built + len, sizeof(built) - len, "%s%s", len && built[len - 1] != '/' ? "/" : "", part);
        if (!stat(built, &st)) { len = strlen(built); continue; }
        {   /* not as written: find it ignoring case */
            DIR *dir;
            struct dirent *entry;
            int found = 0;
            built[len] = '\0';
            dir = opendir(len ? built : ".");
            if (!dir) return -1;
            while ((entry = readdir(dir)) != NULL)
                if (!strcasecmp(entry->d_name, part)) {
                    snprintf(built + len, sizeof(built) - len, "%s%s", len && built[len - 1] != '/' ? "/" : "", entry->d_name);
                    found = 1;
                    break;
                }
            closedir(dir);
            if (!found) return -1;
            len = strlen(built);
        }
    }
    if (snprintf(out, out_len, "%s", built) >= (int)out_len) return -1;
    return 0;
}

static int source_for(ds_native_engine_t *e, const char *path) {
    ds_source_t *s;
    char error[128];
    for (unsigned i = 0; i < e->source_count; ++i)
        if (!strcmp(e->source_paths[i], path)) return e->sources[i].file.frame_count ? (int)i : -1;
    s = &e->sources[e->source_count];
    snprintf(e->source_paths[e->source_count], sizeof(e->source_paths[0]), "%s", path);
    e->source_count++;
    if (ds_wav_source_open(&s->file, path, error, sizeof(error))) {
        char actual[1600];
        /* the worker reopens this path to stream, so keep the one that exists */
        if (!ds_resolve_path_case(path, actual, sizeof(actual)) && strcmp(actual, path) &&
            !ds_wav_source_open(&s->file, actual, error, sizeof(error)))
            snprintf(e->source_paths[e->source_count - 1], sizeof(e->source_paths[0]), "%s", actual);
    }
    if (s->file.fd < 0 ||
        s->file.channels > 2 || !s->file.frame_count) {
        ds_wav_source_close(&s->file);
        s->file.frame_count = 0;
        e->missing_files++;
        return -1;
    }
    return (int)(e->source_count - 1);
}

static void resolve_bounds(ds_zone_t *z, const ds_source_t *s) {
    const ds_dspreset_sample_t *d = &z->def;
    uint64_t frames = s->file.frame_count;
    int loop_on;
    z->start = d->start >= 0 ? (uint64_t)d->start : 0;
    z->end = d->end >= 0 ? (uint64_t)d->end + 1 : frames;   /* DS `end` is the last frame played */
    if (z->end > frames) z->end = frames;
    if (z->start >= z->end) z->start = 0;
    /* An explicit loopEnabled wins; otherwise a loop the file carries is used,
     * as DecentSampler does with embedded markers. */
    loop_on = d->loop_enabled == 1 || (d->loop_enabled == -1 && s->file.has_loop);
    z->loop_start = d->loop_start >= 0 ? (uint64_t)d->loop_start : s->file.has_loop ? s->file.loop_start : z->start;
    z->loop_end = d->loop_end >= 0 ? (uint64_t)d->loop_end + 1 : s->file.has_loop ? s->file.loop_end + 1 : z->end;
    if (z->loop_end > frames) z->loop_end = frames;
    z->loop = loop_on && z->loop_end > z->loop_start + 1 && z->loop_end > z->start;
    if (s->resident) z->streams = 0;
    else z->streams = (z->loop ? z->loop_end : z->end) > s->head_frames || z->start >= s->head_frames;
}

int ds_native_engine_load(ds_native_engine_t *e, const char *preset_path,
                          unsigned output_rate, ds_cancel_fn cancelled, void *cancel_context,
                          char *error, unsigned error_len) {
    char directory[1024], path[1600], *slash;
    collect_t collect;
    if (!e || !preset_path || !output_rate) return -1;
    memset(e, 0, sizeof(*e));
    e->output_rate = output_rate;
    for (int i = 0; i < 4; ++i) e->amp_override[i] = -1.0f;
    e->bend_ratio = 1.0;
    e->rng = 0x12345678u;
    for (int i = 0; i < DS_MAX_VOICES; ++i) { e->voices[i].generation = (uint32_t)i; e->stream_fd[i] = -1; }
    if (snprintf(directory, sizeof(directory), "%s", preset_path) >= (int)sizeof(directory) ||
        !(slash = strrchr(directory, '/'))) { fail(error, error_len, "bad DSPreset path"); return -1; }
    *slash = '\0';

    e->zones = calloc(DS_MAX_ZONES, sizeof(ds_zone_t));
    if (!e->zones) { fail(error, error_len, "out of memory"); return -1; }
    collect = (collect_t){e, error, error_len};
    if (ds_dspreset_visit_samples(preset_path, collect_zone, &collect, error, error_len) || !e->zone_count) {
        ds_native_engine_destroy(e); return -1;
    }
    e->sources = calloc(e->zone_count, sizeof(ds_source_t));
    e->source_paths = calloc(e->zone_count, sizeof(e->source_paths[0]));
    e->group_len = calloc(e->group_count ? e->group_count : 1, 1);
    if (!e->sources || !e->source_paths || !e->group_len) {
        fail(error, error_len, "out of memory"); ds_native_engine_destroy(e); return -1;
    }
    for (unsigned i = 0; i < e->zone_count; ++i) {
        snprintf(path, sizeof(path), "%s/%s", directory, e->zones[i].def.path);
        e->zones[i].source = source_for(e, path);
    }
    /* Residency is per file: small files, and files a zone asks to keep in memory. */
    for (unsigned i = 0; i < e->zone_count; ++i) {
        ds_zone_t *z = &e->zones[i];
        if (z->source >= 0 && z->def.playback_mode == DS_PLAYBACK_MEMORY &&
            e->sources[z->source].file.frame_count <= MAX_MEMORY_FRAMES) e->sources[z->source].resident = 1;
    }
    for (unsigned i = 0; i < e->source_count; ++i) {
        ds_source_t *s = &e->sources[i];
        char read_error[128];
        if (cancelled && cancelled(cancel_context)) {
            fail(error, error_len, "cancelled");
            for (unsigned j = i; j < e->source_count; ++j) ds_wav_source_close(&e->sources[j].file);
            ds_native_engine_destroy(e);
            return DS_LOAD_CANCELLED;
        }
        if (s->file.fd < 0 || !s->file.frame_count) continue;
        if (s->file.frame_count <= DS_RESIDENT_FRAMES) s->resident = 1;
        s->head_frames = s->resident ? s->file.frame_count : DS_HEAD_FRAMES;
        s->head = malloc((size_t)s->head_frames * s->file.channels * sizeof(float));
        if (!s->head || ds_wav_source_read_frames(&s->file, 0, s->head, (unsigned)s->head_frames,
                                                  read_error, sizeof(read_error)) != (int)s->head_frames) {
            free(s->head); s->head = NULL; s->head_frames = 0;
            ds_wav_source_close(&s->file);
            e->missing_files++;
            continue;
        }
        e->resident_bytes += (uint64_t)s->head_frames * s->file.channels * sizeof(float);
        ds_wav_source_close(&s->file);    /* reopened per streaming voice by the worker */
    }
    for (unsigned i = 0; i < e->zone_count; ++i) {
        ds_zone_t *z = &e->zones[i];
        if (z->source >= 0 && !e->sources[z->source].head) z->source = -1;
        if (z->source < 0) { e->missing_zones++; continue; }
        resolve_bounds(z, &e->sources[z->source]);
    }
    if (e->missing_zones == e->zone_count) {
        snprintf(error, error_len, "no playable samples (%u files missing)", e->missing_files);
        ds_native_engine_destroy(e); return -1;
    }
    if (ds_preset_model_load(&e->model, preset_path, error, error_len)) { ds_native_engine_destroy(e); return -1; }
    e->groups_rt = calloc(e->model.group_count ? e->model.group_count : 1, sizeof(ds_group_settings_t));
    if (!e->groups_rt) { fail(error, error_len, "out of memory"); ds_native_engine_destroy(e); return -1; }
    memcpy(e->groups_rt, e->model.groups, e->model.group_count * sizeof(ds_group_settings_t));
    e->instrument_rt = e->model.instrument;
    for (unsigned i = 0; i < e->zone_count; ++i) {
        e->zones[i].tag_mask = ds_preset_model_tag_mask(&e->model, e->zones[i].def.tags);
        if (e->zones[i].def.group_index >= (int)e->model.group_count) { e->zones[i].source = -1; }
    }
    for (unsigned t = 0; t < DS_MAX_TAGS; ++t) { e->tag_volume[t] = 1.0f; e->tag_enabled[t] = 1; }
    /* Controls start at the preset's values, and fire (DecentSampler's
     * triggerOnLoad default) so the sound matches what the preset shows. */
    for (unsigned c = 0; c < e->model.control_count; ++c) ds_native_engine_set_control(e, c, e->model.controls[c].def);
    for (unsigned x = 0; x < e->model.effect_count; ++x) { ds_fx_prepare(&e->fx_coeffs[x], &e->model.effects[x], (float)output_rate); e->fx_dirty[x] = 0; }
    /* An instrument-level reverb gets its buffers now (the audio thread never
     * allocates). A GROUP-level one would be a reverb per note, as DecentSampler
     * runs it — far too heavy here — so it stays off. */
    for (unsigned x = 0; x < e->model.effect_count; ++x)
        if (!strcmp(e->model.effects[x].type, "reverb") && e->model.effects[x].group < 0 &&
            !(e->reverb[x] = ds_reverb_create((float)output_rate))) {
            fail(error, error_len, "out of memory"); ds_native_engine_destroy(e); return -1;
        }
    for (unsigned k = 0; k < e->model.modulator_count; ++k) {
        const ds_modulator_t *m = &e->model.modulators[k];
        e->mod_global[k].stage = DS_ENV_DONE;                         /* a global envelope waits for a key */
        for (unsigned i = 0; i < m->binding_count; ++i) {
            const ds_binding_t *b = &e->model.bindings[m->first_binding + i];
            if (b->target != DS_TARGET_EFFECT) continue;
            for (unsigned x = 0; x < e->model.effect_count; ++x)
                if (b->tag_mask ? (e->model.effects[x].tag_mask & b->tag_mask) != 0 : b->effect == (int)x) e->fx_modulated[x] = 1;
        }
    }
    for (int i = 0; i < DS_MAX_VOICES; ++i) {
        e->voices[i].ring = malloc((size_t)DS_RING_FRAMES * 2 * sizeof(float));
        if (!e->voices[i].ring) { fail(error, error_len, "out of memory"); ds_native_engine_destroy(e); return -1; }
        /* Touch every page here, so the audio thread never takes a first-touch fault. */
        memset(e->voices[i].ring, 0, (size_t)DS_RING_FRAMES * 2 * sizeof(float));
    }
    return 0;
}

void ds_native_engine_destroy(ds_native_engine_t *e) {
    if (!e) return;
    for (unsigned i = 0; i < e->source_count; ++i) free(e->sources[i].head);
    for (int i = 0; i < DS_MAX_VOICES; ++i) if (e->stream_fd[i] >= 0 && e->stream_key[i]) close(e->stream_fd[i]);
    for (int i = 0; i < DS_MAX_VOICES; ++i) free(e->voices[i].ring);
    for (unsigned x = 0; x < DS_MAX_EFFECTS; ++x) ds_reverb_destroy(e->reverb[x]);
    free(e->sources); free(e->source_paths); free(e->zones); free(e->group_len); free(e->groups_rt);
    ds_preset_model_free(&e->model);
    memset(e, 0, sizeof(*e));
}

/* ---- audio thread ------------------------------------------------------- */

static float coef_for(float seconds, unsigned rate) {
    return seconds > 0 ? expf(logf(SILENT) / (seconds * (float)rate)) : 0.0f;
}

/* ---- live settings ------------------------------------------------------
 * A zone's volume, pitch, pan and envelope come from its own attributes where
 * it sets them, else its group's LIVE settings, else the instrument's — so a
 * control bound to a group or the instrument moves everything that inherits. */
typedef struct { float gain, pan, vel_track, env[4]; double tuning; } zone_now_t;

static int zone_enabled(const ds_native_engine_t *e, const ds_zone_t *z) {
    uint64_t tags = z->tag_mask;
    if (!e->groups_rt[z->def.group_index].enabled) return 0;
    for (unsigned t = 0; tags; ++t, tags >>= 1) if ((tags & 1) && !e->tag_enabled[t]) return 0;
    return 1;
}

static void zone_now_from(const ds_native_engine_t *e, const ds_zone_t *z, const ds_group_settings_t *g,
                          const ds_group_settings_t *in, zone_now_t *out) {
    const ds_dspreset_sample_t *d = &z->def;
    static const unsigned own_env[4] = {DS_OWN_ATTACK, DS_OWN_DECAY, DS_OWN_SUSTAIN, DS_OWN_RELEASE};
    const float def_env[4] = {d->attack, d->decay, d->sustain, d->release};
    uint64_t tags = z->tag_mask;
    out->gain = d->own_volume * g->volume * in->volume;
    for (unsigned t = 0; tags; ++t, tags >>= 1) if (tags & 1) out->gain *= e->tag_volume[t];
    out->tuning = d->base_tuning + g->tuning + in->tuning;
    out->pan = (d->own_mask & DS_OWN_PAN) ? d->pan : g->has_pan ? g->pan : in->pan;
    out->vel_track = (d->own_mask & DS_OWN_VEL_TRACK) ? d->amp_vel_track : g->has_vel_track ? g->vel_track : in->vel_track;
    for (int i = 0; i < 4; ++i)
        out->env[i] = (d->own_mask & own_env[i]) ? def_env[i] : g->has_env[i] ? g->env[i] : in->env[i];
    for (int i = 0; i < 4; ++i)                                  /* the module's envelope, where set, wins */
        if (e->amp_override[i] >= 0) out->env[i] = e->amp_override[i];
    if (out->pan < -1) out->pan = -1;
    if (out->pan > 1) out->pan = 1;
}

static void zone_now(const ds_native_engine_t *e, const ds_zone_t *z, zone_now_t *out) {
    zone_now_from(e, z, &e->groups_rt[z->def.group_index], &e->instrument_rt, out);
}

static void set_group_value(ds_group_settings_t *g, int target, float v) {
    switch (target) {
    case DS_TARGET_VOLUME: g->volume = v < 0 ? 0 : v; break;
    case DS_TARGET_TUNING: g->tuning = v; break;
    case DS_TARGET_PAN: g->pan = v / 100.0f; g->has_pan = 1; break;
    case DS_TARGET_VEL_TRACK: g->vel_track = v; g->has_vel_track = 1; break;
    case DS_TARGET_ATTACK: case DS_TARGET_DECAY: case DS_TARGET_SUSTAIN: case DS_TARGET_RELEASE:
        g->env[target - DS_TARGET_ATTACK] = v < 0 ? 0 : v; g->has_env[target - DS_TARGET_ATTACK] = 1; break;
    case DS_TARGET_ENABLED: g->enabled = v >= 0.5f; break;
    default: break;
    }
}

/* DecentSampler parameter token -> the <effect> attribute it moves. */
static const char *effect_attribute(const char *token) {
    static const struct { const char *token, *attr; } map[] = {
        {"FX_FILTER_FREQUENCY", "frequency"}, {"FX_FILTER_RESONANCE", "resonance"}, {"FX_FILTER_GAIN", "gain"},
        {"FX_FILTER_Q", "q"}, {"LEVEL", "level"},
        {"FX_CENTER_FREQUENCY", "frequency"}, {"FX_REVERB_WET_LEVEL", "wetLevel"}, {"FX_REVERB_ROOM_SIZE", "roomSize"},
        {"FX_REVERB_DAMPING", "damping"}, {"FX_DELAY_TIME", "delayTime"}, {"FX_FEEDBACK", "feedback"},
        {"FX_WET_LEVEL", "wetLevel"}, {"FX_MIX", "mix"}, {"FX_MOD_RATE", "modRate"}, {"FX_MOD_DEPTH", "modDepth"},
        {"FX_STEREO_OFFSET", "stereoOffset"}, {"FX_DRIVE", "drive"}, {"FX_OUTPUT_LEVEL", "outputLevel"},
        {"FX_BIT_DEPTH", "bitDepth"}, {"FX_DOWNSAMPLE_FACTOR", "downsampleFactor"}};
    for (unsigned i = 0; i < sizeof(map) / sizeof(map[0]); ++i) if (!strcmp(token, map[i].token)) return map[i].attr;
    return NULL;
}

static void set_effect_value(ds_effect_t *fx, const char *token, float v) {
    const char *name;
    if (!strcmp(token, "ENABLED")) { fx->enabled = v >= 0.5f; return; }
    if (!(name = effect_attribute(token))) return;
    for (unsigned i = 0; i < fx->param_count; ++i)
        if (!strcmp(fx->param_names[i], name)) { fx->param_values[i] = v; return; }
    if (fx->param_count < DS_MAX_EFFECT_PARAMS) {          /* a parameter the XML left at its default */
        snprintf(fx->param_names[fx->param_count], sizeof(fx->param_names[0]), "%s", name);
        fx->param_values[fx->param_count++] = v;
    }
}

static void set_modulator_value(ds_modulator_t *m, const char *token, float v) {
    if (!strcmp(token, "MOD_AMOUNT")) m->mod_amount = v;
    else if (!strcmp(token, "FREQUENCY")) m->frequency = v < 0 ? 0 : v;
    else if (!strcmp(token, "ENV_ATTACK")) m->attack = v < 0 ? 0 : v;
    else if (!strcmp(token, "ENV_DECAY")) m->decay = v < 0 ? 0 : v;
    else if (!strcmp(token, "ENV_SUSTAIN")) m->sustain = v;
    else if (!strcmp(token, "ENV_RELEASE")) m->release = v < 0 ? 0 : v;
    else if (!strcmp(token, "DELAY_TIME")) m->delay = v < 0 ? 0 : v;
}

static void control_changed(ds_native_engine_t *e, unsigned index, float value, int depth);

static void apply_binding(ds_native_engine_t *e, const ds_binding_t *b, float in_min, float in_max, float value, int depth) {
    float v = ds_binding_translate(b, in_min, in_max, value);
    switch (b->target) {
    case DS_TARGET_NONE: return;
    case DS_TARGET_CONTROL_VALUE:
        if (depth < 2 && b->position >= 0 && b->position < (int)e->model.control_count) control_changed(e, (unsigned)b->position, v, depth + 1);
        return;
    case DS_TARGET_MODULATOR: {
        int m = b->position < 0 ? 0 : b->position;
        if (m < (int)e->model.modulator_count) set_modulator_value(&e->model.modulators[m], b->name, v);
        return;
    }
    case DS_TARGET_EFFECT:
        for (unsigned i = 0; i < e->model.effect_count; ++i)
            if (b->tag_mask ? (e->model.effects[i].tag_mask & b->tag_mask) != 0 : (int)i == b->effect) {
                set_effect_value(&e->model.effects[i], b->name, v);
                e->fx_dirty[i] = 1;
            }
        return;
    default: break;
    }
    if (b->level == DS_LEVEL_TAG) {
        for (unsigned t = 0; t < e->model.tag_count; ++t) {
            if (!(b->tag_mask & (1ull << t))) continue;
            if (b->target == DS_TARGET_VOLUME) e->tag_volume[t] = v < 0 ? 0 : v;
            else if (b->target == DS_TARGET_ENABLED) e->tag_enabled[t] = v >= 0.5f;
        }
    } else if (b->level == DS_LEVEL_GROUP) {
        for (unsigned g = 0; g < e->model.group_count; ++g)
            if (b->tag_mask ? (e->groups_rt[g].tag_mask & b->tag_mask) != 0 : (int)g == (b->position < 0 ? 0 : b->position))
                set_group_value(&e->groups_rt[g], b->target, v);
    } else if (b->level == DS_LEVEL_INSTRUMENT) {
        set_group_value(&e->instrument_rt, b->target, v);
    }
}

static void control_changed(ds_native_engine_t *e, unsigned index, float value, int depth) {
    const ds_control_t *c = &e->model.controls[index];
    if (value < c->min) value = c->min;
    if (value > c->max) value = c->max;
    if (c->integer) value = floorf(value + 0.5f);
    e->control_value[index] = value;
    if (c->kind == DS_CONTROL_KNOB) {
        for (unsigned i = 0; i < c->binding_count; ++i)
            apply_binding(e, &e->model.bindings[c->first_binding + i], c->min, c->max, value, depth);
    } else if (c->choice_count) {
        const ds_choice_t *ch = &e->model.choices[c->first_choice + (unsigned)value];
        for (unsigned i = 0; i < ch->binding_count; ++i)
            apply_binding(e, &e->model.bindings[ch->first_binding + i], c->min, c->max, value, depth);
    }
}

int ds_native_engine_preset_envelope(const ds_native_engine_t *e, float out[4]) {
    for (unsigned i = 0; e && i < e->zone_count; ++i) {
        const ds_zone_t *z = &e->zones[i];
        zone_now_t now;
        if (z->source < 0 || !z->def.amp_env_enabled || z->def.trigger != DS_TRIGGER_ATTACK) continue;
        zone_now_from(e, z, &e->groups_rt[z->def.group_index], &e->instrument_rt, &now);
        for (int k = 0; k < 4; ++k) out[k] = now.env[k];
        return 1;
    }
    return 0;
}

void ds_native_engine_set_control(ds_native_engine_t *e, unsigned index, float value) {
    if (e && index < e->model.control_count) control_changed(e, index, value, 0);
}

/* ---- modulators ---------------------------------------------------------
 * An LFO swings -1..1 around a neutral 0 (CS-20M's vibrato maps it onto
 * -12..+12 semitones at depth 0.04, which only makes sense two-sided); an
 * envelope, a CC and velocity run 0..1. modAmount scales that value BEFORE the
 * binding translates it, as the guide describes depth. Values are taken once
 * per block, which at 2.9 ms is far finer than any of them move. */

static float clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

static void mod_start(const ds_modulator_t *m, ds_mod_state_t *st) {
    st->phase = 0;
    st->delay_left = m->delay;
    st->level = m->attack > 0 ? 0.0f : 1.0f;
    st->stage = m->attack > 0 ? DS_ENV_ATTACK : DS_ENV_DECAY;
}

static void mod_release(ds_mod_state_t *st) { if (st->stage != DS_ENV_DONE) st->stage = DS_ENV_RELEASE; }

static float mod_value(const ds_native_engine_t *e, const ds_modulator_t *m, const ds_mod_state_t *st, float velocity) {
    float raw = 0, p = st->phase;
    switch (m->kind) {
    case DS_MOD_LFO:
        if (st->delay_left > 0) return 0;
        raw = m->shape == DS_LFO_SQUARE ? (p < 0.5f ? 1.0f : -1.0f) :
              m->shape == DS_LFO_SAW ? 2.0f * p - 1.0f :
              m->shape == DS_LFO_TRIANGLE ? 1.0f - 4.0f * fabsf(p - 0.5f) :
              sinf(2.0f * (float)M_PI * p);
        break;
    case DS_MOD_ENVELOPE: raw = st->stage == DS_ENV_DONE ? 0 : st->level; break;
    case DS_MOD_CC: raw = m->cc >= 0 && m->cc < 128 ? e->cc_value[m->cc] : 0; break;
    default: raw = velocity; break;
    }
    return raw * m->mod_amount;
}

static void mod_advance(const ds_modulator_t *m, ds_mod_state_t *st, unsigned frames, float rate) {
    float dt = (float)frames / rate, sustain = clamp01(m->sustain);
    if (m->kind == DS_MOD_LFO) {
        if (st->delay_left > 0) { st->delay_left -= dt; return; }
        st->phase += m->frequency * dt;
        st->phase -= floorf(st->phase);
    } else if (m->kind == DS_MOD_ENVELOPE) {
        switch (st->stage) {
        case DS_ENV_ATTACK:
            st->level += m->attack > 0 ? dt / m->attack : 1.0f;
            if (st->level >= 1.0f) { st->level = 1.0f; st->stage = DS_ENV_DECAY; }
            break;
        case DS_ENV_DECAY:
            st->level = sustain + (st->level - sustain) * (m->decay > 0 ? expf(logf(SILENT) * dt / m->decay) : 0.0f);
            if (fabsf(st->level - sustain) < 1e-4f) { st->level = sustain; st->stage = DS_ENV_SUSTAIN; }
            break;
        case DS_ENV_SUSTAIN: st->level = sustain; break;          /* a Sustain knob reaches held notes */
        case DS_ENV_RELEASE:
            st->level *= m->release > 0 ? expf(logf(SILENT) * dt / m->release) : 0.0f;
            if (st->level < SILENT) { st->level = 0; st->stage = DS_ENV_DONE; }
            break;
        default: break;
        }
    }
}

static float mod_apply(int behavior, float base, float t, float neutral) {
    switch (behavior) {
    case DS_MODB_ADD: return base + t;
    case DS_MODB_MULTIPLY: return base * t;
    case DS_MODB_MODULATE: return base + (t - neutral);
    default: return t;
    }
}

/* The translated value `b` delivers for a modulator value, and for neutral. */
static void mod_translate(const ds_modulator_t *m, const ds_binding_t *b, float value, float *t, float *neutral) {
    float lo = m->kind == DS_MOD_LFO ? -1.0f : 0.0f;
    *t = ds_binding_translate(b, lo, 1.0f, value);
    *neutral = ds_binding_translate(b, lo, 1.0f, 0.0f);
}

/* Every modulator binding that reaches a note's volume, pitch or pan, applied to
 * copies of its group's and the instrument's settings. */
static void voice_mod_settings(const ds_native_engine_t *e, const ds_voice_t *v, const float *values,
                               ds_group_settings_t *g, ds_group_settings_t *in) {
    int group = v->zone->def.group_index;
    for (unsigned k = 0; k < e->model.modulator_count; ++k) {
        const ds_modulator_t *m = &e->model.modulators[k];
        for (unsigned i = 0; i < m->binding_count; ++i) {
            const ds_binding_t *b = &e->model.bindings[m->first_binding + i];
            ds_group_settings_t *target;
            float t, n;
            if (b->target != DS_TARGET_VOLUME && b->target != DS_TARGET_TUNING && b->target != DS_TARGET_PAN) continue;
            if (b->level == DS_LEVEL_INSTRUMENT) target = in;
            else if (b->level == DS_LEVEL_GROUP &&
                     (b->tag_mask ? (e->groups_rt[group].tag_mask & b->tag_mask) != 0 : group == (b->position < 0 ? 0 : b->position)))
                target = g;
            else continue;
            mod_translate(m, b, values[k], &t, &n);
            if (b->target == DS_TARGET_VOLUME) {
                target->volume = mod_apply(b->mod_behavior, target->volume, t, n);
                if (target->volume < 0) target->volume = 0;
            } else if (b->target == DS_TARGET_TUNING) {
                target->tuning = mod_apply(b->mod_behavior, target->tuning, t, n);
            } else {                                                /* pan, in DecentSampler's -100..100 */
                target->pan = mod_apply(b->mod_behavior, target->pan * 100.0f, t, n) / 100.0f;
                target->has_pan = 1;
            }
        }
    }
}

static float effect_param_default(const ds_effect_t *fx, const char *name) {
    if (!strcmp(name, "frequency")) return !strcmp(fx->type, "peak") || !strcmp(fx->type, "notch") ? 10000 : 22000;
    if (!strcmp(name, "resonance") || !strcmp(name, "q")) return 0.7f;
    if (!strcmp(name, "gain")) return 1;
    return 0;
}

/* Has any setting moved since `built`? A relative change under 1e-4 does not
 * count: the tail of an exponential decay would otherwise rebuild every block
 * for an inaudible difference. */
static int fx_settings_moved(const ds_effect_t *fx, const ds_fx_built_t *built) {
    if (!built->valid || built->count != fx->param_count || built->enabled != fx->enabled) return 1;
    for (unsigned i = 0; i < fx->param_count; ++i) {
        float a = fx->param_values[i], b = built->values[i];
        if (fabsf(a - b) > 1e-4f * (fabsf(a) > fabsf(b) ? fabsf(a) : fabsf(b))) return 1;
    }
    return 0;
}

/* Coefficients for effect `x` with the modulators applied, rebuilt only when a
 * setting moved. Only global modulators reach an instrument effect
 * (`with_voice` = 0). */
static void modulated_effect(ds_native_engine_t *e, unsigned x, const float *values, int with_voice,
                             ds_fx_coeffs_t *out, ds_fx_built_t *built) {
    ds_effect_t fx = e->model.effects[x];
    for (unsigned k = 0; k < e->model.modulator_count; ++k) {
        const ds_modulator_t *m = &e->model.modulators[k];
        if (m->voice_scope && !with_voice) continue;
        for (unsigned i = 0; i < m->binding_count; ++i) {
            const ds_binding_t *b = &e->model.bindings[m->first_binding + i];
            const char *name;
            float t, n;
            if (b->target != DS_TARGET_EFFECT) continue;
            if (b->tag_mask ? !(fx.tag_mask & b->tag_mask) : b->effect != (int)x) continue;
            if (!(name = effect_attribute(b->name))) continue;
            mod_translate(m, b, values[k], &t, &n);
            set_effect_value(&fx, b->name, mod_apply(b->mod_behavior, ds_fx_param(&fx, name, effect_param_default(&fx, name)), t, n));
        }
    }
    if (!fx_settings_moved(&fx, built)) return;
    ds_fx_prepare(out, &fx, (float)e->output_rate);
    memcpy(built->values, fx.param_values, sizeof(built->values));
    built->count = fx.param_count;
    built->enabled = fx.enabled;
    built->valid = 1;
    e->fx_rebuilds++;
}

static ds_voice_t *allocate_voice(ds_native_engine_t *e) {
    ds_voice_t *best = NULL;
    for (int i = 0; i < DS_MAX_VOICES; ++i) if (!e->voices[i].active) return &e->voices[i];
    /* Steal: a releasing voice first (quietest), else the oldest. */
    for (int i = 0; i < DS_MAX_VOICES; ++i) {
        ds_voice_t *v = &e->voices[i];
        if (v->env_stage == DS_ENV_RELEASE && (!best || v->env_level < best->env_level)) best = v;
    }
    if (best) return best;
    best = &e->voices[0];
    for (int i = 1; i < DS_MAX_VOICES; ++i) if (e->voices[i].age < best->age) best = &e->voices[i];
    return best;
}

static void start_voice(ds_native_engine_t *e, const ds_zone_t *z, int note, int velocity, int one_shot) {
    ds_voice_t *v = allocate_voice(e);
    const ds_source_t *s = &e->sources[z->source];
    const ds_dspreset_sample_t *d = &z->def;
    zone_now_t now;
    uint32_t zone_index = (uint32_t)(z - e->zones);

    zone_now(e, z, &now);
    v->zone = z; v->src = s;
    v->pos = (double)z->start;
    v->vel = velocity / 127.0f;
    v->note_id = e->note_counter;
    v->choked = 0;
    v->fx_count = 0;
    for (unsigned x = 0; x < e->model.effect_count && v->fx_count < DS_VOICE_FX; ++x)
        if (e->model.effects[x].group == d->group_index) {
            v->fx_index[v->fx_count] = (unsigned char)x;
            memset(&v->fx_state[v->fx_count], 0, sizeof(v->fx_state[0]));
            v->fx_built[v->fx_count].valid = 0;                /* a new note builds its own */
            v->fx_count++;
        }
    for (unsigned k = 0; k < e->model.modulator_count; ++k)
        if (e->model.modulators[k].voice_scope) mod_start(&e->model.modulators[k], &v->mods[k]);
    v->note = note; v->velocity = velocity;
    v->key_down = !one_shot; v->one_shot = one_shot; v->sustained = 0;
    v->age = ++e->age_counter;
    v->underruns = 0;
    if (!d->amp_env_enabled) {
        v->env_stage = DS_ENV_SUSTAIN; v->env_level = 1.0f; v->env_sustain = 1.0f;
        v->env_release_coef = coef_for(0.005f, e->output_rate);
    } else {
        float attack = now.env[0], decay = now.env[1], sustain = now.env[2], release = now.env[3];
        v->env_sustain = sustain < 0 ? 0 : sustain > 1 ? 1 : sustain;
        v->env_attack_step = attack > 0 ? 1.0f / (attack * (float)e->output_rate) : 1.0f;
        v->env_decay_coef = coef_for(decay, e->output_rate);
        v->env_release_coef = coef_for(release > 0.002f ? release : 0.002f, e->output_rate);
        v->env_level = 0.0f;
        v->env_stage = DS_ENV_ATTACK;
    }
    v->generation++;
    if (z->streams) {
        uint64_t ring_from = z->start >= s->head_frames ? z->start : s->head_frames;
        atomic_store_explicit(&v->consumed, (uint32_t)z->start, memory_order_relaxed);
        atomic_store_explicit(&v->stream, pack(v->generation, zone_index + 1, (uint32_t)ring_from),
                              memory_order_release);
    } else {
        atomic_store_explicit(&v->stream, pack(v->generation, 0, 0), memory_order_release);
    }
    v->active = 1;
}

static void release_voice(ds_native_engine_t *e, ds_voice_t *v) {
    if (v->env_stage == DS_ENV_DONE) return;
    if (v->zone->def.amp_env_enabled) {
        zone_now_t now;
        zone_now(e, v->zone, &now);                     /* a Release knob reaches held notes */
        v->env_release_coef = coef_for(now.env[3] > 0.002f ? now.env[3] : 0.002f, e->output_rate);
    }
    /* Released before it rendered a sample (note-on and note-off in one block,
     * a short sequenced note): start the release from the attack's first step,
     * or it decays from zero and the note is never heard at all. */
    if (v->env_stage == DS_ENV_ATTACK && v->env_level <= 0.0f)
        v->env_level = v->env_attack_step < 1.0f ? v->env_attack_step : 1.0f;
    v->env_stage = DS_ENV_RELEASE;
    for (unsigned k = 0; k < e->model.modulator_count; ++k)
        if (e->model.modulators[k].voice_scope) mod_release(&v->mods[k]);
}

static int zone_matches(const ds_native_engine_t *e, const ds_zone_t *z, int note, int velocity, int trigger) {
    return z->source >= 0 && z->def.trigger == trigger && zone_enabled(e, z) &&
           note >= z->def.lo_note && note <= z->def.hi_note &&
           velocity >= z->def.lo_vel && velocity <= z->def.hi_vel;
}

/* Fades every voice of note `id` out in 5 ms and stops counting it. */
static void choke_note(ds_native_engine_t *e, uint32_t id) {
    for (int i = 0; i < DS_MAX_VOICES; ++i) {
        ds_voice_t *v = &e->voices[i];
        if (!v->active || v->one_shot || v->note_id != id) continue;
        if (v->env_stage == DS_ENV_ATTACK && v->env_level <= 0.0f) v->env_level = v->env_attack_step < 1.0f ? v->env_attack_step : 1.0f;
        v->env_release_coef = coef_for(0.005f, e->output_rate);
        v->env_stage = DS_ENV_RELEASE;
        v->choked = 1;
    }
}

/* Make room for one more note under the limit: while `limit` notes are
 * already sounding, fade out the oldest — a released one if there is any. */
static void enforce_note_limit(ds_native_engine_t *e) {
    if (e->poly_limit <= 0) return;
    for (;;) {
        uint32_t ids[DS_MAX_VOICES];
        int held[DS_MAX_VOICES], count = 0, victim = -1;
        for (int i = 0; i < DS_MAX_VOICES; ++i) {
            const ds_voice_t *v = &e->voices[i];
            int k;
            if (!v->active || v->one_shot || v->choked) continue;
            for (k = 0; k < count && ids[k] != v->note_id; ++k) {}
            if (k == count) { ids[count] = v->note_id; held[count] = 0; count++; }
            if (v->key_down || v->sustained) held[k] = 1;
        }
        if (count < e->poly_limit) return;
        for (int k = 0; k < count; ++k)                     /* oldest released note... */
            if (!held[k] && (victim < 0 || ids[k] < ids[victim])) victim = k;
        if (victim < 0)                                     /* ...else the oldest held one */
            for (int k = 0; k < count; ++k) if (victim < 0 || ids[k] < ids[victim]) victim = k;
        choke_note(e, ids[victim]);
    }
}

static void trigger_zones(ds_native_engine_t *e, int note, int velocity, int trigger) {
    /* Only a note-on advances round robin: the note-off's release-trigger pass
     * must not, or every hit steps by two and half the recordings never play. */
    uint32_t counter = trigger == DS_TRIGGER_ATTACK ? e->rr_counter[note]++ : e->rr_counter[note] - 1, random;
    e->rng = e->rng * 1664525u + 1013904223u;
    random = e->rng >> 8;
    memset(e->group_len, 0, e->group_count);
    for (unsigned i = 0; i < e->zone_count; ++i) {
        const ds_zone_t *z = &e->zones[i];
        if (zone_matches(e, z, note, velocity, trigger) && z->def.seq_position > e->group_len[z->def.group_index])
            e->group_len[z->def.group_index] = (unsigned char)(z->def.seq_position > 255 ? 255 : z->def.seq_position);
    }
    for (unsigned i = 0; i < e->zone_count; ++i) {
        const ds_zone_t *z = &e->zones[i];
        unsigned len;
        if (!zone_matches(e, z, note, velocity, trigger)) continue;
        len = e->group_len[z->def.group_index] ? e->group_len[z->def.group_index] : 1;
        if (z->def.seq_mode == DS_SEQ_ROUND_ROBIN && (int)(counter % len) + 1 != z->def.seq_position) continue;
        if (z->def.seq_mode == DS_SEQ_RANDOM && (int)(random % len) + 1 != z->def.seq_position) continue;
        start_voice(e, z, note, velocity, trigger == DS_TRIGGER_RELEASE);
    }
}

void ds_native_engine_note_on(ds_native_engine_t *e, int note, int velocity) {
    if (!e || note < 0 || note > 127 || velocity < 0 || velocity > 127) return;
    if (!velocity) { ds_native_engine_note_off(e, note); return; }
    if (!e->note_velocity[note] && e->keys_held++ == 0)     /* a global envelope starts with the first key */
        for (unsigned k = 0; k < e->model.modulator_count; ++k)
            if (!e->model.modulators[k].voice_scope && e->model.modulators[k].kind == DS_MOD_ENVELOPE)
                mod_start(&e->model.modulators[k], &e->mod_global[k]);
    e->note_velocity[note] = velocity;
    enforce_note_limit(e);
    e->note_counter++;
    trigger_zones(e, note, velocity, DS_TRIGGER_ATTACK);
}

void ds_native_engine_note_off(ds_native_engine_t *e, int note) {
    if (!e || note < 0 || note > 127) return;
    for (int i = 0; i < DS_MAX_VOICES; ++i) {
        ds_voice_t *v = &e->voices[i];
        if (!v->active || !v->key_down || v->note != note) continue;
        v->key_down = 0;
        if (e->sustain_pedal) v->sustained = 1;
        else release_voice(e, v);
    }
    if (e->note_velocity[note]) {
        trigger_zones(e, note, e->note_velocity[note], DS_TRIGGER_RELEASE);
        if (e->keys_held && --e->keys_held == 0)                /* ...and releases with the last */
            for (unsigned k = 0; k < e->model.modulator_count; ++k)
                if (!e->model.modulators[k].voice_scope) mod_release(&e->mod_global[k]);
    }
    e->note_velocity[note] = 0;
}

void ds_native_engine_cc(ds_native_engine_t *e, int cc, int value) {
    if (!e) return;
    if (cc >= 0 && cc < 128) e->cc_value[cc] = clamp01(value / 127.0f);
    for (unsigned m = 0; m < e->model.cc_count; ++m) {
        const ds_cc_map_t *map = &e->model.ccs[m];
        if (map->cc != cc) continue;
        for (unsigned i = 0; i < map->binding_count; ++i)
            apply_binding(e, &e->model.bindings[map->first_binding + i], 0, 127, (float)value, 0);
    }
    if (cc == 64) {
        int down = value >= 64;
        if (e->sustain_pedal && !down)
            for (int i = 0; i < DS_MAX_VOICES; ++i)
                if (e->voices[i].active && e->voices[i].sustained) { e->voices[i].sustained = 0; release_voice(e, &e->voices[i]); }
        e->sustain_pedal = down;
    } else if (cc == 120) {
        for (int i = 0; i < DS_MAX_VOICES; ++i) e->voices[i].active = 0;
    } else if (cc == 123) {
        for (int i = 0; i < DS_MAX_VOICES; ++i) if (e->voices[i].active) { e->voices[i].key_down = 0; release_voice(e, &e->voices[i]); }
    }
}

void ds_native_engine_pitch_bend(ds_native_engine_t *e, int value14) {
    if (e) e->bend_ratio = pow(2.0, ((value14 - 8192) / 8192.0) * 2.0 / 12.0);
}

/* One frame from the resident head or the voice's ring; 0 when the worker
 * has not produced it yet. */
static inline int fetch(const ds_voice_t *v, uint64_t vf, uint32_t produced, float *l, float *r) {
    const ds_zone_t *z = v->zone;
    const ds_source_t *s = v->src;
    unsigned ch = s->file.channels;
    uint64_t f = map_frame(z, vf);
    const float *frame;
    if (!z->loop && vf >= z->end) { *l = *r = 0; return 1; }
    if (f < s->head_frames) frame = s->head + f * ch;
    else if (vf < produced) frame = v->ring + (vf & RING_MASK) * ch;
    else return 0;
    *l = frame[0];
    *r = ch > 1 ? frame[1] : frame[0];
    return 1;
}

static void render_voice(ds_native_engine_t *e, ds_voice_t *v, float *out, unsigned frames, const zone_now_t *settings) {
    uint32_t produced = packed_produced(atomic_load_explicit(&v->stream, memory_order_acquire));
    double inc;
    {   /* volume, pan and pitch follow the controls (and modulators) while the note sounds */
        zone_now_t now = *settings;
        float gain;
        gain = now.gain * (1.0f - now.vel_track + now.vel_track * v->vel);
        v->gain_l = gain * (now.pan > 0 ? 1.0f - now.pan : 1.0f);
        v->gain_r = gain * (now.pan < 0 ? 1.0f + now.pan : 1.0f);
        v->inc = ((double)v->src->file.sample_rate / e->output_rate) *
                 pow(2.0, (v->note - v->zone->def.root_note + now.tuning) / 12.0);
        /* A Sustain moved while the note is held: glide there (never a jump). */
        if (v->zone->def.amp_env_enabled && (v->env_stage == DS_ENV_DECAY || v->env_stage == DS_ENV_SUSTAIN)) {
            float target = now.env[2] < 0 ? 0 : now.env[2] > 1 ? 1 : now.env[2];
            if (fabsf(target - v->env_sustain) > 1e-4f) {
                v->env_sustain = target;
                if (v->env_stage == DS_ENV_SUSTAIN) {
                    float glide = coef_for(0.02f, e->output_rate);
                    v->env_stage = DS_ENV_DECAY;
                    if (v->env_decay_coef < glide) v->env_decay_coef = glide;
                }
            }
        }
    }
    inc = v->inc * e->bend_ratio;
    for (unsigned i = 0; i < frames; ++i) {
        uint64_t v0 = (uint64_t)v->pos;
        float frac = (float)(v->pos - (double)v0), l0, r0, l1, r1, level;
        if (!v->zone->loop && v0 >= v->zone->end) { v->active = 0; break; }
        if (!fetch(v, v0, produced, &l0, &r0) || !fetch(v, v0 + 1, produced, &l1, &r1)) {
            v->underruns++;             /* the worker is behind: hold, never skip */
            continue;
        }
        switch (v->env_stage) {
        case DS_ENV_ATTACK:
            v->env_level += v->env_attack_step;
            if (v->env_level >= 1.0f) { v->env_level = 1.0f; v->env_stage = DS_ENV_DECAY; }
            break;
        case DS_ENV_DECAY:
            v->env_level = v->env_sustain + (v->env_level - v->env_sustain) * v->env_decay_coef;
            /* either way: a Sustain raised while held glides UP through this stage */
            if (fabsf(v->env_level - v->env_sustain) < 1e-4f) { v->env_level = v->env_sustain; v->env_stage = DS_ENV_SUSTAIN; }
            break;
        case DS_ENV_SUSTAIN:
            if (v->env_level < SILENT) v->env_stage = DS_ENV_DONE;
            break;
        case DS_ENV_RELEASE:
            v->env_level *= v->env_release_coef;
            if (v->env_level < SILENT) v->env_stage = DS_ENV_DONE;
            break;
        default: break;
        }
        if (v->env_stage == DS_ENV_DONE) { v->active = 0; break; }
        level = v->env_level;
        out[2 * i] += (l0 + (l1 - l0) * frac) * v->gain_l * level;
        out[2 * i + 1] += (r0 + (r1 - r0) * frac) * v->gain_r * level;
        v->pos += inc;
    }
    if (v->zone->streams) atomic_store_explicit(&v->consumed, (uint32_t)v->pos, memory_order_release);
    if (!v->active) atomic_store_explicit(&v->stream, pack(v->generation, 0, 0), memory_order_release);
}

void ds_native_engine_render(ds_native_engine_t *e, float *out_lr, unsigned frames) {
    uint32_t underruns = 0;
    if (!e || !out_lr) return;
    for (unsigned x = 0; x < e->model.effect_count; ++x)
        if (e->fx_dirty[x]) { ds_fx_prepare(&e->fx_coeffs[x], &e->model.effects[x], (float)e->output_rate); e->fx_dirty[x] = 0; }
    for (unsigned k = 0; k < e->model.modulator_count; ++k) {         /* shared modulators: once per block */
        const ds_modulator_t *m = &e->model.modulators[k];
        if (m->voice_scope) continue;
        e->mod_global_value[k] = mod_value(e, m, &e->mod_global[k], 0);
        mod_advance(m, &e->mod_global[k], frames, (float)e->output_rate);
    }
    for (int i = 0; i < DS_MAX_VOICES; ++i) {
        ds_voice_t *v = &e->voices[i];
        zone_now_t now;
        float values[DS_MAX_MODULATORS];
        if (!v->active) continue;
        if (e->model.modulator_count) {
            ds_group_settings_t g = e->groups_rt[v->zone->def.group_index], in = e->instrument_rt;
            for (unsigned k = 0; k < e->model.modulator_count; ++k) {
                const ds_modulator_t *m = &e->model.modulators[k];
                values[k] = m->voice_scope ? mod_value(e, m, &v->mods[k], v->vel) : e->mod_global_value[k];
            }
            voice_mod_settings(e, v, values, &g, &in);
            zone_now_from(e, v->zone, &g, &in, &now);
        } else {
            zone_now(e, v->zone, &now);
        }
        const ds_fx_coeffs_t *chain[DS_VOICE_FX];
        int filtering = 0;
        for (unsigned k = 0; k < v->fx_count; ++k) {                  /* this block's coefficients first */
            unsigned x = v->fx_index[k];
            chain[k] = &e->fx_coeffs[x];
            if (e->fx_modulated[x]) { modulated_effect(e, x, values, 1, &v->fx_live[k], &v->fx_built[k]); chain[k] = &v->fx_live[k]; }
            /* (a reverb inside a note is not run: see the load) */
            filtering |= chain[k]->kind != DS_FX_BYPASS && chain[k]->kind != DS_FX_UNSUPPORTED && chain[k]->kind != DS_FX_REVERB;
        }
        if (!filtering || frames > 256) {
            /* Nothing to run this block (a filter swept wide open): straight out.
             * A filter's state is simply held, as a bypassed one would. */
            render_voice(e, v, out_lr, frames, &now);
        } else {
            /* A MONO note is filtered once and panned after: the effects are
             * linear and per channel, and a block's pan is constant, so this is
             * exact — and halves the cost of the notes that dominate it. */
            float note[2 * 256];
            int mono = v->src->file.channels == 1;
            zone_now_t centred = now;
            if (mono) centred.pan = 0;
            memset(note, 0, frames * 2 * sizeof(float));
            render_voice(e, v, note, frames, &centred);
            for (unsigned k = 0; k < v->fx_count; ++k) {
                if (mono) ds_fx_process_left(chain[k], &v->fx_state[k], note, frames);
                else ds_fx_process(chain[k], &v->fx_state[k], note, frames);
            }
            if (mono) {
                float pl = now.pan > 0 ? 1.0f - now.pan : 1.0f, pr = now.pan < 0 ? 1.0f + now.pan : 1.0f;
                for (unsigned k = 0; k < frames; ++k) { out_lr[2 * k] += note[2 * k] * pl; out_lr[2 * k + 1] += note[2 * k] * pr; }
            } else {
                for (unsigned k = 0; k < frames * 2; ++k) out_lr[k] += note[k];
            }
        }
        for (unsigned k = 0; k < e->model.modulator_count; ++k)
            if (e->model.modulators[k].voice_scope) mod_advance(&e->model.modulators[k], &v->mods[k], frames, (float)e->output_rate);
        underruns += v->underruns;
    }
    for (unsigned x = 0; x < e->model.effect_count; ++x) {
        const ds_fx_coeffs_t *c = &e->fx_coeffs[x];
        if (e->model.effects[x].group >= 0) continue;
        if (e->fx_modulated[x]) { modulated_effect(e, x, e->mod_global_value, 0, &e->fx_live[x], &e->fx_live_built[x]); c = &e->fx_live[x]; }
        if (c->kind == DS_FX_REVERB) {
            if (e->reverb[x]) { ds_reverb_set(e->reverb[x], c->room, c->damping, c->wet); ds_reverb_process(e->reverb[x], out_lr, frames); }
        } else {
            ds_fx_process(c, &e->fx_state[x], out_lr, frames);
        }
    }
    if (underruns) atomic_fetch_add_explicit(&e->underruns, underruns, memory_order_relaxed);
    for (int i = 0; i < DS_MAX_VOICES; ++i) e->voices[i].underruns = 0;
}

unsigned ds_native_engine_active_voices(const ds_native_engine_t *e) {
    unsigned n = 0;
    for (int i = 0; e && i < DS_MAX_VOICES; ++i) n += e->voices[i].active != 0;
    return n;
}

/* ---- worker ------------------------------------------------------------- */

static void fill(ds_native_engine_t *e, ds_voice_t *v, int fd, const ds_zone_t *z, uint64_t from, unsigned count) {
    const ds_source_t *s = &e->sources[z->source];
    ds_wav_source_t file = s->file;
    unsigned ch = s->file.channels;
    char error[64];
    file.fd = fd;
    while (count) {
        uint64_t f = map_frame(z, from);
        uint64_t limit = z->loop && f < z->loop_end ? z->loop_end : z->end;
        unsigned run = count, slot = (unsigned)(from & RING_MASK);
        float *dst = v->ring + (size_t)slot * ch;
        if (run > limit - f) run = (unsigned)(limit - f);
        if (run > DS_RING_FRAMES - slot) run = DS_RING_FRAMES - slot;
        if (f < s->head_frames) {
            if (run > s->head_frames - f) run = (unsigned)(s->head_frames - f);
            memcpy(dst, s->head + f * ch, (size_t)run * ch * sizeof(float));
        } else if (fd < 0 || ds_wav_source_read_frames(&file, f, dst, run, error, sizeof(error)) != (int)run) {
            memset(dst, 0, (size_t)run * ch * sizeof(float));
        }
        from += run;
        count -= run;
    }
}

unsigned ds_native_engine_service(ds_native_engine_t *e) {
    unsigned total = 0;
    if (!e) return 0;
    for (int i = 0; i < DS_MAX_VOICES; ++i) {
        ds_voice_t *v = &e->voices[i];
        uint64_t word = atomic_load_explicit(&v->stream, memory_order_acquire);
        uint32_t zone_plus_one = packed_zone(word), produced, consumed;
        const ds_zone_t *z;
        uint64_t space, count;
        if (!zone_plus_one) continue;
        z = &e->zones[zone_plus_one - 1];
        produced = packed_produced(word);
        consumed = atomic_load_explicit(&v->consumed, memory_order_acquire);
        if (!z->loop && produced >= z->end) continue;
        space = (uint64_t)consumed + DS_RING_FRAMES - produced;
        if (consumed > produced || space < 1024) continue;
        count = space < DS_FILL_FRAMES ? space : DS_FILL_FRAMES;
        if (!z->loop && produced + count > z->end) count = z->end - produced;
        /* The voice's own descriptor, reopened when it starts a new note. */
        if (e->stream_key[i] != (word >> 32)) {
            if (e->stream_fd[i] >= 0 && e->stream_key[i]) close(e->stream_fd[i]);
            e->stream_fd[i] = open(e->source_paths[z->source], O_RDONLY);
            e->stream_key[i] = word >> 32;
        }
        fill(e, v, e->stream_fd[i], z, produced, (unsigned)count);
        /* Publish only if the voice was not restarted meanwhile. */
        if (atomic_compare_exchange_strong_explicit(&v->stream, &word,
                                                    (word & ~0xffffffffull) | (uint32_t)(produced + count),
                                                    memory_order_release, memory_order_relaxed))
            total += (unsigned)count;
    }
    return total;
}
