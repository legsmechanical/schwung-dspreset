#include <assert.h>
#include <stdio.h>

#include "../src/dsp/dspreset/page_cache.h"

int main(int argc, char **argv) {
    char error[128] = {0};
    float cached = 0, direct[1] = {0};
    ds_wav_source_t source;
    ds_page_cache_t cache;
    assert(argc == 2);
    assert(ds_wav_source_open(&source, argv[1], error, sizeof(error)) == 0);
    ds_page_cache_init(&cache);
    assert(!ds_page_cache_sample(&cache, &source, 0, 0, &cached));
    ds_page_cache_request(&cache, &source, 0);
    assert(ds_page_cache_service(&cache, error, sizeof(error)) == 1);
    assert(ds_page_cache_sample(&cache, &source, 0, 0, &cached));
    assert(ds_wav_source_read_frames(&source, 0, direct, 1, error, sizeof(error)) == 1);
    assert(cached == direct[0]);
    ds_wav_source_close(&source);
    puts("page cache test passed");
    return 0;
}
