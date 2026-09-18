#include "voice.h"

#include <math.h>
#include <string.h>

void ds_voice_start(ds_voice_t *voice, const ds_dspreset_sample_t *region,
                    const ds_wav_source_t *source, int note, int velocity,
                    unsigned output_sample_rate) {
    double semitones;
    memset(voice, 0, sizeof(*voice));
    if (!region || !source || !output_sample_rate) return;
    semitones = (double)(note - region->root_note) / 12.0;
    voice->source = source;
    voice->increment = ((double)source->sample_rate / output_sample_rate) * pow(2.0, semitones);
    voice->gain = velocity / 127.0f;
    voice->release_gain = 1.0f;
    voice->note = note;
    voice->active = 1;
}

void ds_voice_release(ds_voice_t *voice) { if (voice) voice->released = 1; }

void ds_voice_render(ds_voice_t *voice, ds_page_cache_t *cache, float *out_lr, unsigned frames) {
    if (!voice || !voice->active || !cache || !out_lr) return;
    for (unsigned i = 0; i < frames; ++i) {
        uint64_t frame = (uint64_t)voice->position;
        float left = 0, right = 0;
        if (frame >= voice->source->frame_count) { voice->active = 0; break; }
        ds_page_cache_request(cache, voice->source, frame);
        ds_page_cache_request(cache, voice->source, frame + DS_CACHE_PAGE_FRAMES);
        if (ds_page_cache_sample(cache, voice->source, frame, 0, &left)) {
            if (voice->source->channels > 1) ds_page_cache_sample(cache, voice->source, frame, 1, &right);
            else right = left;
            out_lr[2 * i] += left * voice->gain * voice->release_gain;
            out_lr[2 * i + 1] += right * voice->gain * voice->release_gain;
        }
        voice->position += voice->increment;
        if (voice->released) {
            voice->release_gain *= 0.999f;
            if (voice->release_gain < 0.0001f) { voice->active = 0; break; }
        }
    }
}
