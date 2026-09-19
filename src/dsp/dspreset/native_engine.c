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
static uint64_t map_frame(const ds_bounds_t *b, uint64_t v) {
    if (b->loop && v >= b->loop_end)
        return b->loop_start + (v - b->loop_start) % (b->loop_end - b->loop_start);
    return v;
}

/* A loop crossfade: over the last `xf` frames before the loop end, the audio
 * as far BEFORE the loop start fades in as the end fades out, so the loop
 * point meets what came before it. 0 outside that stretch. */
static inline int xf_gains(const ds_bounds_t *b, uint64_t f, float *out_gain, float *in_gain) {
    float t;
    if (!b->xf || !b->loop || f >= b->loop_end || f < b->loop_end - b->xf) return 0;
    t = (float)(f - (b->loop_end - b->xf)) / (float)b->xf;
    if (b->xf_equal_power) { *out_gain = cosf(t * (float)M_PI_2); *in_gain = sinf(t * (float)M_PI_2); }
    else { *out_gain = 1.0f - t; *in_gain = t; }
    return 1;
}

static float random_next(uint32_t *rng);

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

/* Frame bounds from the preset's numbers (-1 = not set; ends as written, i.e.
 * the last frame played) against the file. */
static void compute_bounds(const ds_source_t *s, int64_t start, int64_t end, int loop_enabled,
                           int64_t loop_start, int64_t loop_end, int64_t xf, int equal_power, ds_bounds_t *b) {
    uint64_t frames = s->file.frame_count;
    int loop_on;
    b->start = start >= 0 ? (uint64_t)start : 0;
    b->end = end >= 0 ? (uint64_t)end + 1 : frames;          /* DS `end` is the last frame played */
    if (b->end > frames) b->end = frames;
    if (b->start >= b->end) b->start = 0;
    /* An explicit loopEnabled wins; otherwise a loop the file carries is used,
     * as DecentSampler does with embedded markers. */
    loop_on = loop_enabled == 1 || (loop_enabled == -1 && s->file.has_loop);
    b->loop_start = loop_start >= 0 ? (uint64_t)loop_start : s->file.has_loop ? s->file.loop_start : b->start;
    b->loop_end = loop_end >= 0 ? (uint64_t)loop_end + 1 : s->file.has_loop ? s->file.loop_end + 1 : b->end;
    if (b->loop_end > frames) b->loop_end = frames;
    b->loop = loop_on && b->loop_end > b->loop_start + 1 && b->loop_end > b->start;
    /* The fade needs as much audio before the loop start as it is long: a
     * loop starting at the file's start (CS-20M, DS The Synths) has none, so
     * it keeps its plain loop point. */
    b->xf = b->loop && xf > 0 ? (uint64_t)xf : 0;
    if (b->xf > 65536) b->xf = 65536;
    if (b->xf) {
        uint64_t before = b->loop_start > b->start ? b->loop_start - b->start : 0;
        if (b->xf > before) b->xf = before;
        if (b->xf > b->loop_end - b->loop_start) b->xf = b->loop_end - b->loop_start;
    }
    b->xf_equal_power = equal_power;
    if (s->resident) b->streams = 0;
    else b->streams = (b->loop ? b->loop_end : b->end) > s->head_frames || b->start >= s->head_frames;
}

static void resolve_bounds(ds_zone_t *z, const ds_source_t *s) {
    const ds_dspreset_sample_t *d = &z->def;
    compute_bounds(s, d->start, d->end, d->loop_enabled, d->loop_start, d->loop_end,
                   d->loop_crossfade, d->loop_crossfade_equal_power, &z->b);
}

/* The longest echo delay effect `x` can reach, in seconds: its own time and
 * offset, or DecentSampler's full range (20 s, offset 10 s) when a control or
 * modulator can move either. */
