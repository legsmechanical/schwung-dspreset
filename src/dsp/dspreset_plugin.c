#define _DEFAULT_SOURCE
#include <stdatomic.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include "dspreset/library_input.h"
#include "dspreset/library_preparer.h"
#include "dspreset/native_engine.h"

#define MOVE_PLUGIN_API_VERSION_2 2

typedef struct host_api_v1 { uint32_t api_version; int sample_rate, frames_per_block; uint8_t *mapped_memory; int audio_out_offset, audio_in_offset; void (*log)(const char *); int (*midi_send_internal)(const uint8_t *, int); int (*midi_send_external)(const uint8_t *, int); } host_api_v1_t;
typedef struct plugin_api_v2 { uint32_t api_version; void *(*create_instance)(const char *, const char *); void (*destroy_instance)(void *); void (*on_midi)(void *, const uint8_t *, int, int); void (*set_param)(void *, const char *, const char *); int (*get_param)(void *, const char *, char *, int); int (*get_error)(void *, char *, int); void (*render_block)(void *, int16_t *, int); } plugin_api_v2_t;

typedef struct retired_engine { ds_native_engine_t *engine; struct retired_engine *next; } retired_engine_t;
typedef struct {
    _Atomic(ds_native_engine_t *) active;
    pthread_t worker;
    pthread_mutex_t request_lock;
    char requested_path[1024], preset_path[1024], error[256];
    float gain;
    int worker_running, loading;
    retired_engine_t *retired;
} dspreset_instance_t;

static void set_error(dspreset_instance_t *instance, const char *message) {
    snprintf(instance->error, sizeof(instance->error), "%s", message ? message : "unknown error");
}

static void retire_engine(dspreset_instance_t *instance, ds_native_engine_t *engine) {
    retired_engine_t *node;
    if (!engine) return;
    node = malloc(sizeof(*node));
    if (!node) return;
    node->engine = engine; node->next = instance->retired; instance->retired = node;
}

static int has_suffix(const char *path, const char *suffix) {
    size_t path_len = strlen(path), suffix_len = strlen(suffix);
    return path_len >= suffix_len && !strcmp(path + path_len - suffix_len, suffix);
}

static int find_preset(const char *directory, char *out, size_t out_len) {
    DIR *dir = opendir(directory);
    struct dirent *entry;
    if (!dir) return -1;
    while ((entry = readdir(dir)) != NULL) {
        char path[1024]; struct stat st;
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        if (snprintf(path, sizeof(path), "%s/%s", directory, entry->d_name) >= (int)sizeof(path) ||
            lstat(path, &st)) continue;
        if (S_ISDIR(st.st_mode) && !find_preset(path, out, out_len)) { closedir(dir); return 0; }
        if (S_ISREG(st.st_mode) && has_suffix(path, ".dspreset") &&
            snprintf(out, out_len, "%s", path) < (int)out_len) { closedir(dir); return 0; }
    }
    closedir(dir); return -1;
}

static int prepare_input(const char *request, char *preset_path, size_t preset_len,
                         char *error, unsigned error_len) {
    ds_library_input_kind_t kind = ds_classify_library_input(request, 0);
    if (kind == DS_LIBRARY_INPUT_PRESET_FILE) {
        return snprintf(preset_path, preset_len, "%s", request) >= (int)preset_len ? -1 : 0;
    }
    if (kind == DS_LIBRARY_INPUT_DSLIBRARY_ARCHIVE) {
        char destination[1024]; struct stat st; ds_library_prepare_result_t result;
        if (snprintf(destination, sizeof(destination), "%s.unpacked", request) >= (int)sizeof(destination)) return -1;
        if (stat(destination, &st)) {
            if (ds_library_prepare_archive(request, destination, &result, error, error_len)) return -1;
        } else if (!S_ISDIR(st.st_mode)) {
            snprintf(error, error_len, "%s", "DSLibrary cache path is not a directory"); return -1;
        }
        if (find_preset(destination, preset_path, preset_len)) {
            snprintf(error, error_len, "%s", "prepared DSLibrary has no DSPreset"); return -1;
        }
        return 0;
    }
    snprintf(error, error_len, "%s", "unsupported library input"); return -1;
}

