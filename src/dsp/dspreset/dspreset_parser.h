#ifndef DSPRESET_PARSER_H
#define DSPRESET_PARSER_H

#include <stdint.h>

typedef enum {
    DS_PLAYBACK_AUTO = 0,
    DS_PLAYBACK_MEMORY,
    DS_PLAYBACK_DISK_STREAMING,
} ds_dspreset_playback_mode_t;

typedef struct {
    char path[512];
    int root_note, lo_note, hi_note;
    int lo_vel, hi_vel, seq_position;
    ds_dspreset_playback_mode_t playback_mode;
} ds_dspreset_sample_t;

typedef int (*ds_dspreset_sample_visitor_t)(const ds_dspreset_sample_t *sample, void *context);

/* Reads DSPreset XML directly. This deliberately recognizes sample elements
 * rather than converting XML into SFZ: the callback is the native region
 * handoff to the scheduler/streaming layer. */
int ds_dspreset_visit_samples(const char *preset_path,
                              ds_dspreset_sample_visitor_t visitor, void *context,
                              char *error, unsigned error_len);

#endif
