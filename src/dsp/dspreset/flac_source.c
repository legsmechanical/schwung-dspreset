#include "flac_source.h"

#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#define DR_FLAC_IMPLEMENTATION
#define DR_FLAC_NO_STDIO
#define DR_FLAC_NO_OGG
#define DR_FLAC_NO_WCHAR
#include "third_party/dr_flac.h"

struct ds_flac {
    int fd;
    int64_t pos, size;
    drflac *decoder;
};

static size_t on_read(void *user, void *out, size_t bytes) {
    ds_flac_t *f = user;
    ssize_t got = pread(f->fd, out, bytes, (off_t)f->pos);
    if (got <= 0) return 0;
    f->pos += got;
    return (size_t)got;
}

static drflac_bool32 on_seek(void *user, int offset, drflac_seek_origin origin) {
    ds_flac_t *f = user;
    int64_t to = origin == DRFLAC_SEEK_SET ? offset : origin == DRFLAC_SEEK_CUR ? f->pos + offset : f->size + offset;
    if (to < 0 || to > f->size) return DRFLAC_FALSE;       /* dr_flac may ask past the end: refuse */
    f->pos = to;
    return DRFLAC_TRUE;
}

static drflac_bool32 on_tell(void *user, drflac_int64 *cursor) {
    *cursor = ((ds_flac_t *)user)->pos;
    return DRFLAC_TRUE;
}

ds_flac_t *ds_flac_open(int fd, uint32_t *sample_rate, uint16_t *channels, uint16_t *bits, uint64_t *frames) {
    ds_flac_t *f;
    struct stat st;
    if (fd < 0 || fstat(fd, &st) || !(f = calloc(1, sizeof(*f)))) return NULL;
    f->fd = fd;
    f->size = st.st_size;
    if (!(f->decoder = drflac_open(on_read, on_seek, on_tell, f, NULL)) || !f->decoder->channels) {
        if (f->decoder) drflac_close(f->decoder);
        free(f);
        return NULL;
    }
    *sample_rate = f->decoder->sampleRate;
    *channels = f->decoder->channels;
    *bits = f->decoder->bitsPerSample;
    *frames = f->decoder->totalPCMFrameCount;
    return f;
}

int ds_flac_read(ds_flac_t *f, uint64_t frame, float *interleaved, unsigned count) {
    drflac_uint64 got;
    if (!f) return -1;
    if (f->decoder->currentPCMFrame != frame && !drflac_seek_to_pcm_frame(f->decoder, frame)) return -1;
    got = drflac_read_pcm_frames_f32(f->decoder, count, interleaved);
    return (int)got;
}

void ds_flac_close(ds_flac_t *f) {
    if (!f) return;
    drflac_close(f->decoder);
    free(f);
}
