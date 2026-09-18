#ifndef DSPRESET_VOICE_H
#define DSPRESET_VOICE_H

#include "dspreset_parser.h"
#include "page_cache.h"

typedef struct {
    const ds_wav_source_t *source;
    double position, increment;
    float gain, release_gain;
    int note, active, released;
} ds_voice_t;

/* Audio-safe after worker-created `source` and page cache have been supplied. */
void ds_voice_start(ds_voice_t *voice, const ds_dspreset_sample_t *region,
                    const ds_wav_source_t *source, int note, int velocity,
                    unsigned output_sample_rate);
void ds_voice_release(ds_voice_t *voice);
void ds_voice_render(ds_voice_t *voice, ds_page_cache_t *cache,
                     float *out_lr, unsigned frames);

#endif
