#ifndef DSPRESET_WAV_SOURCE_H
#define DSPRESET_WAV_SOURCE_H

#include <stdint.h>

typedef struct {
    int fd;
    uint64_t data_offset, frame_count;
    uint32_t sample_rate;
    uint16_t channels, bits_per_sample, format;
    int has_loop;                   /* the file's own 'smpl' loop, if any */
    uint64_t loop_start, loop_end;  /* frames, loop_end INCLUSIVE (smpl convention) */
} ds_wav_source_t;

/* Worker-only source access. The audio callback never calls these; it reads
 * the resident head and per-voice stream rings the worker fills. */
int ds_wav_source_open(ds_wav_source_t *source, const char *path,
                       char *error, unsigned error_len);
void ds_wav_source_close(ds_wav_source_t *source);

/* Reads up to `frame_count` frames starting at `frame` as interleaved float,
 * in one pread. Returns frames read (clipped at end of data) or -1. */
int ds_wav_source_read_frames(const ds_wav_source_t *source, uint64_t frame,
                              float *interleaved, unsigned frame_count,
                              char *error, unsigned error_len);

#endif
