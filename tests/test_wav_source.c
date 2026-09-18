/* The WAV reader against files it must read exactly, including a 'smpl' loop
 * written after 'data'. */
#include "test_support.h"

#include "../src/dsp/dspreset/wav_source.h"

int main(void) {
    const char *dir = getenv("TEST_TMP");
    char path[512], error[128] = {0};
    static float frames[70000 * 2];
    ds_wav_source_t source;
    CHECK(dir);
    snprintf(path, sizeof(path), "%s/stereo_loop.wav", dir);
    write_wav24(path, 48000, 2, 70001, 40000, 49999);   /* odd size: exercises the pad byte */
    CHECK(ds_wav_source_open(&source, path, error, sizeof(error)) == 0);
    CHECK(source.sample_rate == 48000 && source.channels == 2 && source.bits_per_sample == 24);
    CHECK(source.frame_count == 70001);
    CHECK(source.has_loop && source.loop_start == 40000 && source.loop_end == 49999);
    /* a read larger than one internal chunk, from an offset */
    CHECK(ds_wav_source_read_frames(&source, 5, frames, 70000, error, sizeof(error)) == 69996);
    for (unsigned i = 0; i < 69996; ++i)
        for (unsigned c = 0; c < 2; ++c)
            CHECK(frames[i * 2 + c] == test_signal24(i + 5, c) / 8388608.0f);
    ds_wav_source_close(&source);

    snprintf(path, sizeof(path), "%s/missing.wav", dir);
    CHECK(ds_wav_source_open(&source, path, error, sizeof(error)) != 0);
    CHECK(strstr(error, "missing"));
    puts("wav source test passed");
    return 0;
}
