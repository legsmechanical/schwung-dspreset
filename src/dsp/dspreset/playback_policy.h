#ifndef DSPRESET_PLAYBACK_POLICY_H
#define DSPRESET_PLAYBACK_POLICY_H

#ifdef __cplusplus
extern "C" {
#endif

/* DecentSampler's playbackMode values.  Keep this model native to the
 * DSPreset engine: it must never be translated into an SFZ implementation
 * detail. */
typedef enum {
    DS_PLAYBACK_AUTO = 0,
    DS_PLAYBACK_MEMORY,
    DS_PLAYBACK_DISK_STREAMING,
} ds_playback_mode_t;

/* The guide requires memory playback when these are changed after a library
 * has loaded. A disk ring is inherently windowed, so allowing a dynamic seek
 * or loop-boundary edit would make the requested frames unavailable until a
 * later refill. */
typedef struct {
    int start_is_runtime_bound;
    int end_is_runtime_bound;
    int loop_start_is_runtime_bound;
    int loop_end_is_runtime_bound;
} ds_dynamic_sample_bounds_t;

typedef struct {
    ds_playback_mode_t requested;
    ds_playback_mode_t auto_preference;
    ds_dynamic_sample_bounds_t dynamic_bounds;
} ds_playback_request_t;

typedef enum {
    DS_PLAYBACK_REASON_REQUESTED = 0,
    DS_PLAYBACK_REASON_AUTO_PREFERENCE,
    DS_PLAYBACK_REASON_DYNAMIC_BOUNDS_REQUIRE_MEMORY,
} ds_playback_reason_t;

typedef struct {
    ds_playback_mode_t mode;
    ds_playback_reason_t reason;
    int forced_from_disk_streaming;
} ds_playback_decision_t;

/* Resolve one sample's final playback path. `auto_preference` is a user/device
 * policy (usually disk streaming for large Move libraries); a DSPreset's
 * explicit memory/disk_streaming choice takes precedence except when live
 * bindings mutate sample bounds. */
ds_playback_decision_t ds_resolve_playback_mode(const ds_playback_request_t *request);

#ifdef __cplusplus
}
#endif

#endif
