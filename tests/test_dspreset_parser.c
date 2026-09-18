#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../src/dsp/dspreset/dspreset_parser.h"

typedef struct { int samples, round_robin; } counts_t;

static int count_sample(const ds_dspreset_sample_t *sample, void *opaque) {
    counts_t *counts = opaque;
    assert(strstr(sample->path, "Samples/") == sample->path);
    assert(sample->lo_note <= sample->root_note && sample->root_note <= sample->hi_note);
    counts->samples++;
    if (sample->seq_position > 1) counts->round_robin++;
    return 0;
}

int main(int argc, char **argv) {
    char error[128] = {0};
    counts_t counts = {0};
    assert(argc == 2);
    assert(ds_dspreset_visit_samples(argv[1], count_sample, &counts, error, sizeof(error)) == 0);
    assert(counts.samples == 564);
    assert(counts.round_robin > 0);
    puts("DSPreset parser test passed");
    return 0;
}
