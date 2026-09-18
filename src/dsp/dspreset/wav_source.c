#include "wav_source.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define WAV_HEADER_LIMIT (1024 * 1024)

static uint16_t le16(const unsigned char *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t le32(const unsigned char *p) { return (uint32_t)le16(p) | ((uint32_t)le16(p + 2) << 16); }
static void fail(char *out, unsigned n, const char *message) { if (n) snprintf(out, n, "%s", message); }

int ds_wav_source_open(ds_wav_source_t *source, const char *path,
                       char *error, unsigned error_len) {
    unsigned char header[12], chunk[8], format[40];
    uint64_t offset = 12;
    int have_format = 0;
    if (!source || !path) return -1;
    memset(source, 0, sizeof(*source)); source->fd = -1;
    source->fd = open(path, O_RDONLY);
    if (source->fd < 0 || read(source->fd, header, sizeof(header)) != (ssize_t)sizeof(header) ||
        memcmp(header, "RIFF", 4) || memcmp(header + 8, "WAVE", 4)) {
        fail(error, error_len, "not a RIFF/WAVE file"); ds_wav_source_close(source); return -1;
    }
    while (offset < WAV_HEADER_LIMIT && read(source->fd, chunk, sizeof(chunk)) == (ssize_t)sizeof(chunk)) {
        uint32_t size = le32(chunk + 4);
        offset += 8;
        if (!memcmp(chunk, "fmt ", 4)) {
            if (size < 16 || size > sizeof(format) || read(source->fd, format, size) != (ssize_t)size) break;
            source->format = le16(format);
            source->channels = le16(format + 2);
            source->sample_rate = le32(format + 4);
            source->bits_per_sample = le16(format + 14);
            have_format = 1;
        } else if (!memcmp(chunk, "data", 4)) {
            if (!have_format || !source->channels || !source->bits_per_sample ||
                (source->format != 1 && source->format != 3)) break;
            source->data_offset = offset;
            source->frame_count = size / (source->channels * (source->bits_per_sample / 8));
            return 0;
        } else if (lseek(source->fd, (off_t)size, SEEK_CUR) < 0) break;
        offset += size;
        if (size & 1) { if (lseek(source->fd, 1, SEEK_CUR) < 0) break; offset++; }
    }
    fail(error, error_len, "unsupported or malformed WAVE file");
    ds_wav_source_close(source);
    return -1;
}

void ds_wav_source_close(ds_wav_source_t *source) {
    if (source && source->fd >= 0) close(source->fd);
    if (source) source->fd = -1;
}

int ds_wav_source_read_frames(const ds_wav_source_t *source, uint64_t frame,
                              float *interleaved, unsigned frame_count,
                              char *error, unsigned error_len) {
    unsigned bytes_per_sample, bytes_per_frame;
    unsigned char bytes[4];
    if (!source || source->fd < 0 || !interleaved || frame >= source->frame_count) return -1;
    if (frame_count > source->frame_count - frame) frame_count = (unsigned)(source->frame_count - frame);
    bytes_per_sample = source->bits_per_sample / 8;
    bytes_per_frame = source->channels * bytes_per_sample;
    if ((source->format != 1 && source->format != 3) ||
        (bytes_per_sample != 2 && bytes_per_sample != 3 && bytes_per_sample != 4)) {
        fail(error, error_len, "unsupported WAVE encoding"); return -1;
    }
    for (unsigned i = 0; i < frame_count; ++i) for (unsigned channel = 0; channel < source->channels; ++channel) {
        off_t position = (off_t)(source->data_offset + (frame + i) * bytes_per_frame + channel * bytes_per_sample);
        if (pread(source->fd, bytes, bytes_per_sample, position) != (ssize_t)bytes_per_sample) {
            fail(error, error_len, "cannot read WAVE frame"); return -1;
        }
        if (source->format == 1 && bytes_per_sample == 2) {
            int16_t value = (int16_t)le16(bytes);
            interleaved[i * source->channels + channel] = value / 32768.0f;
        } else if (source->format == 1 && bytes_per_sample == 3) {
            int32_t value = (int32_t)bytes[0] | ((int32_t)bytes[1] << 8) | ((int32_t)bytes[2] << 16);
            if (value & 0x800000) value |= ~0xffffff;
            interleaved[i * source->channels + channel] = value / 8388608.0f;
        } else if (source->format == 3 && bytes_per_sample == 4) {
            float value; memcpy(&value, bytes, sizeof(value));
            interleaved[i * source->channels + channel] = value;
        } else { fail(error, error_len, "unsupported WAVE encoding"); return -1; }
    }
    return (int)frame_count;
}
