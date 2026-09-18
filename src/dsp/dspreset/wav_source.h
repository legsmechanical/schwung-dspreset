#ifndef DSPRESET_WAV_SOURCE_H
#define DSPRESET_WAV_SOURCE_H

#include <stdint.h>

typedef struct {
    int fd;
    uint64_t data_offset, frame_count;
    uint32_t sample_rate;
    uint16_t channels, bits_per_sample, format;
} ds_wav_source_t;

/* Worker-only source access. The audio callback receives decoded cache pages,
 * never calls either function below. */
int ds_wav_source_open(ds_wav_source_t *source, const char *path,
                       char *error, unsigned error_len);
void ds_wav_source_close(ds_wav_source_t *source);
int ds_wav_source_read_frames(const ds_wav_source_t *source, uint64_t frame,
                              float *interleaved, unsigned frame_count,
                              char *error, unsigned error_len);

#endif