static float longest_delay(const ds_preset_model_t *m, unsigned x) {
    const ds_effect_t *fx = &m->effects[x];
    float time = ds_fx_param(fx, "delayTime", ds_fx_default("delay", "delayTime"));
    float offset = ds_fx_param(fx, "stereoOffset", 0);
    for (unsigned i = 0; i < m->binding_count; ++i) {
        const ds_binding_t *b = &m->bindings[i];
        if (b->target != DS_TARGET_EFFECT || (strcmp(b->name, "FX_DELAY_TIME") && strcmp(b->name, "FX_STEREO_OFFSET"))) continue;
        if (b->tag_mask ? (fx->tag_mask & b->tag_mask) != 0 : b->effect == (int)x) { time = 20; offset = 10; break; }
    }
    if (time < 0) time = 0;
    if (time > 20) time = 20;
    if (offset < 0) offset = -offset;
    if (offset > 10) offset = 10;
    return time + 0.5f * offset;
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
        e->zones[i].silenced_by = ds_preset_model_tag_mask(&e->model, e->zones[i].def.silenced_by);
        e->zones[i].live_volume = 1.0f;
        e->zones[i].live_enabled = 1;
        if (e->zones[i].def.group_index >= (int)e->model.group_count) { e->zones[i].source = -1; }
    }
    for (unsigned t = 0; t < DS_MAX_TAGS; ++t) {
        e->tag_volume[t] = e->model.tag_volume[t];
        e->tag_enabled[t] = e->model.tag_enabled[t];
        e->tag_polyphony[t] = e->model.tag_polyphony[t];
    }
    /* Controls start at the preset's values, and fire (DecentSampler's
     * triggerOnLoad default) so the sound matches what the preset shows. */
    e->last_note = -1;
    e->bpm = 120.0f;
    for (unsigned n = 0; n < e->model.note_count; ++n) e->note_map_enabled[n] = (unsigned char)e->model.notes[n].enabled;
    e->initialising = 1;
    for (unsigned c = 0; c < e->model.control_count; ++c) ds_native_engine_set_control(e, c, e->model.controls[c].def);
    e->initialising = 0;
    for (unsigned x = 0; x < e->model.effect_count; ++x) { ds_fx_prepare(&e->fx_coeffs[x], &e->model.effects[x], (float)output_rate); e->fx_dirty[x] = 0; }
    /* An instrument-level reverb, chorus or delay gets its buffers now (the
     * audio thread never allocates). A GROUP-level one would be one per note,
     * as DecentSampler runs it — too heavy here — so it stays off. */
    for (unsigned x = 0; x < e->model.effect_count; ++x) {
        const ds_effect_t *fx = &e->model.effects[x];
        int made = 1;
        if (fx->group >= 0) continue;
        if (!strcmp(fx->type, "reverb")) made = (e->reverb[x] = ds_reverb_create((float)output_rate)) != NULL;
        else if (!strcmp(fx->type, "chorus")) made = (e->chorus[x] = ds_chorus_create((float)output_rate)) != NULL;
        else if (!strcmp(fx->type, "delay")) made = (e->delay[x] = ds_delay_create((float)output_rate, longest_delay(&e->model, x))) != NULL;
        if (!made) { fail(error, error_len, "out of memory"); ds_native_engine_destroy(e); return -1; }
    }
    for (unsigned k = 0; k < e->model.modulator_count; ++k) {
        const ds_modulator_t *m = &e->model.modulators[k];
        e->mod_global[k].stage = DS_ENV_DONE;                         /* a global envelope waits for a key */
        e->mod_rng[k] = m->seed ? m->seed : 0x9e3779b9u * (k + 1);
        if (m->kind == DS_MOD_RANDOM) e->mod_global[k].level = random_next(&e->mod_rng[k]);
        for (unsigned i = 0; i < m->binding_count; ++i) {
            const ds_binding_t *b = &e->model.bindings[m->first_binding + i];
            if (b->target != DS_TARGET_EFFECT) continue;
            for (unsigned x = 0; x < e->model.effect_count; ++x)
                if (b->tag_mask ? (e->model.effects[x].tag_mask & b->tag_mask) != 0 : b->effect == (int)x) e->fx_modulated[x] = 1;
        }
    }
    if (!(e->xf_scratch = calloc((size_t)DS_FILL_FRAMES * 8, sizeof(float)))) {
        fail(error, error_len, "out of memory"); ds_native_engine_destroy(e); return -1;
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
    for (unsigned x = 0; x < DS_MAX_EFFECTS; ++x) {
        ds_reverb_destroy(e->reverb[x]);
        ds_chorus_destroy(e->chorus[x]);
        ds_delay_destroy(e->delay[x]);
    }
    free(e->sources); free(e->source_paths); free(e->zones); free(e->group_len); free(e->groups_rt); free(e->xf_scratch);
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
typedef struct { float gain, pan, vel_track, env[4], key_track, silencing_decay, glide_time; int silencing_mode, root, glide_mode; double tuning; } zone_now_t;

/* Frame positions, key/velocity ranges, root and amp-envelope switch: a
 * binding on the group wins, then one on the instrument, then the sample's
 * own value (every sample sets its own range, so it cannot win here). */
static int64_t pick_frame(const ds_zone_t *z, const ds_group_settings_t *g, const ds_group_settings_t *in,
                          int i, unsigned bit, int64_t own) {
    (void)z;
    if (g->live & bit) return g->frames[i];
    if (in->live & bit) return in->frames[i];
    return own;
}
static int pick_key(const ds_zone_t *z, const ds_group_settings_t *g, const ds_group_settings_t *in,
                    int i, unsigned bit, int own) {
    (void)z;
    if (g->live & bit) return i < 0 ? g->amp_env : g->keys[i];
    if (in->live & bit) return i < 0 ? in->amp_env : in->keys[i];
    return own;
}

static int zone_enabled(const ds_native_engine_t *e, const ds_zone_t *z) {
    uint64_t tags = z->tag_mask;
    if (!z->live_enabled || !e->groups_rt[z->def.group_index].enabled) return 0;
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
    out->gain *= z->live_volume;
    out->tuning = d->base_tuning + g->tuning + in->tuning + z->live_tuning;
    out->pan = z->live_has_pan ? z->live_pan : (d->own_mask & DS_OWN_PAN) ? d->pan : g->has_pan ? g->pan : in->pan;
    out->vel_track = (d->own_mask & DS_OWN_VEL_TRACK) ? d->amp_vel_track : g->has_vel_track ? g->vel_track : in->vel_track;
    for (int i = 0; i < 4; ++i)
        out->env[i] = (d->own_mask & own_env[i]) ? def_env[i] : g->has_env[i] ? g->env[i] : in->env[i];
    out->root = pick_key(z, g, in, 0, DS_OWN_ROOT, d->root_note);
    out->glide_time = (d->own_mask & DS_OWN_GLIDE_TIME) ? d->glide_time : g->has_glide_time ? g->glide_time : in->glide_time;
    out->glide_mode = (d->own_mask & DS_OWN_GLIDE_MODE) ? d->glide_mode : g->has_glide_mode ? g->glide_mode : in->glide_mode;
    out->key_track = (d->own_mask & DS_OWN_KEY_TRACK) ? d->pitch_key_track : g->has_key_track ? g->key_track : in->key_track;
    out->silencing_mode = (d->own_mask & DS_OWN_SILENCING_MODE) ? d->silencing_mode :
                          g->has_silencing_mode ? g->silencing_mode : in->silencing_mode;
    out->silencing_decay = (d->own_mask & DS_OWN_SILENCING_DECAY) ? d->silencing_decay :
                           g->has_silencing_decay ? g->silencing_decay : in->silencing_decay;
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
    case DS_TARGET_KEY_TRACK: g->key_track = v < 0 ? 0 : v > 1 ? 1 : v; g->has_key_track = 1; break;
    case DS_TARGET_SILENCING_MODE: g->silencing_mode = v >= 0.5f ? DS_SILENCE_NORMAL : DS_SILENCE_FAST; g->has_silencing_mode = 1; break;
    case DS_TARGET_SILENCING_DECAY: g->silencing_decay = v < 0 ? 0 : v; g->has_silencing_decay = 1; break;
    case DS_TARGET_SAMPLE_START: case DS_TARGET_SAMPLE_END: case DS_TARGET_LOOP_START: case DS_TARGET_LOOP_END: {
        static const unsigned bits[4] = {DS_OWN_START, DS_OWN_END, DS_OWN_LOOP_START, DS_OWN_LOOP_END};
        int i = target - DS_TARGET_SAMPLE_START;
        g->frames[i] = v < 0 ? 0 : (int64_t)llrint(v);
        g->live |= bits[i];
        break;
    }
    case DS_TARGET_ROOT_NOTE: case DS_TARGET_LO_NOTE: case DS_TARGET_HI_NOTE: case DS_TARGET_LO_VEL: case DS_TARGET_HI_VEL: {
        static const unsigned bits[5] = {DS_OWN_ROOT, DS_OWN_LO_NOTE, DS_OWN_HI_NOTE, DS_OWN_LO_VEL, DS_OWN_HI_VEL};
        int i = target - DS_TARGET_ROOT_NOTE;
        long k = lrintf(v);
        g->keys[i] = k < 0 ? 0 : k > 127 ? 127 : (int)k;
        g->live |= bits[i];
        break;
    }
    case DS_TARGET_AMP_ENV_ENABLED: g->amp_env = v >= 0.5f; g->live |= DS_OWN_AMP_ENV; break;
    case DS_TARGET_GLIDE_TIME: g->glide_time = v < 0 ? 0 : v; g->has_glide_time = 1; break;
    case DS_TARGET_GLIDE_MODE: { long k = lrintf(v); g->glide_mode = k < DS_GLIDE_OFF || k > DS_GLIDE_LEGATO ? DS_GLIDE_LEGATO : (int)k; g->has_glide_mode = 1; break; }
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
    else if (!strcmp(token, "DELAY_TIME") || !strcmp(token, "MOD_DELAY_TIME")) m->delay = v < 0 ? 0 : v;
    else if (!strcmp(token, "SHAPE")) { int k = (int)lrintf(v); m->shape = k < DS_LFO_SINE || k > DS_LFO_TRIANGLE ? DS_LFO_SINE : k; }
    else if (!strcmp(token, "TRIGGER")) m->trigger = v >= 0.5f;
}

static void control_changed(ds_native_engine_t *e, unsigned index, float value, int depth);

/* A binding onto another control. With no output range of its own, a plain
 * linear one spans the target's range (BassForge's CC maps give none: taken
 * literally, any CC above 1 would pin the knob at its top). */
static void drive_control(ds_native_engine_t *e, const ds_binding_t *b, unsigned index, float in_min, float in_max,
                          float value, float translated, int depth) {
    const ds_control_t *c = &e->model.controls[index];
    float v = translated;
    if (b->translation == DS_TRANSLATE_LINEAR && !b->has_range && !b->has_factor && !b->reversed && in_max > in_min) {
        float t = (value - in_min) / (in_max - in_min);
        v = c->min + (t < 0 ? 0 : t > 1 ? 1 : t) * (c->max - c->min);
    }
    control_changed(e, index, v, depth + 1);
}

static void apply_binding(ds_native_engine_t *e, const ds_binding_t *b, float in_min, float in_max, float value, int depth) {
    float v;
    if (b->disabled || (e->initialising && !b->trigger_on_load)) return;
    v = ds_binding_translate(b, in_min, in_max, value);
    switch (b->target) {
    case DS_TARGET_NONE: return;
    case DS_TARGET_CONTROL_VALUE:
        if (depth >= 2) return;
        if (b->tag_mask) {
            for (unsigned i = 0; i < e->model.control_count; ++i)
                if (e->model.controls[i].tag_mask & b->tag_mask) drive_control(e, b, i, in_min, in_max, value, v, depth);
        } else if (b->position >= 0 && b->position < (int)e->model.control_count) {
            drive_control(e, b, (unsigned)b->position, in_min, in_max, value, v, depth);
        }
        return;
    case DS_TARGET_MIDI_ENABLED:
        for (unsigned n = 0; n < e->model.note_count; ++n)
            if (e->model.notes[n].midi_index == b->position) e->note_map_enabled[n] = v >= 0.5f;
        return;
    case DS_TARGET_MODULATOR:
        if (b->tag_mask) {
            for (unsigned i = 0; i < e->model.modulator_count; ++i)
                if (e->model.modulators[i].tag_mask & b->tag_mask) set_modulator_value(&e->model.modulators[i], b->name, v);
        } else {
            int m = b->position < 0 ? 0 : b->position;
            if (m < (int)e->model.modulator_count) set_modulator_value(&e->model.modulators[m], b->name, v);
        }
        return;
    case DS_TARGET_EFFECT:
        for (unsigned i = 0; i < e->model.effect_count; ++i)
            if (b->tag_mask ? (e->model.effects[i].tag_mask & b->tag_mask) != 0 : (int)i == b->effect) {
                set_effect_value(&e->model.effects[i], b->name, v);
                e->fx_dirty[i] = 1;
            }
        return;
    default: break;
    }
    if (b->level == DS_LEVEL_SAMPLE) {
        for (unsigned i = 0; i < e->zone_count; ++i) {
            ds_zone_t *z = &e->zones[i];
            if (!(z->tag_mask & b->tag_mask)) continue;
            if (b->target == DS_TARGET_VOLUME) z->live_volume = v < 0 ? 0 : v;
            else if (b->target == DS_TARGET_TUNING) z->live_tuning = v;
            else if (b->target == DS_TARGET_PAN) { z->live_pan = v / 100.0f; z->live_has_pan = 1; }
            else if (b->target == DS_TARGET_ENABLED) z->live_enabled = v >= 0.5f;
        }
    } else if (b->level == DS_LEVEL_TAG) {
        for (unsigned t = 0; t < e->model.tag_count; ++t) {
            if (!(b->tag_mask & (1ull << t))) continue;
            if (b->target == DS_TARGET_VOLUME) e->tag_volume[t] = v < 0 ? 0 : v;
            else if (b->target == DS_TARGET_ENABLED) e->tag_enabled[t] = v >= 0.5f;
            else if (b->target == DS_TARGET_TAG_POLYPHONY) e->tag_polyphony[t] = v >= 1 ? (int)lrintf(v) : -1;
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

/* A <random>'s next value, -1..1. */
static float random_next(uint32_t *rng) {
    *rng = *rng * 1664525u + 1013904223u;
    return (float)(*rng >> 8) / 8388607.5f - 1.0f;
}

static void mod_start(const ds_modulator_t *m, ds_mod_state_t *st, uint32_t *rng) {
    st->phase = 0;
    st->delay_left = m->delay;
    st->level = m->attack > 0 ? 0.0f : 1.0f;
    st->stage = m->attack > 0 ? DS_ENV_ATTACK : DS_ENV_DECAY;
    if (m->kind == DS_MOD_RANDOM) st->level = random_next(rng);
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
    case DS_MOD_ENVELOPE: raw = st->stage == DS_ENV_DONE || st->delay_left > 0 ? 0 : st->level; break;
    case DS_MOD_RANDOM: raw = st->level; break;
    case DS_MOD_CC: raw = m->cc >= 0 && m->cc < 128 ? e->cc_value[m->cc] : 0; break;
    default: raw = velocity; break;
    }
    return raw * m->mod_amount;
}

static void mod_advance(const ds_modulator_t *m, ds_mod_state_t *st, unsigned frames, float rate, uint32_t *rng) {
    float dt = (float)frames / rate, sustain = clamp01(m->sustain);
    if (m->kind == DS_MOD_RANDOM) {
        if (!m->periodic || m->frequency <= 0) return;
        st->phase += m->frequency * dt;                    /* a new value each 1/frequency s */
        while (st->phase >= 1.0f) { st->phase -= 1.0f; st->level = random_next(rng); }
        return;
    }
    if (m->kind == DS_MOD_ENVELOPE && st->delay_left > 0) { st->delay_left -= dt; return; }   /* MOD_DELAY_TIME */
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
    float lo = m->kind == DS_MOD_LFO || m->kind == DS_MOD_RANDOM ? -1.0f : 0.0f;
    *t = ds_binding_translate(b, lo, 1.0f, value * b->mod_amount);
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
            set_effect_value(&fx, b->name, mod_apply(b->mod_behavior, ds_fx_param(&fx, name, ds_fx_default(fx.type, name)), t, n));
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

static void make_way(ds_native_engine_t *e, const ds_zone_t *z);

/* A time in seconds, beats (at the host's tempo) or samples, as frames. */
static uint32_t frames_of(const ds_native_engine_t *e, float value, int unit) {
    double f = unit == DS_UNIT_SAMPLES ? value :
               unit == DS_UNIT_BEATS ? value * 60.0 / (e->bpm >= 20 ? e->bpm : 120.0) * e->output_rate :
               (double)value * e->output_rate;
    return f <= 0 ? 0 : f > 4e9 ? 4000000000u : (uint32_t)floor(f + 0.5);
}

static void start_voice(ds_native_engine_t *e, const ds_zone_t *z, int note, int velocity, int one_shot) {
    ds_voice_t *v;
    make_way(e, z);
    v = allocate_voice(e);
    const ds_source_t *s = &e->sources[z->source];
    const ds_dspreset_sample_t *d = &z->def;
    zone_now_t now;
    uint32_t zone_index = (uint32_t)(z - e->zones);

    zone_now(e, z, &now);
    v->zone = z; v->src = s;
    {
        const ds_group_settings_t *g = &e->groups_rt[d->group_index], *in = &e->instrument_rt;
        ds_bounds_t b = z->b;
        if ((g->live | in->live) & (DS_OWN_START | DS_OWN_END | DS_OWN_LOOP_START | DS_OWN_LOOP_END))
            compute_bounds(s, pick_frame(z, g, in, 0, DS_OWN_START, d->start), pick_frame(z, g, in, 1, DS_OWN_END, d->end),
                           d->loop_enabled, pick_frame(z, g, in, 2, DS_OWN_LOOP_START, d->loop_start),
                           pick_frame(z, g, in, 3, DS_OWN_LOOP_END, d->loop_end), d->loop_crossfade,
                           d->loop_crossfade_equal_power, &b);
        /* field by field: the worker may be reading the last note's */
        __atomic_store_n(&v->b.start, b.start, __ATOMIC_RELAXED);
        __atomic_store_n(&v->b.end, b.end, __ATOMIC_RELAXED);
        __atomic_store_n(&v->b.loop_start, b.loop_start, __ATOMIC_RELAXED);
        __atomic_store_n(&v->b.loop_end, b.loop_end, __ATOMIC_RELAXED);
        __atomic_store_n(&v->b.xf, b.xf, __ATOMIC_RELAXED);
        __atomic_store_n(&v->b.loop, b.loop, __ATOMIC_RELAXED);
        __atomic_store_n(&v->b.streams, b.streams, __ATOMIC_RELAXED);
        __atomic_store_n(&v->b.xf_equal_power, b.xf_equal_power, __ATOMIC_RELAXED);
        v->amp_env = pick_key(z, g, in, -1, DS_OWN_AMP_ENV, d->amp_env_enabled);
    }
    v->pos = (double)v->b.start;
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
        if (e->model.modulators[k].voice_scope) mod_start(&e->model.modulators[k], &v->mods[k], &e->mod_rng[k]);
    v->note = note; v->velocity = velocity;
    v->key_down = !one_shot; v->one_shot = one_shot; v->sustained = 0;
    /* portamento from the previous note: a glide in pitch, constant in time */
    v->glide_left = 0;
    if (!one_shot && now.glide_time > 0 && e->ctx_prev >= 0 && e->ctx_prev != note &&
        (now.glide_mode == DS_GLIDE_ALWAYS || (now.glide_mode == DS_GLIDE_LEGATO && e->ctx_keys_before > 0))) {
        double frames = floor(now.glide_time * e->output_rate + 0.5);
        v->glide_left = frames < 1 ? 1 : (uint32_t)frames;
        v->glide_step = -((double)(e->ctx_prev - note) * now.key_track) / (double)v->glide_left;
    }
    /* a start delay, and a retrigger pattern while the key is held */
    v->delay_left = frames_of(e, d->delay, d->delay_unit);
    if (d->retrigger && !one_shot && !e->retriggering && e->retrig_count < DS_MAX_VOICES) {
        uint32_t every = frames_of(e, d->retrigger_interval, d->retrigger_unit);
        if (every) {
            e->retrig[e->retrig_count].zone = z;
            e->retrig[e->retrig_count].note = note;
            e->retrig[e->retrig_count].velocity = velocity;
            e->retrig[e->retrig_count].note_id = e->note_counter;
            e->retrig[e->retrig_count].every = every;
            e->retrig[e->retrig_count].in = v->delay_left + every;
            e->retrig_count++;
        }
    }
    /* a release trigger quieter the longer its key was held */
    v->rt_gain = 1.0f;
    if (one_shot && d->release_decay > 0) {
        float held = (float)(e->frame_clock - e->note_on_frame[note]) / (float)e->output_rate;
        v->rt_gain = d->release_decay_db ? powf(10.0f, -d->release_decay * held / 20.0f) : 1.0f - d->release_decay * held;
        if (v->rt_gain < 0) v->rt_gain = 0;
    }
    v->age = ++e->age_counter;
    v->underruns = 0;
    if (!v->amp_env) {
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
    if (v->b.streams) {
        uint64_t ring_from = v->b.start >= s->head_frames ? v->b.start : s->head_frames;
        atomic_store_explicit(&v->consumed, (uint32_t)v->b.start, memory_order_relaxed);
        atomic_store_explicit(&v->stream, pack(v->generation, zone_index + 1, (uint32_t)ring_from),
                              memory_order_release);
    } else {
        atomic_store_explicit(&v->stream, pack(v->generation, 0, 0), memory_order_release);
    }
    v->active = 1;
}

static void release_voice(ds_native_engine_t *e, ds_voice_t *v) {
    if (v->env_stage == DS_ENV_DONE) return;
    if (v->amp_env) {
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
    const ds_group_settings_t *g = &e->groups_rt[z->def.group_index], *in = &e->instrument_rt;
    const ds_dspreset_sample_t *d = &z->def;
    if (trigger == DS_TRIGGER_RELEASE ? d->trigger != DS_TRIGGER_RELEASE : d->trigger == DS_TRIGGER_RELEASE) return 0;
    /* first: only with no other key down; legato: only with one (continuous: always) */
    if (d->trigger == DS_TRIGGER_FIRST && e->ctx_keys_before) return 0;
    if (d->trigger == DS_TRIGGER_LEGATO && !e->ctx_keys_before) return 0;
    if (d->previous_count) {
        int k = 0;
        while (k < d->previous_count && d->previous_notes[k] != e->ctx_prev) ++k;
        if (e->ctx_prev < 0 || k == d->previous_count) return 0;
    }
    if (d->legato_interval != DS_NO_INTERVAL && (e->ctx_prev < 0 || note - e->ctx_prev != d->legato_interval)) return 0;
    return z->source >= 0 && zone_enabled(e, z) &&
           note >= pick_key(z, g, in, 1, DS_OWN_LO_NOTE, d->lo_note) && note <= pick_key(z, g, in, 2, DS_OWN_HI_NOTE, d->hi_note) &&
           velocity >= pick_key(z, g, in, 3, DS_OWN_LO_VEL, d->lo_vel) && velocity <= pick_key(z, g, in, 4, DS_OWN_HI_VEL, d->hi_vel);
}

/* The one way a voice is cut short: over `decay` seconds when that is set,
 * else by its own release ("normal"), else in 5 ms ("fast": immediate, without
 * the click of a jump to zero). It stops counting toward any limit. */
static void silence_voice(ds_native_engine_t *e, ds_voice_t *v, int mode, float decay) {
    if (v->env_stage == DS_ENV_DONE) return;
    if (decay <= 0 && mode == DS_SILENCE_NORMAL) {
        release_voice(e, v);
    } else {
        if (v->env_stage == DS_ENV_ATTACK && v->env_level <= 0.0f) v->env_level = v->env_attack_step < 1.0f ? v->env_attack_step : 1.0f;
        v->env_release_coef = coef_for(decay > 0 ? decay : 0.005f, e->output_rate);
        v->env_stage = DS_ENV_RELEASE;
    }
    v->key_down = 0;
    v->sustained = 0;
    v->choked = 1;
}

/* A voice silenced by another sample, with its OWN silencing settings. */
static void silence_by_zone(ds_native_engine_t *e, ds_voice_t *v) {
    zone_now_t now;
    zone_now(e, v->zone, &now);
    silence_voice(e, v, now.silencing_mode, now.silencing_decay);
}

/* Fades every voice of note `id` out in 5 ms and stops counting it. */
static void choke_note(ds_native_engine_t *e, uint32_t id) {
    for (int i = 0; i < DS_MAX_VOICES; ++i) {
        ds_voice_t *v = &e->voices[i];
        if (!v->active || v->one_shot || v->note_id != id) continue;
        silence_voice(e, v, DS_SILENCE_FAST, 0);
    }
}

/* Before zone `z` starts: stop what it silences (silencedByTags) and make room
 * under each of its tags' voice limits, oldest first. Voices of the note
 * being started are left alone, so one key's layers never cut each other. */
static void make_way(ds_native_engine_t *e, const ds_zone_t *z) {
    for (int i = 0; i < DS_MAX_VOICES; ++i) {
        ds_voice_t *v = &e->voices[i];
        if (v->active && !v->choked && v->note_id != e->note_counter && (v->zone->silenced_by & z->tag_mask))
            silence_by_zone(e, v);
    }
    for (unsigned t = 0; t < e->model.tag_count; ++t) {
        if (!(z->tag_mask & (1ull << t)) || e->tag_polyphony[t] < 1) continue;
        for (;;) {
            ds_voice_t *oldest = NULL;
            int count = 0;
            for (int i = 0; i < DS_MAX_VOICES; ++i) {
                ds_voice_t *v = &e->voices[i];
                if (!v->active || v->choked || v->note_id == e->note_counter || !(v->zone->tag_mask & (1ull << t))) continue;
                count++;
                if (!oldest || v->age < oldest->age) oldest = v;
            }
            if (count < e->tag_polyphony[t]) break;
            silence_by_zone(e, oldest);
        }
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

/* <midi><note> listeners for `note`: fire the bindings of those listening to
 * this event; 1 if any of them swallows the key. */
static int note_listeners(ds_native_engine_t *e, int note, int velocity, int on) {
    int swallow = 0;
    for (unsigned n = 0; n < e->model.note_count; ++n) {
        const ds_note_map_t *map = &e->model.notes[n];
        if (!e->note_map_enabled[n] || note < map->lo || note > map->hi) continue;
        swallow |= map->swallow;
        if (map->event != DS_NOTE_EVENT_ANY && map->event != (on ? DS_NOTE_EVENT_ON : DS_NOTE_EVENT_OFF)) continue;
        for (unsigned i = 0; i < map->binding_count; ++i)
            apply_binding(e, &e->model.bindings[map->first_binding + i], 0, 127, (float)velocity, 0);
    }
    return swallow;
}

void ds_native_engine_note_on(ds_native_engine_t *e, int note, int velocity) {
    if (!e || note < 0 || note > 127 || velocity < 0 || velocity > 127) return;
    if (!velocity) { ds_native_engine_note_off(e, note); return; }
    /* keyswitches act before the note plays; a swallowing one keeps it silent */
    if (e->model.note_count && note_listeners(e, note, velocity, 1)) { e->swallowed[note] = 1; return; }
    e->ctx_prev = e->last_note;
    e->ctx_keys_before = (int)e->keys_held - (e->note_velocity[note] ? 1 : 0);
    e->note_on_frame[note] = e->frame_clock;
    e->key_note_id[note] = e->note_counter + 1;               /* the id this note-on is about to take */
    if (!e->note_velocity[note] && e->keys_held++ == 0)     /* a global envelope starts with the first key */
        for (unsigned k = 0; k < e->model.modulator_count; ++k)
            if (!e->model.modulators[k].voice_scope && e->model.modulators[k].kind == DS_MOD_ENVELOPE)
                mod_start(&e->model.modulators[k], &e->mod_global[k], &e->mod_rng[k]);
    for (unsigned k = 0; k < e->model.modulator_count; ++k) {  /* trigger="attack", and a note-on <random>: every key */
        const ds_modulator_t *m = &e->model.modulators[k];
        if (m->voice_scope) continue;
        if ((m->kind == DS_MOD_LFO && m->trigger) || (m->kind == DS_MOD_RANDOM && (m->trigger || !m->periodic)))
            mod_start(m, &e->mod_global[k], &e->mod_rng[k]);
    }
    e->note_velocity[note] = velocity;
    enforce_note_limit(e);
    e->note_counter++;
    trigger_zones(e, note, velocity, DS_TRIGGER_ATTACK);
    e->last_note = note;
}

void ds_native_engine_note_off(ds_native_engine_t *e, int note) {
    if (!e || note < 0 || note > 127) return;
    if (e->model.note_count) note_listeners(e, note, 0, 0);
    if (e->swallowed[note]) { e->swallowed[note] = 0; return; }
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
    const ds_bounds_t *b = &v->b;
    const ds_source_t *s = v->src;
    unsigned ch = s->file.channels;
    uint64_t f = map_frame(b, vf);
    const float *frame;
    float g_out, g_in;
    if (!b->loop && vf >= b->end) { *l = *r = 0; return 1; }
    if (f < s->head_frames) {
        frame = s->head + f * ch;
        if (xf_gains(b, f, &g_out, &g_in)) {            /* the far side is in the head too */
            const float *far = s->head + (f - (b->loop_end - b->loop_start)) * ch;
            *l = frame[0] * g_out + far[0] * g_in;
            *r = ch > 1 ? frame[1] * g_out + far[1] * g_in : *l;
            return 1;
        }
    } else if (vf < produced) {
        frame = v->ring + (vf & RING_MASK) * ch;        /* the worker mixed any crossfade in */
    } else {
        return 0;
    }
    *l = frame[0];
    *r = ch > 1 ? frame[1] : frame[0];
    return 1;
}

static void render_voice(ds_native_engine_t *e, ds_voice_t *v, float *out, unsigned frames, const zone_now_t *settings) {
    uint32_t produced = packed_produced(atomic_load_explicit(&v->stream, memory_order_acquire));
    double inc, glide_ratio = 1.0;
    unsigned first;
    {   /* volume, pan and pitch follow the controls (and modulators) while the note sounds */
        zone_now_t now = *settings;
        float gain;
        gain = now.gain * (1.0f - now.vel_track + now.vel_track * v->vel) * v->rt_gain;
        v->gain_l = gain * (now.pan > 0 ? 1.0f - now.pan : 1.0f);
        v->gain_r = gain * (now.pan < 0 ? 1.0f + now.pan : 1.0f);
        v->inc = ((double)v->src->file.sample_rate / e->output_rate) *
                 pow(2.0, ((v->note - now.root) * now.key_track + now.tuning) / 12.0);
        /* A Sustain moved while the note is held: glide there (never a jump). */
        if (v->amp_env && (v->env_stage == DS_ENV_DECAY || v->env_stage == DS_ENV_SUSTAIN)) {
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
    if (v->glide_left) {                        /* where the glide is now, then one ratio per frame */
        glide_ratio = pow(2.0, v->glide_step / 12.0);
        inc *= pow(2.0, -v->glide_step * v->glide_left / 12.0);
    }
    {   /* a start delay: silent, and nothing moves, until it runs out */
        unsigned wait = v->delay_left < frames ? v->delay_left : frames;
        v->delay_left -= wait;
        first = wait;
    }
    for (unsigned i = first; i < frames; ++i) {
        uint64_t v0 = (uint64_t)v->pos;
        float frac = (float)(v->pos - (double)v0), l0, r0, l1, r1, level;
        if (!v->b.loop && v0 >= v->b.end) { v->active = 0; break; }
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
        if (v->glide_left && --v->glide_left) inc *= glide_ratio;
        else if (glide_ratio != 1.0) { inc = v->inc * e->bend_ratio; glide_ratio = 1.0; }
    }
    if (v->b.streams) atomic_store_explicit(&v->consumed, (uint32_t)v->pos, memory_order_release);
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
        mod_advance(m, &e->mod_global[k], frames, (float)e->output_rate, &e->mod_rng[k]);
    }
    for (unsigned r = 0; r < e->retrig_count; ) {           /* retriggers due in this block */
        int note = e->retrig[r].note;
        if (!e->note_velocity[note] || e->key_note_id[note] != e->retrig[r].note_id) {   /* its key is up: the pattern stops */
            e->retrig[r] = e->retrig[--e->retrig_count];
            continue;
        }
        while (e->retrig[r].in < frames) {
            int prev = e->ctx_prev, keys = e->ctx_keys_before;
            uint32_t at = e->retrig[r].in, id = e->note_counter;
            e->ctx_prev = -1; e->ctx_keys_before = 0;      /* a repeat, not a new note: no glide */
            e->retriggering = 1;
            e->note_counter = e->retrig[r].note_id;        /* its note's own voice, for chokes and limits */
            start_voice(e, e->retrig[r].zone, note, e->retrig[r].velocity, 0);
            e->note_counter = id;
            e->retriggering = 0;
            e->ctx_prev = prev; e->ctx_keys_before = keys;
            for (int i = 0; i < DS_MAX_VOICES; ++i)
                if (e->voices[i].active && e->voices[i].age == e->age_counter) e->voices[i].delay_left = at;   /* the zone's own delay was the pattern's first */
            e->retrig[r].in = at + e->retrig[r].every;
        }
        e->retrig[r].in -= frames;
        ++r;
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
            /* (a reverb, chorus or delay inside a note is not run: see the load) */
            filtering |= chain[k]->kind == DS_FX_BIQUAD || chain[k]->kind == DS_FX_ONEPOLE || chain[k]->kind == DS_FX_GAIN;
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
            if (e->model.modulators[k].voice_scope) mod_advance(&e->model.modulators[k], &v->mods[k], frames, (float)e->output_rate, &e->mod_rng[k]);
        underruns += v->underruns;
    }
    for (unsigned x = 0; x < e->model.effect_count; ++x) {
        const ds_fx_coeffs_t *c = &e->fx_coeffs[x];
        if (e->model.effects[x].group >= 0) continue;
        if (e->fx_modulated[x]) { modulated_effect(e, x, e->mod_global_value, 0, &e->fx_live[x], &e->fx_live_built[x]); c = &e->fx_live[x]; }
        if (c->kind == DS_FX_REVERB) {
            if (e->reverb[x]) { ds_reverb_set(e->reverb[x], c->room, c->damping, c->wet); ds_reverb_process(e->reverb[x], out_lr, frames); }
        } else if (c->kind == DS_FX_CHORUS) {
            if (e->chorus[x]) { ds_chorus_set(e->chorus[x], c->mix, c->depth, c->rate); ds_chorus_process(e->chorus[x], out_lr, frames); }
        } else if (c->kind == DS_FX_DELAY) {
            if (e->delay[x]) { ds_delay_set(e->delay[x], c->time, c->offset, c->feedback, c->wet); ds_delay_process(e->delay[x], out_lr, frames); }
        } else {
            ds_fx_process(c, &e->fx_state[x], out_lr, frames);
        }
    }
    e->frame_clock += frames;
    if (underruns) atomic_fetch_add_explicit(&e->underruns, underruns, memory_order_relaxed);
    for (int i = 0; i < DS_MAX_VOICES; ++i) e->voices[i].underruns = 0;
}

unsigned ds_native_engine_active_voices(const ds_native_engine_t *e) {
    unsigned n = 0;
    for (int i = 0; e && i < DS_MAX_VOICES; ++i) n += e->voices[i].active != 0;
    return n;
}

/* ---- worker ------------------------------------------------------------- */

/* The voice's bounds as the worker may use them: read field by field and made
 * safe to index with, whatever a concurrent restart left half-written (that
 * fill is discarded by the publish below). */
static void worker_bounds(const ds_voice_t *v, const ds_source_t *s, ds_bounds_t *b) {
    uint64_t frames = s->file.frame_count;
    b->start = __atomic_load_n(&v->b.start, __ATOMIC_RELAXED);
    b->end = __atomic_load_n(&v->b.end, __ATOMIC_RELAXED);
    b->loop_start = __atomic_load_n(&v->b.loop_start, __ATOMIC_RELAXED);
    b->loop_end = __atomic_load_n(&v->b.loop_end, __ATOMIC_RELAXED);
    b->xf = __atomic_load_n(&v->b.xf, __ATOMIC_RELAXED);
    b->loop = __atomic_load_n(&v->b.loop, __ATOMIC_RELAXED);
    b->streams = __atomic_load_n(&v->b.streams, __ATOMIC_RELAXED);
    b->xf_equal_power = __atomic_load_n(&v->b.xf_equal_power, __ATOMIC_RELAXED);
    if (b->end > frames) b->end = frames;
    if (b->start >= b->end) b->start = 0;
    if (b->loop_end > frames || b->loop_end <= b->loop_start + 1) b->loop = 0;
    if (!b->loop || b->xf > b->loop_start || b->xf > b->loop_end - b->loop_start) b->xf = 0;
}

static void fill(ds_native_engine_t *e, ds_voice_t *v, int fd, const ds_zone_t *z, const ds_bounds_t *b,
                 uint64_t from, unsigned count) {
    const ds_source_t *s = &e->sources[z->source];
    ds_wav_source_t file = s->file;
    unsigned ch = s->file.channels;
    char error[64];
    file.fd = fd;
    while (count) {
        uint64_t f = map_frame(b, from);
        uint64_t limit = b->loop && f < b->loop_end ? b->loop_end : b->end;
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
        if (b->xf && f + run > b->loop_end - b->xf) {       /* mix the loop crossfade in, here, off the audio thread */
            uint64_t first = f > b->loop_end - b->xf ? f : b->loop_end - b->xf, len = b->loop_end - b->loop_start;
            unsigned n = (unsigned)(f + run - first);
            const float *far = NULL;
            if (first - len + n <= s->head_frames) far = s->head + (first - len) * ch;
            else if (fd >= 0 && e->xf_scratch && ch <= 8 &&
                     ds_wav_source_read_frames(&file, first - len, e->xf_scratch, n, error, sizeof(error)) == (int)n)
                far = e->xf_scratch;
            for (unsigned k = 0; far && k < n; ++k) {
                float g_out, g_in, *out = dst + (size_t)(first - f + k) * ch;
                xf_gains(b, first + k, &g_out, &g_in);
                for (unsigned c = 0; c < ch; ++c) out[c] = out[c] * g_out + far[(size_t)k * ch + c] * g_in;
            }
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
        ds_bounds_t b;
        uint64_t space, count;
        if (!zone_plus_one) continue;
        z = &e->zones[zone_plus_one - 1];
        worker_bounds(v, &e->sources[z->source], &b);
        produced = packed_produced(word);
        consumed = atomic_load_explicit(&v->consumed, memory_order_acquire);
        if (!b.loop && produced >= b.end) continue;
        space = (uint64_t)consumed + DS_RING_FRAMES - produced;
        if (consumed > produced || space < 1024) continue;
        count = space < DS_FILL_FRAMES ? space : DS_FILL_FRAMES;
        if (!b.loop && produced + count > b.end) count = b.end - produced;
        /* The voice's own descriptor, reopened when it starts a new note. */
        if (e->stream_key[i] != (word >> 32)) {
            if (e->stream_fd[i] >= 0 && e->stream_key[i]) close(e->stream_fd[i]);
            e->stream_fd[i] = open(e->source_paths[z->source], O_RDONLY);
            e->stream_key[i] = word >> 32;
        }
        fill(e, v, e->stream_fd[i], z, &b, produced, (unsigned)count);
        /* Publish only if the voice was not restarted meanwhile. */
        if (atomic_compare_exchange_strong_explicit(&v->stream, &word,
                                                    (word & ~0xffffffffull) | (uint32_t)(produced + count),
                                                    memory_order_release, memory_order_relaxed))
            total += (unsigned)count;
    }
    return total;
}
