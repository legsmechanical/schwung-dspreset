#include <assert.h>
#include <stdio.h>

#include "../src/dsp/dspreset/playback_policy.h"

static ds_playback_request_t request(ds_playback_mode_t mode, ds_playback_mode_t auto_mode) {
    ds_playback_request_t result = {0};
    result.requested = mode;
    result.auto_preference = auto_mode;
    return result;
}

int main(void) {
    ds_playback_request_t r = request(DS_PLAYBACK_DISK_STREAMING, DS_PLAYBACK_MEMORY);
    ds_playback_decision_t d = ds_resolve_playback_mode(&r);
    assert(d.mode == DS_PLAYBACK_DISK_STREAMING);
    assert(d.reason == DS_PLAYBACK_REASON_REQUESTED);
    assert(!d.forced_from_disk_streaming);

    r = request(DS_PLAYBACK_AUTO, DS_PLAYBACK_DISK_STREAMING);
    d = ds_resolve_playback_mode(&r);
    assert(d.mode == DS_PLAYBACK_DISK_STREAMING);
    assert(d.reason == DS_PLAYBACK_REASON_AUTO_PREFERENCE);

    r = request(DS_PLAYBACK_DISK_STREAMING, DS_PLAYBACK_DISK_STREAMING);
    r.dynamic_bounds.loop_end_is_runtime_bound = 1;
    d = ds_resolve_playback_mode(&r);
    assert(d.mode == DS_PLAYBACK_MEMORY);
    assert(d.reason == DS_PLAYBACK_REASON_DYNAMIC_BOUNDS_REQUIRE_MEMORY);
    assert(d.forced_from_disk_streaming);

    r = request(DS_PLAYBACK_AUTO, DS_PLAYBACK_AUTO);
    r.dynamic_bounds.start_is_runtime_bound = 1;
    d = ds_resolve_playback_mode(&r);
    assert(d.mode == DS_PLAYBACK_MEMORY);
    assert(d.reason == DS_PLAYBACK_REASON_AUTO_PREFERENCE);

    puts("playback policy tests passed");
    return 0;
}
