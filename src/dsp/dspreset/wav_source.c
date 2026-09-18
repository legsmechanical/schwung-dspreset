#include "wav_source.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define READ_CHUNK_BYTES 65536

static uint16_t le16(const unsigned char *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t le32(const unsigned char *p) { return (uint32_t)le16(p) | ((uint32_t)le16(p + 2) << 16); }
static void fail(char *out, unsigned n, const char *message) { if (n) snprintf(out, n, "%s", message); }

static int encoding_supported(const ds_wav_source_t *s) {
    return (s->format == 1 && (s->bits_per_sample == 16 || s->bits_per_sample == 24 || s->bits_per_sample == 32)) ||
           (s->format == 3 && s->bits_per_sample == 32);
}

int ds_wav_source_open(ds_wav_source_t *source, const char *path,
                       char *error, unsigned error_len) {
    unsigned char header[12], chunk[8], body[64];
    uint64_t offset = 12;
    int have_format = 0, have_data = 0;
    uint32_t data_size = 0;
    if (!source || !path) return -1;
    memset(source, 0, sizeof(*source));
    source->fd = open(path, O_RDONLY);
    if (source->fd < 0) { fail(error, error_len, "sample file missing"); return -1; }
    if (read(source->fd, header, sizeof(header)) != (ssize_t)sizeof(header) ||
        memcmp(header, "RIFF", 4) || memcmp(header + 8, "WAVE", 4)) {
        fail(error, error_len, "not a RIFF/WAVE file"); ds_wav_source_close(source); return -1;
    }
    /* Walk every chunk: 'smpl' commonly follows 'data'. */
    while (offset < UINT32_MAX && pread(source->fd, chunk, sizeof(chunk), (off_t)offset) == (ssize_t)sizeof(chunk)) {
        uint32_t size = le32(chunk + 4);
        uint64_t body_at = offset + 8;
        if (!memcmp(chunk, "fmt ", 4)) {
            unsigned n = size < sizeof(body) ? size : sizeof(body);
            if (size < 16 || pread(source->fd, body, n, (off_t)body_at) != (ssize_t)n) break;
            source->format = le16(body);
            source->channels = le16(body + 2);
            source->sample_rate = le32(body + 4);
            source->bits_per_sample = le16(body + 14);
            if (source->format == 0xFFFE && size >= 26) source->format = le16(body + 24); /* EXTENSIBLE */
            have_format = 1;
        } else if (!memcmp(chunk, "data", 4)) {
            source->data_offset = body_at;
            data_size = size;
            have_data = 1;
        } else if (!memcmp(chunk, "smpl", 4) && size >= 36 + 24) {
            unsigned char loop[36 + 24];
            if (pread(source->fd, loop, sizeof(loop), (off_t)body_at) == (ssize_t)sizeof(loop) && le32(loop + 28) > 0) {
                source->loop_start = le32(loop + 36 + 8);
                source->loop_end = le32(loop + 36 + 12);
                source->has_loop = source->loop_end > source->loop_start;
            }
        }
        offset = body_at + size + (size & 1);
    }
    if (!have_format || !have_data || !source->channels || !encoding_supported(source)) {
        fail(error, error_len, "unsupported or malformed WAVE file");
        ds_wav_source_close(source);
        return -1;
    }
    source->frame_count = data_size / (source->channels * (source->bits_per_sample / 8u));
    if (source->has_loop && source->loop_end >= source->frame_count) {
        if (source->loop_start + 1 < source->frame_count) source->loop_end = source->frame_count - 1;
        else source->has_loop = 0;
    }
    return 0;
}

void ds_wav_source_close(ds_wav_source_t *source) {
    if (source && source->fd >= 0) close(source->fd);
    if (source) source->fd = -1;
}

int ds_wav_source_read_frames(const ds_wav_source_t *source, uint64_t frame,
                              float *interleaved, unsigned frame_count,
                              char *error, unsigned error_len) {
    unsigned char bytes[READ_CHUNK_BYTES];
    unsigned bytes_per_sample, bytes_per_frame, done = 0;
    if (!source || source->fd < 0 || !interleaved || frame >= source->frame_count) return -1;
    if (frame_count > source->frame_count - frame) frame_count = (unsigned)(source->frame_count - frame);
    bytes_per_sample = source->bits_per_sample / 8u;
    bytes_per_frame = source->channels * bytes_per_sample;
    while (done < frame_count) {
        unsigned frames = frame_count - done, samples;
        const unsigned char *b = bytes;
        float *out = interleaved + (size_t)done * source->channels;
        if (frames > sizeof(bytes) / bytes_per_frame) frames = sizeof(bytes) / bytes_per_frame;
        if (pread(source->fd, bytes, (size_t)frames * bytes_per_frame,
                  (off_t)(source->data_offset + (frame + done) * bytes_per_frame)) !=
            (ssize_t)((size_t)frames * bytes_per_frame)) {
            fail(error, error_len, "cannot read WAVE frames"); return -1;
        }
        samples = frames * source->channels;
        if (source->format == 3) {
            for (unsigned i = 0; i < samples; ++i, b += 4) memcpy(&out[i], b, 4);
        } else if (bytes_per_sample == 2) {
            for (unsigned i = 0; i < samples; ++i, b += 2) out[i] = (int16_t)le16(b) / 32768.0f;
        } else if (bytes_per_sample == 3) {
            for (unsigned i = 0; i < samples; ++i, b += 3) {
                int32_t v = (int32_t)((uint32_t)b[0] << 8 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 24) >> 8;
                out[i] = v / 8388608.0f;
            }
        } else {
            for (unsigned i = 0; i < samples; ++i, b += 4) out[i] = (int32_t)le32(b) / 2147483648.0f;
        }
        done += frames;
    }
    return (int)frame_count;
}
