#include "playback_policy.h"

static int has_dynamic_bounds(const ds_dynamic_sample_bounds_t *bounds) {
    return bounds->start_is_runtime_bound || bounds->end_is_runtime_bound ||
           bounds->loop_start_is_runtime_bound || bounds->loop_end_is_runtime_bound;
}

ds_playback_decision_t ds_resolve_playback_mode(const ds_playback_request_t *request) {
    ds_playback_decision_t result;
    ds_playback_mode_t requested = request->requested;

    if (requested != DS_PLAYBACK_MEMORY && requested != DS_PLAYBACK_DISK_STREAMING) {
        requested = request->auto_preference;
        if (requested != DS_PLAYBACK_MEMORY && requested != DS_PLAYBACK_DISK_STREAMING) {
            /* An invalid/missing device preference is intentionally safe: it
             * must not make a dynamically controlled sample nondeterministic. */
            requested = DS_PLAYBACK_MEMORY;
        }
        result.reason = DS_PLAYBACK_REASON_AUTO_PREFERENCE;
    } else {
        result.reason = DS_PLAYBACK_REASON_REQUESTED;
    }

    result.mode = requested;
    result.forced_from_disk_streaming = 0;
    if (requested == DS_PLAYBACK_DISK_STREAMING &&
        has_dynamic_bounds(&request->dynamic_bounds)) {
        result.mode = DS_PLAYBACK_MEMORY;
        result.reason = DS_PLAYBACK_REASON_DYNAMIC_BOUNDS_REQUIRE_MEMORY;
        result.forced_from_disk_streaming = 1;
    }
    return result;
}
