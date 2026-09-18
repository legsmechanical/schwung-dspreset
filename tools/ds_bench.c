/* Render cost per 128-frame block with N simultaneous voices, engine only.
 * usage: ds_bench <preset> <voices>   (a guide, not a Move measurement) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "dspreset/native_engine.h"

static double now_us(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e6 + t.tv_nsec / 1e3; }

int main(int argc, char **argv) {
    static ds_native_engine_t e;
    char error[256];
    float out[256];
    int voices = argc > 2 ? atoi(argv[2]) : 16;
    double total = 0, worst = 0, first_worst = 0;
    if (argc < 2 || ds_native_engine_load(&e, argv[1], 44100, NULL, NULL, error, sizeof(error))) { fprintf(stderr, "load: %s\n", error); return 1; }
    for (int v = 0; v < voices; ++v) ds_native_engine_note_on(&e, 36 + v % 40, 100);
    for (int b = 0; b < 2000; ++b) {
        double t0;
        while (ds_native_engine_service(&e)) {}
        memset(out, 0, sizeof(out));
        t0 = now_us();
        ds_native_engine_render(&e, out, 128);
        t0 = now_us() - t0;
        total += t0; if (b >= 200 && t0 > worst) worst = t0;
        if (b < 200 && t0 > first_worst) first_worst = t0;
    }
    printf("%d voices requested, %u active at end: mean %.1f us, first-200 worst %.1f us, later worst %.1f us per block, underruns %u, resident %.1f MB\n",
           voices, ds_native_engine_active_voices(&e), total / 2000, first_worst, worst, atomic_load(&e.underruns), e.resident_bytes / 1048576.0);
    return 0;
}
