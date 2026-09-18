#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../src/dsp/dspreset/voice.h"

int main(int argc, char **argv) {
    char error[128] = {0};
    float output[256] = {0};
    ds_wav_source_t source;
    ds_page_cache_t cache;
    ds_dspreset_sample_t region = {.root_note = 35, .lo_note = 35, .hi_note = 35, .lo_vel = 0, .hi_vel = 127};
    ds_voice_t voice;
    assert(argc == 2);
    assert(ds_wav_source_open(&source, argv[1], error, sizeof(error)) == 0);
    ds_page_cache_init(&cache);
    ds_voice_start(&voice, &region, &source, 35, 127, 44100);
    ds_voice_render(&voice, &cache, output, 128);
    assert(ds_page_cache_service(&cache, error, sizeof(error)) > 0);
    memset(output, 0, sizeof(output));
    ds_voice_render(&voice, &cache, output, 128);
    assert(output[0] != 0 || output[1] != 0);
    assert(output[0] == output[1]);
    ds_wav_source_close(&source);
    puts("voice test passed");
    return 0;
}
