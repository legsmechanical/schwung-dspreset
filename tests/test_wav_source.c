#include <assert.h>
#include <stdio.h>

#include "../src/dsp/dspreset/wav_source.h"

int main(int argc, char **argv) {
    char error[128] = {0};
    float frames[128 * 2] = {0};
    ds_wav_source_t source;
    assert(argc == 2);
    assert(ds_wav_source_open(&source, argv[1], error, sizeof(error)) == 0);
    assert(source.sample_rate == 44100 && source.channels == 1 && source.bits_per_sample == 24);
    assert(ds_wav_source_read_frames(&source, 0, frames, 128, error, sizeof(error)) == 128);
    ds_wav_source_close(&source);
    puts("wav source test passed");
    return 0;
}
