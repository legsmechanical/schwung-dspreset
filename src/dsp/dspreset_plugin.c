#define _DEFAULT_SOURCE
#include <ctype.h>
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

static const host_api_v1_t *g_host;

typedef struct {
    _Atomic(ds_native_engine_t *) active;
    _Atomic int audio_users;            /* >0 while on_midi/render_block hold `active` */
    pthread_t worker;
    pthread_mutex_t lock;               /* guards the strings below; never taken by audio */
    char requested_path[1024], preset_path[1024], loaded_path[1024], status[256], error[256], module_dir[512];
    _Atomic int worker_running, loading;
    float gain;
} dspreset_instance_t;

static void log_line(const char *fmt, const char *a, const char *b) {
    char line[1400];
    if (!g_host || !g_host->log) return;
    snprintf(line, sizeof(line), fmt, a, b);
    g_host->log(line);
}

static void set_status(dspreset_instance_t *in, const char *status, const char *error) {
    pthread_mutex_lock(&in->lock);
    snprintf(in->status, sizeof(in->status), "%s", status);
    snprintf(in->error, sizeof(in->error), "%s", error ? error : "");
    pthread_mutex_unlock(&in->lock);
}

static int has_suffix(const char *path, const char *suffix) {
    size_t path_len = strlen(path), suffix_len = strlen(suffix);
    return path_len >= suffix_len && !strcasecmp(path + path_len - suffix_len, suffix);
}

