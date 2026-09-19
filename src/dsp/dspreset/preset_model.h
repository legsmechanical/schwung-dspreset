#ifndef DSPRESET_PRESET_MODEL_H
#define DSPRESET_PRESET_MODEL_H

/* Everything in a DSPreset besides its samples that a user can change: the
 * group table (volume, tuning, pan, envelope, enabled, tags), the <ui>
 * controls and the bindings they drive, <midi> CC mappings, and the <effects>
 * list. Parsed on the worker; the engine keeps a runtime copy the bindings
 * write into. */

#include <stdint.h>

#define DS_MAX_TAGS 64
#define DS_MAX_CONTROLS 64
#define DS_MAX_EFFECT_PARAMS 12
#define DS_MAX_TABLE 64
#define DS_MAX_EFFECTS 64
#define DS_MAX_MODULATORS 16

enum { DS_CONTROL_KNOB = 0, DS_CONTROL_BUTTON, DS_CONTROL_MENU };
enum { DS_LEVEL_INSTRUMENT = 0, DS_LEVEL_GROUP, DS_LEVEL_TAG, DS_LEVEL_UI, DS_LEVEL_OTHER };
enum { DS_TRANSLATE_LINEAR = 0, DS_TRANSLATE_TABLE, DS_TRANSLATE_FIXED };
enum {
    DS_TARGET_NONE = 0,
    DS_TARGET_VOLUME, DS_TARGET_TUNING, DS_TARGET_PAN, DS_TARGET_VEL_TRACK,
    DS_TARGET_ATTACK, DS_TARGET_DECAY, DS_TARGET_SUSTAIN, DS_TARGET_RELEASE,
    DS_TARGET_ENABLED,
    DS_TARGET_CONTROL_VALUE,          /* level ui: set another control */
    DS_TARGET_EFFECT,                 /* type effect: an <effects> parameter */
    DS_TARGET_MODULATOR,              /* type modulator: a modulator's own setting */
    DS_TARGET_KEY_TRACK,              /* PITCH_KEY_TRACK */
    DS_TARGET_SILENCING_MODE,         /* 0 fast, 1 normal */
    DS_TARGET_SILENCING_DECAY,
    DS_TARGET_TAG_POLYPHONY,
    /* frame positions and key/velocity ranges, set live on a group or the instrument */
    DS_TARGET_SAMPLE_START, DS_TARGET_SAMPLE_END, DS_TARGET_LOOP_START, DS_TARGET_LOOP_END,
    DS_TARGET_ROOT_NOTE, DS_TARGET_LO_NOTE, DS_TARGET_HI_NOTE, DS_TARGET_LO_VEL, DS_TARGET_HI_VEL,
    DS_TARGET_AMP_ENV_ENABLED,
};

/* How a modulator's value lands on its target (DecentSampler's modBehavior;
 * its default is set). */
enum { DS_MODB_SET = 0, DS_MODB_ADD, DS_MODB_MULTIPLY, DS_MODB_MODULATE };
enum { DS_MOD_LFO = 0, DS_MOD_ENVELOPE, DS_MOD_CC, DS_MOD_VELOCITY };
enum { DS_LFO_SINE = 0, DS_LFO_SQUARE, DS_LFO_SAW, DS_LFO_TRIANGLE };

typedef struct {
    int target, level;
    int position;                     /* group / effect / control index; -1 = by tags */
    int effect;                       /* type effect: index into model.effects, -1 = none (resolved at load) */
    int effect_group, effect_index;   /* as written: groupIndex (-1 = instrument) and effectIndex */
    uint64_t tag_mask;                /* level tag, or groups chosen by tags */
    char name[32];                    /* the DS parameter token, for labels and effects */
    int translation;
    int has_range, reversed, has_factor;
    float out_min, out_max, factor;
    int table_n;
    float table_in[DS_MAX_TABLE], table_out[DS_MAX_TABLE];   /* Capture's cutoff table has 21 */
    float fixed;                      /* fixed_value, numeric (true = 1, "-6dB" -> linear when a volume) */
    int mod_behavior;                 /* bindings under a modulator */
} ds_binding_t;

/* One <lfo>, <envelope>, <midiCC> or <midiVelocity>. The settings here are
 * LIVE: controls bound to them (type="modulator") write into the engine's copy. */
typedef struct {
    int kind, voice_scope, shape, cc;
    float frequency, mod_amount, delay;
    float attack, decay, sustain, release;
    unsigned first_binding, binding_count;
} ds_modulator_t;

typedef struct { char name[32]; unsigned first_binding, binding_count; } ds_choice_t;

typedef struct {
    int kind;
    char name[32];
    float min, max, def;
    int integer;
    unsigned first_binding, binding_count;      /* knob */
    unsigned first_choice, choice_count;        /* button states / menu options */
} ds_control_t;

typedef struct { int cc; unsigned first_binding, binding_count; } ds_cc_map_t;

typedef struct {
    char type[24];
    uint64_t tag_mask;
    int group;                        /* -1 = instrument level */
    int enabled;
    unsigned param_count;
    char param_names[DS_MAX_EFFECT_PARAMS][24];
    float param_values[DS_MAX_EFFECT_PARAMS];
} ds_effect_t;

/* One group's live settings. `has_*` marks a value the GROUP sets itself: an
 * instrument-level binding reaches only groups that do not. */
typedef struct {
    char name[64];
    float volume, tuning, pan, vel_track, env[4];
    int has_pan, has_vel_track, has_env[4];
    float key_track, silencing_decay;
    int silencing_mode;
    int has_key_track, has_silencing_mode, has_silencing_decay;
    /* Set only by bindings; `live` says which (DS_OWN_* bits of dspreset_parser.h). */
    unsigned live;
    int64_t frames[4];                /* start, end, loopStart, loopEnd as written (inclusive ends) */
    int keys[5];                      /* rootNote, loNote, hiNote, loVel, hiVel */
    int amp_env;
    int enabled;
    uint64_t tag_mask;
} ds_group_settings_t;

typedef struct {
    ds_group_settings_t instrument;   /* the <groups> element */
    ds_group_settings_t *groups;
    unsigned group_count;
    char tag_names[DS_MAX_TAGS][32];
    unsigned tag_count;
    /* <tags><tag>: each tag's starting volume, on/off and voice limit (-1 = none) */
    float tag_volume[DS_MAX_TAGS];
    unsigned char tag_enabled[DS_MAX_TAGS];
    int tag_polyphony[DS_MAX_TAGS];
    ds_control_t controls[DS_MAX_CONTROLS];
    unsigned control_count;
    ds_choice_t *choices;
    unsigned choice_count;
    ds_binding_t *bindings;
    unsigned binding_count;
    ds_cc_map_t *ccs;
    unsigned cc_count;
    ds_effect_t *effects;
    unsigned effect_count;
    ds_modulator_t modulators[DS_MAX_MODULATORS];
    unsigned modulator_count;
} ds_preset_model_t;

int ds_preset_model_load(ds_preset_model_t *model, const char *preset_path, char *error, unsigned error_len);
void ds_preset_model_free(ds_preset_model_t *model);

/* Tags are registered by name as they are seen; returns the bit, or 0 when the
 * table is full. Used for sample tags too, so every zone shares one numbering. */
uint64_t ds_preset_model_tag_mask(ds_preset_model_t *model, const char *comma_list);

/* A control's value -> what its binding sends, by the binding's translation.
 * `in_min`/`in_max` is the source's range (a control's, or 0..127 for a CC). */
float ds_binding_translate(const ds_binding_t *binding, float in_min, float in_max, float value);

#endif
