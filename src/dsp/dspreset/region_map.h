#ifndef DSPRESET_REGION_MAP_H
#define DSPRESET_REGION_MAP_H

#include "dspreset_parser.h"

#define DS_MAX_REGIONS 2048

typedef struct {
    ds_dspreset_sample_t regions[DS_MAX_REGIONS];
    unsigned count;
} ds_region_map_t;

typedef int (*ds_region_visitor_t)(const ds_dspreset_sample_t *region, void *context);

/* Builds the fixed native region table on the loading worker. No XML and no
 * allocation occurs in the MIDI or render paths after this returns. */
int ds_region_map_load(ds_region_map_t *map, const char *preset_path,
                       char *error, unsigned error_len);

/* Visits every native region matching note, velocity and round-robin position.
 * A sequence position of zero accepts all sequence layers. */
int ds_region_map_visit(const ds_region_map_t *map, int note, int velocity,
                        int sequence_position, ds_region_visitor_t visitor, void *context);

#endif
