#include "dspreset_parser.h"

#include "library_input.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_PRESET_BYTES (16 * 1024 * 1024)

static void fail(char *out, unsigned n, const char *message) {
    if (n) snprintf(out, n, "%s", message);
}

static const char *attribute(const char *tag, const char *end, const char *name,
                             char *out, size_t out_size) {
    size_t name_len = strlen(name);
    const char *p;
    for (p = tag; p + name_len + 1 < end; ++p) {
        const char *value;
        char quote;
        if (strncmp(p, name, name_len) ||
            (p != tag && !isspace((unsigned char)p[-1])) || p[name_len] != '=') continue;
        value = p + name_len + 1;
        if (value >= end || (*value != '\'' && *value != '"')) return NULL;
        quote = *value++;
        p = value;
        while (p < end && *p != quote) ++p;
        if (p == end || (size_t)(p - value) >= out_size) return NULL;
        memcpy(out, value, (size_t)(p - value));
        out[p - value] = '\0';
        return out;
    }
    return NULL;
}

static int number_or(const char *tag, const char *end, const char *name, int fallback) {
    char value[32];
    char *tail;
    const char *found = attribute(tag, end, name, value, sizeof(value));
    long result;
    if (!found) return fallback;
    result = strtol(found, &tail, 10);
    return *tail ? fallback : (int)result;
}

static ds_dspreset_playback_mode_t playback_or(const char *tag, const char *end) {
    char value[32];
    if (!attribute(tag, end, "playbackMode", value, sizeof(value))) return DS_PLAYBACK_AUTO;
    if (!strcmp(value, "memory")) return DS_PLAYBACK_MEMORY;
    if (!strcmp(value, "disk_streaming")) return DS_PLAYBACK_DISK_STREAMING;
    return DS_PLAYBACK_AUTO;
}

int ds_dspreset_visit_samples(const char *preset_path,
                              ds_dspreset_sample_visitor_t visitor, void *context,
                              char *error, unsigned error_len) {
    FILE *file;
    long length;
    char *xml, *tag;
    unsigned sample_count = 0;
    int rc = -1;
    if (!preset_path || !visitor) return -1;
    file = fopen(preset_path, "rb");
    if (!file || fseek(file, 0, SEEK_END) || (length = ftell(file)) < 0 ||
        length > MAX_PRESET_BYTES || fseek(file, 0, SEEK_SET)) {
        fail(error, error_len, "cannot read DSPreset");
        if (file) fclose(file);
        return -1;
    }
    xml = malloc((size_t)length + 1);
    if (!xml || fread(xml, 1, (size_t)length, file) != (size_t)length) {
        fail(error, error_len, "cannot read DSPreset");
        free(xml); fclose(file); return -1;
    }
    fclose(file); xml[length] = '\0';
    for (tag = xml; (tag = strstr(tag, "<sample")) != NULL; tag += 7) {
        char path[512];
        char *end = strchr(tag, '>');
        ds_dspreset_sample_t sample;
        if (!end || (tag[7] && !isspace((unsigned char)tag[7]) && tag[7] != '>' && tag[7] != '/')) {
            continue;
        }
        if (!attribute(tag + 7, end, "path", path, sizeof(path)) ||
            !ds_library_archive_entry_is_safe(path)) {
            fail(error, error_len, "DSPreset has unsafe or missing sample path"); goto done;
        }
        memset(&sample, 0, sizeof(sample));
        memcpy(sample.path, path, strlen(path) + 1);
        sample.root_note = number_or(tag + 7, end, "rootNote", 60);
        sample.lo_note = number_or(tag + 7, end, "loNote", sample.root_note);
        sample.hi_note = number_or(tag + 7, end, "hiNote", sample.root_note);
        sample.lo_vel = number_or(tag + 7, end, "loVel", 0);
        sample.hi_vel = number_or(tag + 7, end, "hiVel", 127);
        sample.seq_position = number_or(tag + 7, end, "seqPosition", 1);
        sample.playback_mode = playback_or(tag + 7, end);
        if (sample.lo_note < 0 || sample.hi_note > 127 || sample.lo_note > sample.hi_note ||
            sample.lo_vel < 0 || sample.hi_vel > 127 || sample.lo_vel > sample.hi_vel) {
            fail(error, error_len, "DSPreset has invalid sample range"); goto done;
        }
        if (visitor(&sample, context)) { rc = 0; goto done; }
        sample_count++;
    }
    if (!sample_count) {
        fail(error, error_len, "DSPreset contains no samples");
        goto done;
    }
    rc = 0;
done:
    free(xml);
    return rc;
}
