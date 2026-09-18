#include "native_engine.h"

#include <fcntl.h>
#include <math.h>
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

static int source_for(ds_native_engine_t *e, const char *path) {
    ds_source_t *s;
    char error[128];
    for (unsigned i = 0; i < e->source_count; ++i)
        if (!strcmp(e->source_paths[i], path)) return e->sources[i].file.frame_count ? (int)i : -1;
    s = &e->sources[e->source_count];
    snprintf(e->source_paths[e->source_count], sizeof(e->source_paths[0]), "%s", path);
    e->source_count++;
    if (ds_wav_source_open(&s->file, path, error, sizeof(error)) ||
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
                          unsigned output_rate, char *error, unsigned error_len) {
    char directory[1024], path[1600], *slash;
    collect_t collect;
    if (!e || !preset_path || !output_rate) return -1;
    memset(e, 0, sizeof(*e));
    e->output_rate = output_rate;
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
    free(e->sources); free(e->source_paths); free(e->zones); free(e->group_len);
    memset(e, 0, sizeof(*e));
}

/* ---- audio thread ------------------------------------------------------- */

static float coef_for(float seconds, unsigned rate) {
    return seconds > 0 ? expf(logf(SILENT) / (seconds * (float)rate)) : 0.0f;
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
    float vel_gain = 1.0f - d->amp_vel_track + d->amp_vel_track * (velocity / 127.0f);
    float gain = d->gain * vel_gain;
    uint32_t zone_index = (uint32_t)(z - e->zones);

    v->zone = z; v->src = s;
    v->pos = (double)z->start;
    v->inc = ((double)s->file.sample_rate / e->output_rate) * pow(2.0, (note - d->root_note + d->tuning) / 12.0);
    v->gain_l = gain * (d->pan > 0 ? 1.0f - d->pan : 1.0f);
    v->gain_r = gain * (d->pan < 0 ? 1.0f + d->pan : 1.0f);
    v->note = note; v->velocity = velocity;
    v->key_down = !one_shot; v->one_shot = one_shot; v->sustained = 0;
    v->age = ++e->age_counter;
    v->underruns = 0;
    if (!d->amp_env_enabled) {
        v->env_stage = DS_ENV_SUSTAIN; v->env_level = 1.0f; v->env_sustain = 1.0f;
        v->env_release_coef = coef_for(0.005f, e->output_rate);
    } else {
        v->env_sustain = d->sustain < 0 ? 0 : d->sustain > 1 ? 1 : d->sustain;
        v->env_attack_step = d->attack > 0 ? 1.0f / (d->attack * (float)e->output_rate) : 1.0f;
        v->env_decay_coef = coef_for(d->decay, e->output_rate);
        v->env_release_coef = coef_for(d->release > 0.002f ? d->release : 0.002f, e->output_rate);
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

static void release_voice(ds_voice_t *v) {
    if (v->env_stage != DS_ENV_DONE) v->env_stage = DS_ENV_RELEASE;
}

static int zone_matches(const ds_zone_t *z, int note, int velocity, int trigger) {
    return z->source >= 0 && z->def.trigger == trigger &&
           note >= z->def.lo_note && note <= z->def.hi_note &&
           velocity >= z->def.lo_vel && velocity <= z->def.hi_vel;
}

static void trigger_zones(ds_native_engine_t *e, int note, int velocity, int trigger) {
    uint32_t counter = e->rr_counter[note]++, random;
    e->rng = e->rng * 1664525u + 1013904223u;
    random = e->rng >> 8;
    memset(e->group_len, 0, e->group_count);
    for (unsigned i = 0; i < e->zone_count; ++i) {
        const ds_zone_t *z = &e->zones[i];
        if (zone_matches(z, note, velocity, trigger) && z->def.seq_position > e->group_len[z->def.group_index])
            e->group_len[z->def.group_index] = (unsigned char)(z->def.seq_position > 255 ? 255 : z->def.seq_position);
    }
    for (unsigned i = 0; i < e->zone_count; ++i) {
        const ds_zone_t *z = &e->zones[i];
        unsigned len;
        if (!zone_matches(z, note, velocity, trigger)) continue;
        len = e->group_len[z->def.group_index] ? e->group_len[z->def.group_index] : 1;
        if (z->def.seq_mode == DS_SEQ_ROUND_ROBIN && (int)(counter % len) + 1 != z->def.seq_position) continue;
        if (z->def.seq_mode == DS_SEQ_RANDOM && (int)(random % len) + 1 != z->def.seq_position) continue;
        start_voice(e, z, note, velocity, trigger == DS_TRIGGER_RELEASE);
    }
}

void ds_native_engine_note_on(ds_native_engine_t *e, int note, int velocity) {
    if (!e || note < 0 || note > 127 || velocity < 0 || velocity > 127) return;
    if (!velocity) { ds_native_engine_note_off(e, note); return; }
    e->note_velocity[note] = velocity;
    trigger_zones(e, note, velocity, DS_TRIGGER_ATTACK);
}

void ds_native_engine_note_off(ds_native_engine_t *e, int note) {
    if (!e || note < 0 || note > 127) return;
    for (int i = 0; i < DS_MAX_VOICES; ++i) {
        ds_voice_t *v = &e->voices[i];
        if (!v->active || !v->key_down || v->note != note) continue;
        v->key_down = 0;
        if (e->sustain_pedal) v->sustained = 1;
        else release_voice(v);
    }
    if (e->note_velocity[note]) trigger_zones(e, note, e->note_velocity[note], DS_TRIGGER_RELEASE);
    e->note_velocity[note] = 0;
}

void ds_native_engine_cc(ds_native_engine_t *e, int cc, int value) {
    if (!e) return;
    if (cc == 64) {
        int down = value >= 64;
        if (e->sustain_pedal && !down)
            for (int i = 0; i < DS_MAX_VOICES; ++i)
                if (e->voices[i].active && e->voices[i].sustained) { e->voices[i].sustained = 0; release_voice(&e->voices[i]); }
        e->sustain_pedal = down;
    } else if (cc == 120) {
        for (int i = 0; i < DS_MAX_VOICES; ++i) e->voices[i].active = 0;
    } else if (cc == 123) {
        for (int i = 0; i < DS_MAX_VOICES; ++i) if (e->voices[i].active) { e->voices[i].key_down = 0; release_voice(&e->voices[i]); }
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

static void render_voice(ds_native_engine_t *e, ds_voice_t *v, float *out, unsigned frames) {
    uint32_t produced = packed_produced(atomic_load_explicit(&v->stream, memory_order_acquire));
    double inc = v->inc * e->bend_ratio;
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
            if (v->env_level - v->env_sustain < 1e-4f) { v->env_level = v->env_sustain; v->env_stage = DS_ENV_SUSTAIN; }
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
    for (int i = 0; i < DS_MAX_VOICES; ++i) {
        ds_voice_t *v = &e->voices[i];
        if (!v->active) continue;
        render_voice(e, v, out_lr, frames);
        underruns += v->underruns;
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
