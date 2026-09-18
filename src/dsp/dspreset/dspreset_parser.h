#ifndef DSPRESET_PARSER_H
#define DSPRESET_PARSER_H

#include <stdint.h>

#include "playback_policy.h"

enum { DS_SEQ_ALWAYS = 0, DS_SEQ_ROUND_ROBIN, DS_SEQ_RANDOM };
enum { DS_TRIGGER_ATTACK = 0, DS_TRIGGER_RELEASE };

/* One playable zone with every inheritable attribute already resolved
 * (<groups> -> <group> -> <sample>), so nothing downstream consults XML. */
typedef struct {
    char path[512];                 /* relative to the preset's directory, '/' separators */
    int root_note, lo_note, hi_note;
    int lo_vel, hi_vel;
    int seq_mode, seq_position;
    int trigger;
    int group_index;
    double tuning;                  /* semitones: tuning + groupTuning */
    float gain;                     /* linear: groups x group x sample volume */
    float pan;                      /* -1 .. 1 */
    float amp_vel_track;            /* 0 .. 1 */
    int amp_env_enabled;
    float attack, decay, sustain, release;  /* seconds; sustain 0..1 */
    int64_t start, end;             /* frames; -1 = not set */
    int loop_enabled;               /* 1 / 0, or -1 = not set (use the file's own loop) */
    int64_t loop_start, loop_end;   /* frames; -1 = not set */
    ds_playback_mode_t playback_mode;
} ds_dspreset_sample_t;

typedef int (*ds_dspreset_sample_visitor_t)(const ds_dspreset_sample_t *sample, void *context);

/* Reads DSPreset XML directly and calls `visitor` once per <sample> inside an
 * enabled <group>. Returns 0 when at least one sample was visited. */
int ds_dspreset_visit_samples(const char *preset_path,
                              ds_dspreset_sample_visitor_t visitor, void *context,
                              char *error, unsigned error_len);

/* Exposed for tests: finds `name` in one tag's attribute text, tolerating
 * whitespace around '=' and decoding the five XML entities. */
int ds_xml_attribute(const char *attrs, const char *end, const char *name,
                     char *out, unsigned out_size);

#endif
