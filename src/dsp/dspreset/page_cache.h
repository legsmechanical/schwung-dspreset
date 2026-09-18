#ifndef DSPRESET_PAGE_CACHE_H
#define DSPRESET_PAGE_CACHE_H

#include "wav_source.h"

#include <stdatomic.h>
#include <stdint.h>

#define DS_CACHE_PAGE_FRAMES 2048
#define DS_CACHE_PAGE_SLOTS 64
#define DS_CACHE_MAX_CHANNELS 2

typedef enum { DS_CACHE_EMPTY = 0, DS_CACHE_RESERVED, DS_CACHE_FILLING, DS_CACHE_READY } ds_cache_state_t;

typedef struct {
    _Atomic unsigned state;
    _Atomic unsigned readers;
    const ds_wav_source_t *source;
    uint64_t base_frame;
    unsigned frames;
    float data[DS_CACHE_PAGE_FRAMES * DS_CACHE_MAX_CHANNELS];
} ds_cache_page_t;

typedef struct { ds_cache_page_t pages[DS_CACHE_PAGE_SLOTS]; } ds_page_cache_t;

void ds_page_cache_init(ds_page_cache_t *cache);

/* Audio-safe: queues a page by claiming an empty/evictable slot. It performs
 * no I/O and returns immediately when the cache is temporarily full. */
void ds_page_cache_request(ds_page_cache_t *cache, const ds_wav_source_t *source,
                           uint64_t frame);

/* Worker-only: executes pending reads. It may block on storage but never
 * shares a write buffer with an audio reader. */
unsigned ds_page_cache_service(ds_page_cache_t *cache, char *error, unsigned error_len);

/* Audio-safe sample lookup. Returns zero on a page miss (the caller should
 * output silence for that frame while the prefetch worker catches up). */
int ds_page_cache_sample(ds_page_cache_t *cache, const ds_wav_source_t *source,
                         uint64_t frame, unsigned channel, float *out);

#endif