/* "2 - Foundation" before "10 - Moog Town". */
static int natural_compare(const char *a, const char *b) {
    while (*a && *b) {
        if (isdigit((unsigned char)*a) && isdigit((unsigned char)*b)) {
            unsigned long x = strtoul(a, (char **)&a, 10), y = strtoul(b, (char **)&b, 10);
            if (x != y) return x < y ? -1 : 1;
            continue;
        }
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
            return tolower((unsigned char)*a) - tolower((unsigned char)*b);
        ++a; ++b;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

/* The natural-order first DSPreset under `directory`. */
static void first_preset(const char *directory, char *best, size_t best_len, int depth) {
    DIR *dir = opendir(directory);
    struct dirent *entry;
    if (!dir || depth > 5) { if (dir) closedir(dir); return; }
    while ((entry = readdir(dir)) != NULL) {
        char path[1024]; struct stat st;
        if (entry->d_name[0] == '.' || !strcmp(entry->d_name, "__MACOSX")) continue;
        if (snprintf(path, sizeof(path), "%s/%s", directory, entry->d_name) >= (int)sizeof(path) || stat(path, &st)) continue;
        if (S_ISDIR(st.st_mode)) first_preset(path, best, best_len, depth + 1);
        else if (S_ISREG(st.st_mode) && has_suffix(path, ".dspreset") && (!best[0] || natural_compare(path, best) < 0))
            snprintf(best, best_len, "%s", path);
    }
    closedir(dir);
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
        preset_path[0] = '\0';
        first_preset(destination, preset_path, preset_len, 0);
        if (!preset_path[0]) { snprintf(error, error_len, "%s", "prepared DSLibrary has no DSPreset"); return -1; }
        return 0;
    }
    snprintf(error, error_len, "%s", "unsupported library input"); return -1;
}

/* Frees `old` once no audio call can still be using it: anything that enters
 * after the swap reads the new pointer, so one moment with no users is enough. */
static void retire(dspreset_instance_t *in, ds_native_engine_t *old) {
    if (!old) return;
    while (atomic_load(&in->audio_users)) usleep(200);
    ds_native_engine_destroy(old);
    free(old);
}

static void load_request(dspreset_instance_t *in, const char *request) {
    char preset[1024] = {0}, error[256] = {0}, status[256];
    ds_native_engine_t *next = calloc(1, sizeof(*next));
    unsigned rate = g_host && g_host->sample_rate > 0 ? (unsigned)g_host->sample_rate : 44100;
    const char *name;
    set_status(in, "Loading...", NULL);
    if (!next || prepare_input(request, preset, sizeof(preset), error, sizeof(error)) ||
        ds_native_engine_load(next, preset, rate, error, sizeof(error))) {
        free(next);
        snprintf(status, sizeof(status), "Error: %s", error[0] ? error : "cannot load DSPreset");
        set_status(in, status, error[0] ? error : "cannot load DSPreset");
        log_line("dspreset: load failed: %s (%s)", error, request);
        return;
    }
    name = strrchr(preset, '/') ? strrchr(preset, '/') + 1 : preset;
    if (next->missing_zones)
        snprintf(status, sizeof(status), "%s: %u/%u zones, %u files missing", name,
                 next->zone_count - next->missing_zones, next->zone_count, next->missing_files);
    else
        snprintf(status, sizeof(status), "%s: %u zones", name, next->zone_count);
    retire(in, atomic_exchange(&in->active, next));
    set_status(in, status, NULL);
    pthread_mutex_lock(&in->lock);
    snprintf(in->loaded_path, sizeof(in->loaded_path), "%s", request);
    pthread_mutex_unlock(&in->lock);
    log_line("dspreset: loaded %s%s", status, "");
}

static void *engine_worker(void *opaque) {
    dspreset_instance_t *in = opaque;
    while (atomic_load(&in->worker_running)) {
        char request[1024] = {0};
        ds_native_engine_t *engine;
        pthread_mutex_lock(&in->lock);
        if (in->requested_path[0]) {
            snprintf(request, sizeof(request), "%s", in->requested_path);
            in->requested_path[0] = '\0';
        }
        pthread_mutex_unlock(&in->lock);
        if (request[0]) {
            atomic_store(&in->loading, 1);
            load_request(in, request);
            atomic_store(&in->loading, 0);
        }
        engine = atomic_load(&in->active);
        if (!engine || !ds_native_engine_service(engine)) usleep(1000);
    }
    return NULL;
}

static void *create_instance(const char *module_dir, const char *json_defaults) {
    dspreset_instance_t *in = calloc(1, sizeof(*in));
    (void)json_defaults;
    if (!in) return NULL;
    in->gain = 0.7f;
    snprintf(in->module_dir, sizeof(in->module_dir), "%s", module_dir ? module_dir : "");
    snprintf(in->status, sizeof(in->status), "%s", "No preset");
    atomic_store(&in->worker_running, 1);
    pthread_mutex_init(&in->lock, NULL);
    if (pthread_create(&in->worker, NULL, engine_worker, in)) {
        pthread_mutex_destroy(&in->lock); free(in); return NULL;
    }
    return in;
}

static void destroy_instance(void *opaque) {
    dspreset_instance_t *in = opaque;
    if (!in) return;
    atomic_store(&in->worker_running, 0);
    pthread_join(in->worker, NULL);
    retire(in, atomic_exchange(&in->active, NULL));
    pthread_mutex_destroy(&in->lock);
    free(in);
}

static void on_midi(void *opaque, const uint8_t *msg, int len, int source) {
    dspreset_instance_t *in = opaque;
    ds_native_engine_t *engine;
    (void)source;
    if (!in || len < 2) return;
    atomic_fetch_add(&in->audio_users, 1);
    engine = atomic_load(&in->active);
    if (engine) {
        switch (msg[0] & 0xf0) {
        case 0x90: if (len >= 3) ds_native_engine_note_on(engine, msg[1], msg[2]); break;
        case 0x80: ds_native_engine_note_off(engine, msg[1]); break;
        case 0xb0: if (len >= 3) ds_native_engine_cc(engine, msg[1], msg[2]); break;
        case 0xe0: if (len >= 3) ds_native_engine_pitch_bend(engine, msg[1] | (msg[2] << 7)); break;
        default: break;
        }
    }
    atomic_fetch_sub(&in->audio_users, 1);
}

static void set_param(void *opaque, const char *key, const char *value) {
    dspreset_instance_t *in = opaque;
    if (!in || !key || !value) return;
    if (!strcmp(key, "preset_path")) {
        pthread_mutex_lock(&in->lock);
        /* A re-sent path that already loaded is not reloaded; a failed one is retried. */
        snprintf(in->preset_path, sizeof(in->preset_path), "%s", value);
        if (value[0] && strcmp(value, in->loaded_path))
            snprintf(in->requested_path, sizeof(in->requested_path), "%s", value);
        pthread_mutex_unlock(&in->lock);
    } else if (!strcmp(key, "gain")) {
        in->gain = strtof(value, NULL);
    }
}

static int locked_string(dspreset_instance_t *in, const char *text, char *out, int out_len) {
    int n;
    pthread_mutex_lock(&in->lock);
    n = snprintf(out, (size_t)out_len, "%s", text);
    pthread_mutex_unlock(&in->lock);
    return n < out_len ? n : out_len - 1;
}

static int get_param(void *opaque, const char *key, char *out, int out_len) {
    dspreset_instance_t *in = opaque;
    int n;
    if (!in || !key || !out || out_len <= 0) return -1;
    if (!strcmp(key, "ui_hierarchy")) {
        n = snprintf(out, (size_t)out_len,
            "{\"levels\":{\"root\":{\"name\":\"DSPreset\","
            "\"params\":[{\"key\":\"preset_path\",\"name\":\"Library\"},"
            "{\"key\":\"gain\",\"name\":\"Gain\"}],\"knobs\":[\"gain\"]}}}");
    } else if (!strcmp(key, "chain_params")) {
        n = snprintf(out, (size_t)out_len,
            "[{\"key\":\"preset_path\",\"name\":\"Library\",\"type\":\"filepath\","
            "\"root\":\"/data/UserData\",\"start_path\":\"%s/instruments\","
            "\"filter\":[\".dspreset\",\".dslibrary\"],\"default\":\"\"},"
            "{\"key\":\"gain\",\"name\":\"Gain\",\"type\":\"float\","
            "\"min\":0,\"max\":2,\"step\":0.02,\"default\":0.7}]", in->module_dir);
    } else if (!strcmp(key, "preset_path")) {
        return locked_string(in, in->preset_path, out, out_len);
    } else if (!strcmp(key, "status")) {
        return locked_string(in, in->status, out, out_len);
    } else if (!strcmp(key, "gain")) {
        n = snprintf(out, (size_t)out_len, "%.3f", in->gain);
    } else if (!strcmp(key, "loading")) {
        n = snprintf(out, (size_t)out_len, "%d", atomic_load(&in->loading));
    } else if (!strcmp(key, "underruns") || !strcmp(key, "voices")) {
        ds_native_engine_t *engine;
        unsigned value = 0;
        atomic_fetch_add(&in->audio_users, 1);
        engine = atomic_load(&in->active);
        if (engine) value = !strcmp(key, "voices") ? ds_native_engine_active_voices(engine)
                                                   : atomic_load(&engine->underruns);
        atomic_fetch_sub(&in->audio_users, 1);
        n = snprintf(out, (size_t)out_len, "%u", value);
    } else {
        return -1;
    }
    return n < out_len ? n : out_len - 1;
}

static int get_error(void *opaque, char *out, int out_len) {
    dspreset_instance_t *in = opaque;
    if (!in || !out || out_len <= 0) return -1;
    return locked_string(in, in->error, out, out_len);
}

static void render_block(void *opaque, int16_t *out, int frames) {
    dspreset_instance_t *in = opaque;
    ds_native_engine_t *engine;
    float buffer[2 * 256];
    if (!in || !out || frames <= 0) return;
    if (frames > 256) frames = 256;
    memset(buffer, 0, (size_t)frames * 2 * sizeof(float));
    atomic_fetch_add(&in->audio_users, 1);
    engine = atomic_load(&in->active);
    if (engine) ds_native_engine_render(engine, buffer, (unsigned)frames);
    atomic_fetch_sub(&in->audio_users, 1);
    for (int i = 0; i < frames * 2; ++i) {
        float x = buffer[i] * in->gain;
        if (x > 1) x = 1;
        if (x < -1) x = -1;
        out[i] = (int16_t)(x * 32767);
    }
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
    g_host = host;
    return &g_plugin_api_v2;
}
