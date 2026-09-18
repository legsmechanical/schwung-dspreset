#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../src/dsp/dspreset/native_engine.h"

int main(int argc, char **argv) {
    char error[128] = {0};
    float output[256] = {0};
    ds_native_engine_t engine;
    assert(argc == 2);
    assert(ds_native_engine_load(&engine, argv[1], error, sizeof(error)) == 0);
    assert(engine.regions.count == 564 && engine.source_count == 540);
    ds_native_engine_note_on(&engine, 35, 100, 44100);
    ds_native_engine_render(&engine, output, 128);
    assert(ds_native_engine_service(&engine, error, sizeof(error)) > 0);
    memset(output, 0, sizeof(output));
    ds_native_engine_render(&engine, output, 128);
    assert(output[0] != 0 || output[1] != 0);
    ds_native_engine_note_off(&engine, 35);
    ds_native_engine_destroy(&engine);
    puts("native engine test passed");
    return 0;
}
