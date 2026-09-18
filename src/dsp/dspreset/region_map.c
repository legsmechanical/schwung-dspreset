#include "region_map.h"

#include <stdio.h>
#include <string.h>

typedef struct { ds_region_map_t *map; char *error; unsigned error_len; } load_context_t;

static int add_region(const ds_dspreset_sample_t *sample, void *opaque) {
    load_context_t *context = opaque;
    if (context->map->count == DS_MAX_REGIONS) {
        if (context->error_len) snprintf(context->error, context->error_len,
                                         "%s", "DSPreset has too many sample regions");
        return 1;
    }
    context->map->regions[context->map->count++] = *sample;
    return 0;
}

int ds_region_map_load(ds_region_map_t *map, const char *preset_path,
                       char *error, unsigned error_len) {
    load_context_t context;
    if (!map) return -1;
    memset(map, 0, sizeof(*map));
    context = (load_context_t){map, error, error_len};
    if (ds_dspreset_visit_samples(preset_path, add_region, &context, error, error_len) || !map->count) {
        return -1;
    }
    return 0;
}

int ds_region_map_visit(const ds_region_map_t *map, int note, int velocity,
                        int sequence_position, ds_region_visitor_t visitor, void *context) {
    int count = 0;
    if (!map || !visitor || note < 0 || note > 127 || velocity < 0 || velocity > 127) return -1;
    for (unsigned i = 0; i < map->count; ++i) {
        const ds_dspreset_sample_t *region = &map->regions[i];
        if (note < region->lo_note || note > region->hi_note ||
            velocity < region->lo_vel || velocity > region->hi_vel ||
            (sequence_position && region->seq_position != sequence_position)) continue;
        count++;
        if (visitor(region, context)) break;
    }
    return count;
}
