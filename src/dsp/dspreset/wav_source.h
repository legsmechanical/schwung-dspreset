#ifndef DSPRESET_WAV_SOURCE_H
#define DSPRESET_WAV_SOURCE_H

#include <stdint.h>

typedef struct {
    int fd;
    uint64_t data_offset, frame_count;
    uint32_t sample_rate;
    uint16_t channels, bits_per_sample, format;   /* format: 1 PCM, 3 float */
    int big_endian;                                /* AIFF (and AIFF-C "NONE"/"twos"/"fl32") */
    int has_loop;                   /* the file's own 'smpl' loop, if any */
    uint64_t loop_start, loop_end;  /* frames, loop_end INCLUSIVE (smpl convention) */
    struct ds_flac *flac;           /* a FLAC file's decoder (it has a position: one per reader) */
} ds_wav_source_t;

/* WAV (PCM 16/24/32, float 32, EXTENSIBLE), AIFF / AIFF-C (PCM 8-32 big-
 * endian, "sowt" little-endian, "fl32" float) and FLAC — the formats
 * DecentSampler reads. A file's own loop (WAV 'smpl', AIFF INST sustain loop)
 * is reported so a zone without loop attributes can use it; FLAC carries none.
 * A FLAC source must not be copied to read with: its decoder is its own.
 *
 * Worker-only source access. The audio callback never calls these; it reads
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