static void *engine_worker(void *opaque) {
    dspreset_instance_t *instance = opaque;
    while (instance->worker_running) {
        char request[1024] = {0}, error[256] = {0};
        pthread_mutex_lock(&instance->request_lock);
        if (instance->requested_path[0]) {
            snprintf(request, sizeof(request), "%s", instance->requested_path);
            instance->requested_path[0] = '\0'; instance->loading = 1;
        }
        pthread_mutex_unlock(&instance->request_lock);
        if (request[0]) {
            ds_native_engine_t *next = calloc(1, sizeof(*next));
            char preset_path[1024] = {0};
            if (!next || prepare_input(request, preset_path, sizeof(preset_path), error, sizeof(error)) ||
                ds_native_engine_load(next, preset_path, error, sizeof(error))) {
                if (next) { ds_native_engine_destroy(next); free(next); }
                set_error(instance, error[0] ? error : "cannot load DSPreset");
            } else {
                ds_native_engine_t *previous = atomic_exchange_explicit(&instance->active, next, memory_order_acq_rel);
                snprintf(instance->preset_path, sizeof(instance->preset_path), "%s", request);
                instance->error[0] = '\0'; retire_engine(instance, previous);
            }
            instance->loading = 0;
        }
        ds_native_engine_t *engine = atomic_load_explicit(&instance->active, memory_order_acquire);
        if (engine) ds_native_engine_service(engine, error, sizeof(error));
        usleep(2000);
    }
    return NULL;
}

static void *create_instance(const char *module_dir, const char *json_defaults) {
    dspreset_instance_t *instance = calloc(1, sizeof(*instance));
    (void)module_dir; (void)json_defaults;
    if (!instance) return NULL;
    instance->gain = 0.7f; instance->worker_running = 1;
    pthread_mutex_init(&instance->request_lock, NULL);
    if (pthread_create(&instance->worker, NULL, engine_worker, instance)) {
        pthread_mutex_destroy(&instance->request_lock); free(instance); return NULL;
    }
    return instance;
}

static void destroy_instance(void *opaque) {
    dspreset_instance_t *instance = opaque;
    retired_engine_t *node;
    if (!instance) return;
    instance->worker_running = 0; pthread_join(instance->worker, NULL);
    retire_engine(instance, atomic_load(&instance->active));
    while ((node = instance->retired) != NULL) { instance->retired = node->next; ds_native_engine_destroy(node->engine); free(node->engine); free(node); }
    pthread_mutex_destroy(&instance->request_lock); free(instance);
}

static void on_midi(void *opaque, const uint8_t *msg, int len, int source) {
    dspreset_instance_t *instance = opaque;
    ds_native_engine_t *engine;
    (void)source;
    if (!instance || len < 3) return;
    engine = atomic_load_explicit(&instance->active, memory_order_acquire);
    if (!engine) return;
    if ((msg[0] & 0xf0) == 0x90 && msg[2]) ds_native_engine_note_on(engine, msg[1], msg[2], 44100);
    else if ((msg[0] & 0xf0) == 0x80 || ((msg[0] & 0xf0) == 0x90 && !msg[2])) ds_native_engine_note_off(engine, msg[1]);
}

static void set_param(void *opaque, const char *key, const char *value) {
    dspreset_instance_t *instance = opaque;
    if (!instance || !key || !value) return;
    if (!strcmp(key, "preset_path")) {
        pthread_mutex_lock(&instance->request_lock);
        snprintf(instance->requested_path, sizeof(instance->requested_path), "%s", value);
        pthread_mutex_unlock(&instance->request_lock);
    } else if (!strcmp(key, "gain")) instance->gain = strtof(value, NULL);
}

static int get_param(void *opaque, const char *key, char *out, int out_len) {
    dspreset_instance_t *instance = opaque;
    if (!instance || !key || !out || out_len <= 0) return -1;
    if (!strcmp(key, "preset_path")) snprintf(out, (size_t)out_len, "%s", instance->preset_path);
    else if (!strcmp(key, "gain")) snprintf(out, (size_t)out_len, "%.3f", instance->gain);
    else if (!strcmp(key, "loading")) snprintf(out, (size_t)out_len, "%d", instance->loading);
    else return -1;
    return 0;
}

static int get_error(void *opaque, char *out, int out_len) { dspreset_instance_t *i = opaque; if (!i || !out || out_len <= 0) return -1; snprintf(out, (size_t)out_len, "%s", i->error); return 0; }

static void render_block(void *opaque, int16_t *out, int frames) {
    dspreset_instance_t *instance = opaque;
    ds_native_engine_t *engine;
    float buffer[256];
    if (!instance || !out || frames <= 0 || frames > 128) return;
    memset(buffer, 0, (size_t)frames * 2 * sizeof(float));
    engine = atomic_load_explicit(&instance->active, memory_order_acquire);
    if (engine) ds_native_engine_render(engine, buffer, (unsigned)frames);
    for (int i = 0; i < frames * 2; ++i) { float x = buffer[i] * instance->gain; if (x > 1) x = 1; if (x < -1) x = -1; out[i] = (int16_t)(x * 32767); }
}

static plugin_api_v2_t g_plugin_api_v2 = {
    .api_version = MOVE_PLUGIN_API_VERSION_2,
    .create_instance = create_instance,
    .destroy_instance = destroy_instance,
    .on_midi = on_midi,
    .set_param = set_param,
    .get_param = get_param,
    .get_error = get_error,
    .render_block = render_block,
};

plugin_api_v2_t *move_plugin_init_v2(const host_api_v1_t *host) {
    (void)host;
    return &g_plugin_api_v2;
}
