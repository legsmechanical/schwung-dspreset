#ifndef DSPRESET_NATIVE_ENGINE_H
#define DSPRESET_NATIVE_ENGINE_H

#include "region_map.h"
#include "voice.h"

#define DS_MAX_SOURCES 1024
#define DS_MAX_VOICES 64

typedef struct {
    ds_region_map_t regions;
    ds_wav_source_t sources[DS_MAX_SOURCES];
    char source_paths[DS_MAX_SOURCES][512];
    unsigned region_sources[DS_MAX_REGIONS];
    unsigned source_count, next_voice;
    unsigned sequence[128];
    ds_voice_t voices[DS_MAX_VOICES];
    ds_page_cache_t cache;
} ds_native_engine_t;

/* Loading is worker-only. Render and MIDI calls never parse XML/open files. */
int ds_native_engine_load(ds_native_engine_t *engine, const char *preset_path,
                          char *error, unsigned error_len);
void ds_native_engine_destroy(ds_native_engine_t *engine);
void ds_native_engine_note_on(ds_native_engine_t *engine, int note, int velocity,
                              unsigned output_sample_rate);
void ds_native_engine_note_off(ds_native_engine_t *engine, int note);
unsigned ds_native_engine_service(ds_native_engine_t *engine, char *error, unsigned error_len);
void ds_native_engine_render(ds_native_engine_t *engine, float *out_lr, unsigned frames);

#endif
