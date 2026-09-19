#ifndef DSPRESET_FLAC_SOURCE_H
#define DSPRESET_FLAC_SOURCE_H

#include <stdint.h>

/* FLAC samples, through dr_flac (public domain / MIT-0; notice in
 * THIRD_PARTY.md), reading from a descriptor we own so the engine's count of
 * open files stays what it was. A decoder has a position: reads are cheapest
 * in order, and a read elsewhere seeks first. Worker only, like every file
 * read. */

typedef struct ds_flac ds_flac_t;

/* 0 and the stream's shape, or -1 (not FLAC, or unreadable). `fd` stays the caller's. */
ds_flac_t *ds_flac_open(int fd, uint32_t *sample_rate, uint16_t *channels, uint16_t *bits, uint64_t *frames);
/* Up to `count` frames from `frame` as interleaved float; frames read, or -1. */
int ds_flac_read(ds_flac_t *flac, uint64_t frame, float *interleaved, unsigned count);
void ds_flac_close(ds_flac_t *flac);

#endif
