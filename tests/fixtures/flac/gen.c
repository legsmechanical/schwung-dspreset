/* Writes the WAVs the FLAC fixtures are encoded from (see README.md):
 * test_signal24, 40000 frames, as 16-bit mono and 24-bit stereo. */
#include "../../test_support.h"

static void write_wav16(const char *path, unsigned frames) {
    FILE *f = fopen(path, "wb");
    CHECK(f);
    fwrite("RIFF", 1, 4, f); put32(f, 36 + frames * 2); fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f); put32(f, 16); put16(f, 1); put16(f, 1); put32(f, 44100); put32(f, 88200); put16(f, 2); put16(f, 16);
    fwrite("data", 1, 4, f); put32(f, frames * 2);
    for (unsigned i = 0; i < frames; ++i) put16(f, (uint16_t)(int16_t)(test_signal24(i, 0) >> 8));
    fclose(f);
}

int main(int argc, char **argv) {
    CHECK(argc == 3);
    write_wav16(argv[1], 40000);
    write_wav24(argv[2], 44100, 2, 40000, -1, -1);
    return 0;
}
