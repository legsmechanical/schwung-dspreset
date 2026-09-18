#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../src/dsp/dspreset/region_map.h"

static int accept_region(const ds_dspreset_sample_t *region, void *opaque) {
    int *seen = opaque;
    assert(strstr(region->path, ".wav") != NULL);
    (*seen)++;
    return 0;
}

int main(int argc, char **argv) {
    char error[128] = {0};
    ds_region_map_t map;
    int seen = 0;
    assert(argc == 2);
    assert(ds_region_map_load(&map, argv[1], error, sizeof(error)) == 0);
    assert(map.count == 564);
    assert(ds_region_map_visit(&map, 35, 100, 1, accept_region, &seen) == seen);
    assert(seen > 0);
    puts("region map test passed");
    return 0;
}
