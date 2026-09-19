#ifndef DSPRESET_NATIVE_ENGINE_H
#define DSPRESET_NATIVE_ENGINE_H

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#include "dspreset_parser.h"
#include "effects.h"
#include "reverb.h"
#include "chorus.h"
#include "delay.h"
#include "preset_model.h"
#include "wav_source.h"

#define DS_MAX_ZONES 8192
#define DS_MAX_VOICES 64
/* Every source keeps its first DS_HEAD_FRAMES resident, so a note starts from
 * memory the instant it is triggered; the worker streams the rest. ~370 ms at
 * 44.1 kHz is far longer than one worker pass. */
#define DS_HEAD_FRAMES 16384
/* A file this short is simply kept whole. */
#define DS_RESIDENT_FRAMES (2 * DS_HEAD_FRAMES)
#define DS_RING_FRAMES 16384            /* per voice, power of two */
#define DS_FILL_FRAMES 4096             /* one worker read */
#define DS_VOICE_FX 8                   /* group effects a note carries */

typedef struct {
    ds_wav_source_t file;
    float *head;                        /* interleaved, head_frames x channels */
    uint64_t head_frames;               /* == file.frame_count when resident */
    int resident;
} ds_source_t;

/* A zone with its frame bounds resolved against the file. */
typedef struct {
    ds_dspreset_sample_t def;
    int source;                         /* -1: sample file missing/unreadable */
    uint64_t start, end;                /* end exclusive */
    int loop;
    uint64_t loop_start, loop_end;      /* loop_end exclusive */
    int streams;                        /* needs frames beyond the resident head */
    uint64_t tag_mask;                  /* in the model's tag numbering */
} ds_zone_t;

enum { DS_ENV_ATTACK, DS_ENV_DECAY, DS_ENV_SUSTAIN, DS_ENV_RELEASE, DS_ENV_DONE };

/* One modulator's running state: an LFO's phase (and its start delay), or an
 * envelope's stage and level. Held per note for voice-scope modulators and
 * once in the engine for global ones. Advanced once per block. */
typedef struct { float phase, level, delay_left; int stage; } ds_mod_state_t;

/* The effect settings a modulated effect's coefficients were last built from:
 * rebuilt only when one of them has actually moved (a held envelope sits still,
 * an LFO at zero depth never moves). */
typedef struct { float values[DS_MAX_EFFECT_PARAMS]; unsigned count; int enabled, valid; } ds_fx_built_t;

/* Stream word shared with the worker: generation (16) | zone+1 (16) | produced (32).
 * `produced` is the virtual frame (along the looped play path) the ring holds up
 * to; a zone of 0 means nothing to stream. */
typedef struct {
    _Atomic uint64_t stream;
    _Atomic uint32_t consumed;          /* lowest virtual frame the voice still needs */
    float *ring;                        /* DS_RING_FRAMES x channels */

    /* audio-thread only */
    const ds_zone_t *zone;
    const ds_source_t *src;
    double pos, inc;                    /* virtual frame; frames per output frame */
    float gain_l, gain_r;
    int env_stage;
    float env_level, env_attack_step, env_decay_coef, env_release_coef, env_sustain;
    int note, velocity, active, key_down, sustained, one_shot;
    uint32_t age, generation;
    uint32_t underruns;
    float vel;                          /* velocity 0..1, re-applied as settings move */
    uint32_t note_id;                   /* which note-on started it: its layers share one */
    int choked;                         /* stolen by the note limit: fading out, not counted */
    /* Its group's effects, fresh per note as in DecentSampler. */
    unsigned fx_count;
    unsigned char fx_index[DS_VOICE_FX];
    ds_fx_state_t fx_state[DS_VOICE_FX];
    ds_fx_coeffs_t fx_live[DS_VOICE_FX];    /* this note's coefficients when a modulator moves the effect */
    ds_fx_built_t fx_built[DS_VOICE_FX];
    ds_mod_state_t mods[DS_MAX_MODULATORS];
} ds_voice_t;

