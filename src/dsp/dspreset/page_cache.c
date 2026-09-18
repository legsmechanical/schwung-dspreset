#include "page_cache.h"

#include <string.h>

void ds_page_cache_init(ds_page_cache_t *cache) {
    memset(cache, 0, sizeof(*cache));
}

static uint64_t page_base(uint64_t frame) {
    return frame - frame % DS_CACHE_PAGE_FRAMES;
}

static int key_matches(const ds_cache_page_t *page, const ds_wav_source_t *source, uint64_t base) {
    return page->source == source && page->base_frame == base;
}

void ds_page_cache_request(ds_page_cache_t *cache, const ds_wav_source_t *source, uint64_t frame) {
    uint64_t base = page_base(frame);
    if (!cache || !source || source->channels > DS_CACHE_MAX_CHANNELS) return;
    for (unsigned i = 0; i < DS_CACHE_PAGE_SLOTS; ++i) {
        ds_cache_page_t *page = &cache->pages[i];
        unsigned state = atomic_load_explicit(&page->state, memory_order_acquire);
        if ((state == DS_CACHE_READY || state == DS_CACHE_FILLING) && key_matches(page, source, base)) return;
    }
    for (unsigned i = 0; i < DS_CACHE_PAGE_SLOTS; ++i) {
        ds_cache_page_t *page = &cache->pages[i];
        unsigned expected = DS_CACHE_EMPTY;
        if (atomic_compare_exchange_strong_explicit(&page->state, &expected, DS_CACHE_RESERVED,
                                                    memory_order_acq_rel, memory_order_acquire)) {
            page->source = source; page->base_frame = base; page->frames = 0;
            atomic_store_explicit(&page->state, DS_CACHE_FILLING, memory_order_release);
            return;
        }
    }
    /* Eviction is worker-safe only once the audio reader gate is closed. */
    for (unsigned i = 0; i < DS_CACHE_PAGE_SLOTS; ++i) {
        ds_cache_page_t *page = &cache->pages[i];
        unsigned expected = DS_CACHE_READY;
        if (atomic_compare_exchange_strong_explicit(&page->state, &expected, DS_CACHE_RESERVED,
                                                    memory_order_acq_rel, memory_order_acquire)) {
            if (atomic_load_explicit(&page->readers, memory_order_acquire) == 0) {
                page->source = source; page->base_frame = base; page->frames = 0;
                atomic_store_explicit(&page->state, DS_CACHE_FILLING, memory_order_release);
                return;
            }
            atomic_store_explicit(&page->state, DS_CACHE_READY, memory_order_release);
        }
    }
}

unsigned ds_page_cache_service(ds_page_cache_t *cache, char *error, unsigned error_len) {
    unsigned completed = 0;
    if (!cache) return 0;
    for (unsigned i = 0; i < DS_CACHE_PAGE_SLOTS; ++i) {
        ds_cache_page_t *page = &cache->pages[i];
        const ds_wav_source_t *source;
        int frames;
        if (atomic_load_explicit(&page->state, memory_order_acquire) != DS_CACHE_FILLING) continue;
        while (atomic_load_explicit(&page->readers, memory_order_acquire)) { }
        source = page->source;
        if (!source || source->channels > DS_CACHE_MAX_CHANNELS) {
            atomic_store_explicit(&page->state, DS_CACHE_EMPTY, memory_order_release);
            continue;
        }
        frames = ds_wav_source_read_frames(source, page->base_frame, page->data,
                                           DS_CACHE_PAGE_FRAMES, error, error_len);
        if (frames < 0) { atomic_store_explicit(&page->state, DS_CACHE_EMPTY, memory_order_release); continue; }
        page->frames = (unsigned)frames;
        if ((unsigned)frames < DS_CACHE_PAGE_FRAMES) {
            memset(page->data + (size_t)frames * source->channels, 0,
                   ((size_t)DS_CACHE_PAGE_FRAMES - (unsigned)frames) * source->channels * sizeof(float));
        }
        atomic_store_explicit(&page->state, DS_CACHE_READY, memory_order_release);
        completed++;
    }
    return completed;
}

int ds_page_cache_sample(ds_page_cache_t *cache, const ds_wav_source_t *source,
                         uint64_t frame, unsigned channel, float *out) {
    uint64_t base = page_base(frame);
    if (!cache || !source || !out || channel >= source->channels || channel >= DS_CACHE_MAX_CHANNELS) return 0;
    for (unsigned i = 0; i < DS_CACHE_PAGE_SLOTS; ++i) {
        ds_cache_page_t *page = &cache->pages[i];
        if (atomic_load_explicit(&page->state, memory_order_acquire) != DS_CACHE_READY) continue;
        atomic_fetch_add_explicit(&page->readers, 1, memory_order_acquire);
        if (atomic_load_explicit(&page->state, memory_order_acquire) == DS_CACHE_READY &&
            key_matches(page, source, base) && frame - base < page->frames) {
            *out = page->data[(frame - base) * source->channels + channel];
            atomic_fetch_sub_explicit(&page->readers, 1, memory_order_release);
            return 1;
        }
        atomic_fetch_sub_explicit(&page->readers, 1, memory_order_release);
    }
    return 0;
}
