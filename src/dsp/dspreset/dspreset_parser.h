#ifndef DSPRESET_PARSER_H
#define DSPRESET_PARSER_H

#include <stdint.h>

#include "playback_policy.h"

enum { DS_SEQ_ALWAYS = 0, DS_SEQ_ROUND_ROBIN, DS_SEQ_RANDOM };
enum { DS_TRIGGER_ATTACK = 0, DS_TRIGGER_RELEASE, DS_TRIGGER_FIRST, DS_TRIGGER_LEGATO, DS_TRIGGER_CONTINUOUS };
enum { DS_GLIDE_OFF = 0, DS_GLIDE_ALWAYS, DS_GLIDE_LEGATO };
#define DS_MAX_PREVIOUS 16
#define DS_NO_INTERVAL (-1000)

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
    float pitch_key_track;          /* 0..1: 0 = every key plays the root pitch */
    int64_t loop_crossfade;         /* frames faded across the loop point; 0 = off */
    int loop_crossfade_equal_power; /* 1 = equal_power (the default), 0 = linear */
    char silenced_by[256];          /* silencedByTags: a sample carrying one of these stops this one */
    int silencing_mode;             /* DS_SILENCE_FAST / DS_SILENCE_NORMAL */
    float silencing_decay;          /* seconds; > 0 overrides the mode */
    int previous_notes[DS_MAX_PREVIOUS], previous_count;   /* previousNotes: only after one of these */
    int legato_interval;            /* only when the note is this far from the previous one; DS_NO_INTERVAL = any */
    float glide_time;               /* seconds */
    int glide_mode;                 /* DS_GLIDE_* */
    float release_decay;            /* releaseTriggerDecay: dB per second held, or (linear) gain lost per second */
    int release_decay_db;

    /* For live controls: what the <sample> element sets ITSELF, so a group- or
     * instrument-level binding can override everything else without touching
     * it. `own_volume` is the sample's own volume alone (linear, default 1);
     * `base_tuning` excludes every groupTuning, which bindings may move. */
    unsigned own_mask;
    float own_volume;
    double base_tuning;
    char tags[256];                 /* group tags + sample tags, comma-separated */
} ds_dspreset_sample_t;

enum {
    DS_OWN_PAN = 1u << 0, DS_OWN_VEL_TRACK = 1u << 1, DS_OWN_ATTACK = 1u << 2,
    DS_OWN_DECAY = 1u << 3, DS_OWN_SUSTAIN = 1u << 4, DS_OWN_RELEASE = 1u << 5,
    DS_OWN_KEY_TRACK = 1u << 6, DS_OWN_SILENCING_MODE = 1u << 7, DS_OWN_SILENCING_DECAY = 1u << 8,
    /* Not "own" bits: which of these a BINDING has set on a group or the
     * instrument (ds_group_settings_t.live). A binding wins over the sample's
     * own value here — nearly every sample sets its own key range and root. */
    DS_OWN_START = 1u << 9, DS_OWN_END = 1u << 10, DS_OWN_LOOP_START = 1u << 11, DS_OWN_LOOP_END = 1u << 12,
    DS_OWN_ROOT = 1u << 13, DS_OWN_LO_NOTE = 1u << 14, DS_OWN_HI_NOTE = 1u << 15,
    DS_OWN_LO_VEL = 1u << 16, DS_OWN_HI_VEL = 1u << 17, DS_OWN_AMP_ENV = 1u << 18,
    DS_OWN_GLIDE_TIME = 1u << 19, DS_OWN_GLIDE_MODE = 1u << 20,
};

/* "60", or a note name ("C3" is 60, as JUCE names them; "F#2", "Db-1"): DS_NO_NOTE if neither. */
#define DS_NO_NOTE (-1000)
int ds_note_number(const char *text);

enum { DS_SILENCE_FAST = 0, DS_SILENCE_NORMAL };

typedef int (*ds_dspreset_sample_visitor_t)(const ds_dspreset_sample_t *sample, void *context);

/* Reads DSPreset XML directly and calls `visitor` once per <sample> inside a
 * <group> — disabled groups included (see preset_model for whether it sounds). Returns 0 when at least one sample was visited. */
int ds_dspreset_visit_samples(const char *preset_path,
                              ds_dspreset_sample_visitor_t visitor, void *context,
                              char *error, unsigned error_len);

/* Exposed for tests: finds `name` in one tag's attribute text, tolerating
 * whitespace around '=' and decoding the five XML entities. */
int ds_xml_attribute(const char *attrs, const char *end, const char *name,
                     char *out, unsigned out_size);

#endif
