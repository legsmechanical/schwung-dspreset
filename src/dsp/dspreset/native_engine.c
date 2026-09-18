#include "native_engine.h"

#include <stdio.h>
#include <string.h>

static int source_for_region(ds_native_engine_t *engine, const char *directory,
                             const ds_dspreset_sample_t *region, unsigned *index,
                             char *error, unsigned error_len) {
    char path[1024];
    if (snprintf(path, sizeof(path), "%s/%s", directory, region->path) >= (int)sizeof(path)) return -1;
    for (unsigned i = 0; i < engine->source_count; ++i) {
        if (!strcmp(path, engine->source_paths[i])) { *index = i; return 0; }
    }
    if (engine->source_count == DS_MAX_SOURCES ||
        ds_wav_source_open(&engine->sources[engine->source_count], path, error, error_len)) return -1;
    if (engine->sources[engine->source_count].channels > DS_CACHE_MAX_CHANNELS) {
        ds_wav_source_close(&engine->sources[engine->source_count]);
        if (error_len) snprintf(error, error_len, "%s", "WAVE has too many channels");
        return -1;
    }
    *index = engine->source_count;
    memcpy(engine->source_paths[engine->source_count], path, strlen(path) + 1);
    engine->source_count++;
    return 0;
}

int ds_native_engine_load(ds_native_engine_t *engine, const char *preset_path,
                          char *error, unsigned error_len) {
    char directory[1024];
    char *slash;
    if (!engine || !preset_path) return -1;
    memset(engine, 0, sizeof(*engine));
    ds_page_cache_init(&engine->cache);
    if (snprintf(directory, sizeof(directory), "%s", preset_path) >= (int)sizeof(directory) ||
        !(slash = strrchr(directory, '/'))) return -1;
    *slash = '\0';
    if (ds_region_map_load(&engine->regions, preset_path, error, error_len)) return -1;
    for (unsigned i = 0; i < engine->regions.count; ++i) {
        if (source_for_region(engine, directory, &engine->regions.regions[i],
                              &engine->region_sources[i], error, error_len)) {
            ds_native_engine_destroy(engine);
            return -1;
        }
    }
    return 0;
}

void ds_native_engine_destroy(ds_native_engine_t *engine) {
    if (!engine) return;
    for (unsigned i = 0; i < engine->source_count; ++i) ds_wav_source_close(&engine->sources[i]);
    memset(engine, 0, sizeof(*engine));
}

void ds_native_engine_note_on(ds_native_engine_t *engine, int note, int velocity,
                              unsigned output_sample_rate) {
    int max_sequence = 1, selected, voice_index = -1;
    if (!engine || note < 0 || note > 127 || velocity < 0 || velocity > 127) return;
    for (unsigned i = 0; i < engine->regions.count; ++i) {
        ds_dspreset_sample_t *r = &engine->regions.regions[i];
        if (note >= r->lo_note && note <= r->hi_note && velocity >= r->lo_vel && velocity <= r->hi_vel &&
            r->seq_position > max_sequence) max_sequence = r->seq_position;
    }
    selected = (int)(engine->sequence[note]++ % (unsigned)max_sequence) + 1;
    for (unsigned i = 0; i < engine->regions.count; ++i) {
        ds_dspreset_sample_t *r = &engine->regions.regions[i];
        if (note < r->lo_note || note > r->hi_note || velocity < r->lo_vel || velocity > r->hi_vel ||
            r->seq_position != selected) continue;
        for (unsigned attempt = 0; attempt < DS_MAX_VOICES; ++attempt) {
            unsigned candidate = (engine->next_voice + attempt) % DS_MAX_VOICES;
            if (!engine->voices[candidate].active) { voice_index = (int)candidate; break; }
        }
        if (voice_index < 0) voice_index = (int)(engine->next_voice++ % DS_MAX_VOICES);
        ds_voice_start(&engine->voices[voice_index], r, &engine->sources[engine->region_sources[i]],
                       note, velocity, output_sample_rate);
        engine->next_voice = ((unsigned)voice_index + 1) % DS_MAX_VOICES;
    }
}

void ds_native_engine_note_off(ds_native_engine_t *engine, int note) {
    if (!engine) return;
    for (unsigned i = 0; i < DS_MAX_VOICES; ++i) if (engine->voices[i].active && engine->voices[i].note == note) ds_voice_release(&engine->voices[i]);
}

unsigned ds_native_engine_service(ds_native_engine_t *engine, char *error, unsigned error_len) {
    return engine ? ds_page_cache_service(&engine->cache, error, error_len) : 0;
}

void ds_native_engine_render(ds_native_engine_t *engine, float *out_lr, unsigned frames) {
    if (!engine || !out_lr) return;
    for (unsigned i = 0; i < DS_MAX_VOICES; ++i) ds_voice_render(&engine->voices[i], &engine->cache, out_lr, frames);
}
