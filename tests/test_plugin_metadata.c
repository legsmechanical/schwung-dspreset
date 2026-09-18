#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct host_api_v1 {
    uint32_t api_version;
    int sample_rate, frames_per_block;
    uint8_t *mapped_memory;
    int audio_out_offset, audio_in_offset;
    void (*log)(const char *);
    int (*midi_send_internal)(const uint8_t *, int);
    int (*midi_send_external)(const uint8_t *, int);
} host_api_v1_t;

typedef struct plugin_api_v2 {
    uint32_t api_version;
    void *(*create_instance)(const char *, const char *);
    void (*destroy_instance)(void *);
    void (*on_midi)(void *, const uint8_t *, int, int);
    void (*set_param)(void *, const char *, const char *);
    int (*get_param)(void *, const char *, char *, int);
    int (*get_error)(void *, char *, int);
    void (*render_block)(void *, int16_t *, int);
} plugin_api_v2_t;

extern plugin_api_v2_t *move_plugin_init_v2(const host_api_v1_t *host);

int main(void) {
    char value[2048] = {0};
    host_api_v1_t host = {.api_version = 1, .sample_rate = 44100, .frames_per_block = 128};
    plugin_api_v2_t *api = move_plugin_init_v2(&host);
    void *instance;
    assert(api && api->api_version == 2 && api->create_instance && api->get_param);
    instance = api->create_instance("/tmp", "{}");
    assert(instance);
    assert(api->get_param(instance, "ui_hierarchy", value, sizeof(value)) > 0);
    assert(strstr(value, "preset_path") && strstr(value, "Library"));
    assert(api->get_param(instance, "chain_params", value, sizeof(value)) > 0);
    assert(strstr(value, "filepath") && strstr(value, ".dslibrary"));
    api->destroy_instance(instance);
    puts("plugin metadata test passed");
    return 0;
}
