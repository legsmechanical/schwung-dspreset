#ifndef DSPRESET_NATIVE_ENGINE_H
#define DSPRESET_NATIVE_ENGINE_H

#include <stdatomic.h>
#include <stdint.h>

#include "dspreset_parser.h"
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
} ds_zone_t;

enum { DS_ENV_ATTACK, DS_ENV_DECAY, DS_ENV_SUSTAIN, DS_ENV_RELEASE, DS_ENV_DONE };

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
} ds_native_engine_t;

/* Loading is worker-only: parses XML, opens files, reads every resident head.
 * Missing sample files are skipped and counted, never fatal. */
int ds_native_engine_load(ds_native_engine_t *engine, const char *preset_path,
                          unsigned output_rate, char *error, unsigned error_len);
void ds_native_engine_destroy(ds_native_engine_t *engine);

/* Audio thread: no I/O, no allocation, no locks. */
void ds_native_engine_note_on(ds_native_engine_t *engine, int note, int velocity);
void ds_native_engine_note_off(ds_native_engine_t *engine, int note);
void ds_native_engine_cc(ds_native_engine_t *engine, int cc, int value);
void ds_native_engine_pitch_bend(ds_native_engine_t *engine, int value14);
void ds_native_engine_render(ds_native_engine_t *engine, float *out_lr, unsigned frames);
unsigned ds_native_engine_active_voices(const ds_native_engine_t *engine);

/* Worker: tops up every streaming voice's ring. Returns frames read. */
unsigned ds_native_engine_service(ds_native_engine_t *engine);

#endif