typedef struct {
    ds_source_t *sources;
    char (*source_paths)[1600];         /* full path: preset dir + zone path */
    unsigned source_count;
    ds_zone_t *zones;
    unsigned zone_count, missing_zones, missing_files;
    unsigned group_count;
    unsigned char *group_len;           /* scratch for round-robin, audio thread */
    ds_voice_t voices[DS_MAX_VOICES];
    unsigned output_rate;
    uint32_t age_counter, rng;
    uint32_t rr_counter[128];
    int note_velocity[128];
    int sustain_pedal;
    double bend_ratio;
    _Atomic uint32_t underruns;         /* summed from voices, for status */
    /* Worker only. Files are closed once their head is read, so a library of
     * any size holds at most one descriptor per STREAMING voice. The host
     * process's soft limit is 1024 and it is shared with everything else. */
    int stream_fd[DS_MAX_VOICES];
    uint64_t stream_key[DS_MAX_VOICES]; /* generation+zone the descriptor was opened for */
    uint64_t resident_bytes;
    /* The preset's controls and what they write into. Written by the audio
     * thread only (control changes, CCs), or by the worker before publish. */
    ds_preset_model_t model;
    ds_group_settings_t *groups_rt;     /* model.group_count, live */
    ds_group_settings_t instrument_rt;
    float tag_volume[DS_MAX_TAGS];
    unsigned char tag_enabled[DS_MAX_TAGS];
    float control_value[DS_MAX_CONTROLS];
    /* Effects: coefficients per model effect, recomputed on the audio thread
     * when a binding moves a setting; state for the instrument-level ones. */
    ds_fx_coeffs_t fx_coeffs[DS_MAX_EFFECTS];
    unsigned char fx_dirty[DS_MAX_EFFECTS];
    ds_fx_state_t fx_state[DS_MAX_EFFECTS];
    ds_reverb_t *reverb[DS_MAX_EFFECTS];  /* instrument-level reverbs, made at load */
    ds_chorus_t *chorus[DS_MAX_EFFECTS];  /* instrument-level choruses, made at load */
    ds_delay_t *delay[DS_MAX_EFFECTS];    /* instrument-level delays, made at load */
    /* Modulators: shared state for global ones, their value this block, the
     * last value of every CC (for <midiCC>), keys held (a global envelope
     * gates on the first key down and the last key up), and which effects any
     * modulator reaches (only those pay for per-block coefficients). */
    ds_mod_state_t mod_global[DS_MAX_MODULATORS];
    float mod_global_value[DS_MAX_MODULATORS];
    float cc_value[128];
    unsigned keys_held;
    unsigned char fx_modulated[DS_MAX_EFFECTS];
    /* The module's own amp envelope: attack, decay, sustain, release. A value
     * below 0 means "Preset" (keep what the preset says); otherwise it REPLACES
     * the preset's value for every zone that has an amp envelope. Written by the
     * audio thread only (the plugin copies it in before each MIDI call and block). */
    float amp_override[4];
    /* The module's note limit: how many NOTES (keys, however many layers each
     * plays) may sound at once; 0 = no limit beyond the voice pool. A new note
     * over it fades the oldest out in 5 ms, released notes first. */
    int poly_limit;
    uint32_t note_counter;
    ds_fx_coeffs_t fx_live[DS_MAX_EFFECTS];
    ds_fx_built_t fx_live_built[DS_MAX_EFFECTS];
    uint32_t fx_rebuilds;               /* coefficient rebuilds for modulation, for tests */
} ds_native_engine_t;

/* Loading is worker-only: parses XML, opens files, reads every resident head.
 * Missing sample files are skipped and counted, never fatal. */
typedef int (*ds_cancel_fn)(void *context);
/* `cancelled` (optional) is polled between files; a load it stops returns
 * DS_LOAD_CANCELLED and leaves nothing allocated. */
#define DS_LOAD_CANCELLED (-2)
int ds_native_engine_load(ds_native_engine_t *engine, const char *preset_path,
                          unsigned output_rate, ds_cancel_fn cancelled, void *cancel_context,
                          char *error, unsigned error_len);
void ds_native_engine_destroy(ds_native_engine_t *engine);

/* Audio thread: no I/O, no allocation, no locks. */
void ds_native_engine_note_on(ds_native_engine_t *engine, int note, int velocity);
void ds_native_engine_note_off(ds_native_engine_t *engine, int note);
void ds_native_engine_cc(ds_native_engine_t *engine, int cc, int value);
void ds_native_engine_pitch_bend(ds_native_engine_t *engine, int value14);
/* A preset control moved: stores it and fires its bindings. Audio thread (or
 * the worker before the engine is published). Buttons/menus take an index. */
void ds_native_engine_set_control(ds_native_engine_t *engine, unsigned index, float value);
/* The amp envelope the preset itself gives its first playable zone (attack,
 * decay, sustain, release), ignoring the module's override. Returns 0 if the
 * preset has no zone with an amp envelope. */
int ds_native_engine_preset_envelope(const ds_native_engine_t *engine, float out[4]);

/* `path` as it exists on disk, matching each component case-insensitively
 * when the name as written is not there (presets made on case-insensitive
 * Mac/PC filesystems). 0 on success. Worker only: reads directories. */
int ds_resolve_path_case(const char *path, char *out, size_t out_len);
void ds_native_engine_render(ds_native_engine_t *engine, float *out_lr, unsigned frames);
unsigned ds_native_engine_active_voices(const ds_native_engine_t *engine);

/* Worker: tops up every streaming voice's ring. Returns frames read. */
unsigned ds_native_engine_service(ds_native_engine_t *engine);

#endif
