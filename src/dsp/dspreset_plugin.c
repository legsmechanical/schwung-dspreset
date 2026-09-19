#define _DEFAULT_SOURCE
#include <stdarg.h>
#include <stdatomic.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "dspreset/catalog.h"
#include "dspreset/library_input.h"
#include "dspreset/library_preparer.h"
#include "dspreset/native_engine.h"

#define MOVE_PLUGIN_API_VERSION_2 2

typedef struct host_api_v1 { uint32_t api_version; int sample_rate, frames_per_block; uint8_t *mapped_memory; int audio_out_offset, audio_in_offset; void (*log)(const char *); int (*midi_send_internal)(const uint8_t *, int); int (*midi_send_external)(const uint8_t *, int); } host_api_v1_t;
typedef struct plugin_api_v2 { uint32_t api_version; void *(*create_instance)(const char *, const char *); void (*destroy_instance)(void *); void (*on_midi)(void *, const uint8_t *, int, int); void (*set_param)(void *, const char *, const char *); int (*get_param)(void *, const char *, char *, int); int (*get_error)(void *, char *, int); void (*render_block)(void *, int16_t *, int); } plugin_api_v2_t;

/* How long a selection must stay put before it loads. Scrolling the preset
 * list — or a host walking every index to learn the names — never loads the
 * presets it passes. */
#define SETTLE_MS 150
#define RESCAN_MS 5000
/* A new instance waits this long for a restored state before choosing a first
 * preset itself, so a project reopening never loads something it then drops. */
#define FIRST_PICK_MS 500
#define PERF_REPORT_MS 5000
#define NO_BANK (-2)                    /* nothing chosen yet */
#define FILE_BANK (-1)                  /* a file outside the catalog */

static const host_api_v1_t *g_host;

/* ---- the module's own amp envelope ---------------------------------------
 * An Override switch and four numeric knobs, adjacent and named *_attack ..
 * *_release so both hosts' pages draw them as an ENVELOPE. Off: the preset's
 * own envelope plays, and the knobs are set to it each time a preset loads, so
 * they show what is playing and switching On changes nothing until one moves.
 * On: the knobs REPLACE the preset's envelope, on every preset (Josh,
 * 2026-09-18: a switch on the envelope page, not stepped "Preset" knobs). */
enum { AMP_ATTACK = 0, AMP_DECAY, AMP_SUSTAIN, AMP_RELEASE };
static const char *AMP_KEYS[4] = {"amp_attack", "amp_decay", "amp_sustain", "amp_release"};
static const float AMP_MAX[4] = {10, 10, 1, 20};

static int amp_stage_of(const char *key) {
    for (int i = 0; i < 4; ++i) if (!strcmp(key, AMP_KEYS[i])) return i;
    return -1;
}

/* ---- lock-free publication ----------------------------------------------
 * get_param/set_param can be served on the audio thread, so nothing they touch
 * may lock. Strings cross threads through a seqlock: one writer, readers retry
 * if a write overlapped their copy. */
/* 1024 = the path buffers every reader uses; a longer status line is cut, a
 * longer PATH would never compare equal to itself, so paths are capped at 1023. */
typedef struct { _Atomic uint32_t seq; char text[1024]; } seqstr_t;

static void seqstr_write(seqstr_t *s, const char *text) {
    uint32_t seq = atomic_load_explicit(&s->seq, memory_order_relaxed);
    atomic_store_explicit(&s->seq, seq + 1, memory_order_relaxed);
    atomic_thread_fence(memory_order_release);
    snprintf(s->text, sizeof(s->text), "%.1023s", text);
    atomic_store_explicit(&s->seq, seq + 2, memory_order_release);
}

static void seqstr_read(seqstr_t *s, char *out, size_t n) {
    for (int attempt = 0; attempt < 16; ++attempt) {
        uint32_t a = atomic_load_explicit(&s->seq, memory_order_acquire), b;
        if (a & 1) continue;
        snprintf(out, n, "%s", s->text);
        atomic_thread_fence(memory_order_acquire);
        b = atomic_load_explicit(&s->seq, memory_order_relaxed);
        if (a == b) return;
    }
    if (n) out[0] = '\0';
}

typedef struct retired { ds_catalog_t *catalog; struct retired *next; } retired_t;

typedef struct {
    _Atomic(ds_native_engine_t *) active;
    _Atomic int audio_users;            /* >0 while an audio-side call holds `active` */
    _Atomic(ds_catalog_t *) catalog;    /* immutable once published */
    _Atomic int sel_bank, sel_preset;   /* bank may be FILE_BANK or NO_BANK */
    _Atomic uint32_t sel_gen;           /* bumped by every selection change */
    _Atomic int unpacking_bank, loading, worker_running;
    _Atomic uint32_t load_count;        /* engines actually built, for tests */
    _Atomic float gain;
    _Atomic int amp_on;                 /* the module's amp envelope: Override */
    _Atomic int polyphony;              /* notes at once; 0 = "Preset" (no module limit) */
    _Atomic float amp_value[4];         /* seconds, seconds, 0..1, seconds */
    seqstr_t request;                   /* preset_path / state from the host */
    seqstr_t request_controls;          /* control positions from a restored state */
    _Atomic uint32_t request_gen;
    _Atomic int busy;                   /* the module's is_loading: a pick not yet playing */
    /* Control moves from the host, drained on the audio thread (the engine's
     * live settings have one writer). Dropped when a new preset is swapped in. */
    _Atomic float ctl_pending[DS_MAX_CONTROLS];
    _Atomic uint64_t ctl_dirty;
    /* Render cost, as measured ON the device: summed by the audio thread,
     * reported and reset by the worker every PERF_REPORT_MS while notes play. */
    _Atomic uint64_t perf_ns_sum, perf_ns_max;
    _Atomic uint32_t perf_blocks, perf_voices_max;
    seqstr_t status, loaded_path;
    pthread_t worker;
    char module_dir[512], instruments[600];
    /* worker only */
    retired_t *retired;
    char direct_path[1024];
    char restore_path[1024], restore_controls[1024];
} dspreset_instance_t;

static uint64_t now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000u + (uint64_t)t.tv_nsec / 1000000u;
}

static void log_line(const char *text) {
    char line[1600];
    if (!g_host || !g_host->log) return;
    snprintf(line, sizeof(line), "dspreset: %s", text);
    g_host->log(line);
}

static void select_bank_preset(dspreset_instance_t *in, int bank, int preset) {
    atomic_store(&in->sel_bank, bank);
    atomic_store(&in->sel_preset, preset);
    atomic_fetch_add(&in->sel_gen, 1);
}

/* ---- worker ------------------------------------------------------------- */

static void publish_catalog(dspreset_instance_t *in, ds_catalog_t *next) {
    ds_catalog_t *old = atomic_load(&in->catalog);
    retired_t *node;
    if (old && ds_catalog_equal(old, next)) { ds_catalog_free(next); return; }
    atomic_store(&in->catalog, next);
    /* Readers hold no reference count, so an old catalog lives until destroy.
     * It only changes when the instruments folder does. */
    if (old && (node = malloc(sizeof(*node)))) { node->catalog = old; node->next = in->retired; in->retired = node; }
}

static void rescan(dspreset_instance_t *in) {
    ds_catalog_t *next = ds_catalog_scan(in->instruments);
    if (next) publish_catalog(in, next);
}

static void retire_engine(dspreset_instance_t *in, ds_native_engine_t *old) {
    if (!old) return;
    while (atomic_load(&in->audio_users)) usleep(200);   /* anyone after the swap sees the new one */
    ds_native_engine_destroy(old);
    free(old);
}

typedef struct { dspreset_instance_t *in; uint32_t gen; } cancel_ctx_t;
static int superseded(void *opaque) {
    cancel_ctx_t *c = opaque;
    return atomic_load(&c->in->sel_gen) != c->gen || !atomic_load(&c->in->worker_running);
}

/* The .dspreset the current selection means, or "" if there is none yet. */
static void selected_target(dspreset_instance_t *in, char *out, size_t n, int *needs_unpack) {
    ds_catalog_t *c = atomic_load(&in->catalog);
    int bank = atomic_load(&in->sel_bank), preset = atomic_load(&in->sel_preset);
    *needs_unpack = 0;
    out[0] = '\0';
    if (bank == FILE_BANK) { snprintf(out, n, "%s", in->direct_path); return; }
    if (!c || bank < 0 || bank >= (int)c->bank_count) return;   /* NO_BANK: nothing yet */
    if (c->banks[bank].kind == DS_BANK_DSLIBRARY && !c->banks[bank].prepared) {
        *needs_unpack = 1;
        snprintf(out, n, "%s", c->banks[bank].path);
        return;
    }
    if (preset >= 0 && preset < (int)c->banks[bank].preset_count)
        snprintf(out, n, "%s", c->banks[bank].preset_paths[preset]);
}

static int unpack_bank(dspreset_instance_t *in, const char *archive, int bank) {
    int rc;
    char destination[1100], error[256] = {0}, line[1400];
    ds_library_prepare_result_t result;
    atomic_store(&in->unpacking_bank, bank);
    seqstr_write(&in->status, "Unpacking...");
    snprintf(destination, sizeof(destination), "%s.unpacked", archive);
    rc = ds_library_prepare_archive(archive, destination, &result, error, sizeof(error));
    if (rc) {
        snprintf(line, sizeof(line), "Error: %s", error[0] ? error : "cannot unpack");
        seqstr_write(&in->status, line);
        snprintf(line, sizeof(line), "unpack failed: %s (%s)", error, archive);
        log_line(line);
    } else {
        snprintf(line, sizeof(line), "unpacked %u files from %s", result.extracted_files, archive);
        log_line(line);
    }
    rescan(in);
    atomic_store(&in->unpacking_bank, -1);
    return rc;
}

static int load_target(dspreset_instance_t *in, const char *path, uint32_t gen) {
    char error[256] = {0}, status[512];
    ds_native_engine_t *next = calloc(1, sizeof(*next));
    unsigned rate = g_host && g_host->sample_rate > 0 ? (unsigned)g_host->sample_rate : 44100;
    cancel_ctx_t cancel = {in, gen};
    const char *name = strrchr(path, '/') ? strrchr(path, '/') + 1 : path;
    int rc;
    if (!next) return -1;
    seqstr_write(&in->status, "Loading...");
    atomic_store(&in->loading, 1);
    rc = ds_native_engine_load(next, path, rate, superseded, &cancel, error, sizeof(error));
    atomic_store(&in->loading, 0);
    if (rc) {
        free(next);
        if (rc == DS_LOAD_CANCELLED) return rc;
        snprintf(status, sizeof(status), "Error: %s", error[0] ? error : "cannot load DSPreset");
        seqstr_write(&in->status, status);
        snprintf(status, sizeof(status), "load failed: %s (%s)", error, path);
        log_line(status);
        return -1;
    }
    if (next->missing_zones)
        snprintf(status, sizeof(status), "%s: %u/%u zones, %u files missing", name,
                 next->zone_count - next->missing_zones, next->zone_count, next->missing_files);
    else
        snprintf(status, sizeof(status), "%s: %u zones", name, next->zone_count);
    int restoring = in->restore_path[0] && !strcmp(in->restore_path, path);
    if (restoring) {
        /* A restored project: put its controls back before anyone hears it. */
        const char *q = in->restore_controls;
        next->initialising = 1;                  /* a restore is a load: triggerOnLoad="false" waits */
        for (unsigned i = 0; i < next->model.control_count && *q; ++i) {
            char *tail;
            float v = strtof(q, &tail);
            if (tail != q) ds_native_engine_set_control(next, i, v);
            q = strchr(tail, ';');
            if (!q) break;
            ++q;
        }
        next->initialising = 0;
        in->restore_path[0] = '\0';
    }
    atomic_store(&in->ctl_dirty, 0);
    /* A preset CHOSEN starts with Override off and Polyphony at "Preset" (Josh,
     * 2026-09-18); a project reopening keeps what it was saved with. */
    if (!restoring) { atomic_store(&in->amp_on, 0); atomic_store(&in->polyphony, 0); }
    if (!atomic_load(&in->amp_on)) {          /* Override off: the knobs show the preset's own envelope */
        float env[4];
        if (ds_native_engine_preset_envelope(next, env))
            for (int i = 0; i < 4; ++i) atomic_store(&in->amp_value[i], env[i] < 0 ? 0 : env[i] > AMP_MAX[i] ? AMP_MAX[i] : env[i]);
    }
    retire_engine(in, atomic_exchange(&in->active, next));
    atomic_fetch_add(&in->load_count, 1);
    seqstr_write(&in->loaded_path, path);
    seqstr_write(&in->status, status);
    { char line[600]; snprintf(line, sizeof(line), "loaded %s", status); log_line(line); }
    return 0;
}

/* A path from the host (restored state, or a direct pick). Inside the catalog
 * it becomes a bank/preset selection; outside, a direct file. Either way it
 * loads at once — a restore is not somebody scrolling. */
static void take_request(dspreset_instance_t *in, uint32_t *settled_gen) {
    char path[1024];
    int bank, preset;
    ds_catalog_t *c;
    seqstr_read(&in->request, path, sizeof(path));
    if (!path[0]) return;
    snprintf(in->restore_path, sizeof(in->restore_path), "%s", path);
    seqstr_read(&in->request_controls, in->restore_controls, sizeof(in->restore_controls));
    c = atomic_load(&in->catalog);
    if (!c || ds_catalog_find(c, path, &bank, &preset)) {
        /* An unpacked preset under a .dslibrary's folder may be newer than the scan. */
        rescan(in);
        c = atomic_load(&in->catalog);
    }
    if (c && !ds_catalog_find(c, path, &bank, &preset)) {
        select_bank_preset(in, bank, preset);
    } else {
        /* A .dslibrary from elsewhere: unpack beside it, play its first preset. */
        size_t len = strlen(path);
        snprintf(in->direct_path, sizeof(in->direct_path), "%s", path);
        if (len > 10 && !strcasecmp(path + len - 10, ".dslibrary")) {
            char unpacked[1100];
            struct stat st;
            snprintf(unpacked, sizeof(unpacked), "%s.unpacked", path);
            if (stat(unpacked, &st) && unpack_bank(in, path, FILE_BANK)) return;
            in->restore_path[0] = '\0';         /* its controls belonged to no preset in particular */
            if (ds_catalog_first_preset(unpacked, in->direct_path, sizeof(in->direct_path))) {
                seqstr_write(&in->status, "Error: DSLibrary has no DSPreset");
                return;
            }
        }
        select_bank_preset(in, FILE_BANK, 0);
    }
    *settled_gen = atomic_load(&in->sel_gen);   /* skip the settle delay */
}

static void *engine_worker(void *opaque) {
    dspreset_instance_t *in = opaque;
    uint32_t seen_gen = UINT32_MAX, seen_request = 0, settled_gen = UINT32_MAX, failed_gen = UINT32_MAX;
    uint64_t changed_at = 0, scanned_at = 0, started_at, perf_at = 0;
    int first_pick_done = 0;
    rescan(in);
    scanned_at = started_at = now_ms();
    while (atomic_load(&in->worker_running)) {
        uint32_t gen = atomic_load(&in->sel_gen), request = atomic_load(&in->request_gen);
        ds_native_engine_t *engine;
        uint64_t t = now_ms();
        if (request != seen_request) { seen_request = request; first_pick_done = 1; take_request(in, &settled_gen); gen = atomic_load(&in->sel_gen); }
        if (!first_pick_done && t - started_at >= FIRST_PICK_MS) {
            /* Nothing restored: start on the first bank that can play now —
             * never one that would have to be unpacked first. */
            ds_catalog_t *c = atomic_load(&in->catalog);
            first_pick_done = 1;
            for (unsigned i = 0; c && i < c->bank_count; ++i)
                if (c->banks[i].preset_count && atomic_load(&in->sel_bank) == NO_BANK) { select_bank_preset(in, (int)i, 0); break; }
            gen = atomic_load(&in->sel_gen);
        }
        if (gen != seen_gen) { seen_gen = gen; changed_at = t; }
        if (t - changed_at >= SETTLE_MS) settled_gen = gen;
        if (settled_gen == gen && failed_gen != gen) {
            char target[1100], loaded[1024];
            int needs_unpack;
            selected_target(in, target, sizeof(target), &needs_unpack);
            seqstr_read(&in->loaded_path, loaded, sizeof(loaded));
            if (needs_unpack) {
                if (unpack_bank(in, target, atomic_load(&in->sel_bank))) failed_gen = gen;
                continue;                          /* now its presets are known */
            }
            if (target[0] && strcmp(target, loaded)) {
                int rc = load_target(in, target, gen);
                if (rc && rc != DS_LOAD_CANCELLED) failed_gen = gen;
                continue;
            }
            /* Nothing left to do for this pick: the ready edge the host's
             * pages wait for before re-reading this preset's controls. */
            if (atomic_load(&in->sel_gen) == gen && atomic_load(&in->request_gen) == seen_request)
                atomic_store(&in->busy, 0);
        } else if (failed_gen == gen && atomic_load(&in->request_gen) == seen_request) {
            atomic_store(&in->busy, 0);
        }
        if (t - scanned_at >= RESCAN_MS) { rescan(in); scanned_at = t; }
        if (t - perf_at >= PERF_REPORT_MS) {
            uint32_t blocks = atomic_exchange(&in->perf_blocks, 0), voices = atomic_exchange(&in->perf_voices_max, 0);
            uint64_t sum = atomic_exchange(&in->perf_ns_sum, 0), max = atomic_exchange(&in->perf_ns_max, 0);
            ds_native_engine_t *e = atomic_load(&in->active);
            perf_at = t;
            if (blocks && voices) {
                char line[200];
                snprintf(line, sizeof(line), "perf: %u blocks, render mean %.1f us, max %.1f us, voices max %u, underruns %u",
                         blocks, sum / 1000.0 / blocks, max / 1000.0, voices, e ? atomic_load(&e->underruns) : 0);
                log_line(line);
            }
        }
        engine = atomic_load(&in->active);
        if (!engine || !ds_native_engine_service(engine)) usleep(1000);
    }
    return NULL;
}

/* ---- plugin API --------------------------------------------------------- */

static void *create_instance(const char *module_dir, const char *json_defaults) {
    dspreset_instance_t *in = calloc(1, sizeof(*in));
    (void)json_defaults;
    if (!in) return NULL;
    atomic_store(&in->gain, 0.7f);
    atomic_store(&in->amp_value[AMP_SUSTAIN], 1.0f);
    atomic_store(&in->amp_value[AMP_RELEASE], 0.5f);
    atomic_store(&in->unpacking_bank, -1);
    atomic_store(&in->sel_bank, NO_BANK);
    snprintf(in->module_dir, sizeof(in->module_dir), "%s", module_dir ? module_dir : ".");
    snprintf(in->instruments, sizeof(in->instruments), "%s/instruments", in->module_dir);
    mkdir(in->instruments, 0777);
    seqstr_write(&in->status, "No preset");
    atomic_store(&in->worker_running, 1);
    if (pthread_create(&in->worker, NULL, engine_worker, in)) { free(in); return NULL; }
    return in;
}

static void destroy_instance(void *opaque) {
    dspreset_instance_t *in = opaque;
    retired_t *node;
    if (!in) return;
    atomic_store(&in->worker_running, 0);
    pthread_join(in->worker, NULL);
    retire_engine(in, atomic_exchange(&in->active, NULL));
    ds_catalog_free(atomic_load(&in->catalog));
    while ((node = in->retired) != NULL) { in->retired = node->next; ds_catalog_free(node->catalog); free(node); }
    free(in);
}

/* The module's envelope into the engine: on the audio thread, the engine's only writer. */
static void sync_amp(dspreset_instance_t *in, ds_native_engine_t *engine) {
    int on = atomic_load_explicit(&in->amp_on, memory_order_relaxed);
    engine->poly_limit = atomic_load_explicit(&in->polyphony, memory_order_relaxed);
    for (int i = 0; i < 4; ++i)
        engine->amp_override[i] = on ? atomic_load_explicit(&in->amp_value[i], memory_order_relaxed) : -1.0f;
}

static void on_midi(void *opaque, const uint8_t *msg, int len, int source) {
    dspreset_instance_t *in = opaque;
    ds_native_engine_t *engine;
    (void)source;
    if (!in || len < 2) return;
    atomic_fetch_add(&in->audio_users, 1);
    engine = atomic_load(&in->active);
    if (engine) {
        sync_amp(in, engine);
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

static void request_path(dspreset_instance_t *in, const char *path) {
    atomic_store(&in->busy, 1);
    seqstr_write(&in->request, path);
    atomic_fetch_add(&in->request_gen, 1);
}

/* Pulls one string field out of a flat JSON object; handles \" and \\. */
static int json_string(const char *json, const char *key, char *out, size_t n) {
    char pattern[64];
    const char *p;
    size_t k = 0;
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    if (!(p = strstr(json, pattern))) return 0;
    p += strlen(pattern);
    while (*p == ' ' || *p == ':') ++p;
    if (*p != '"') return 0;
    for (++p; *p && *p != '"' && k + 1 < n; ++p) {
        if (*p == '\\' && p[1]) ++p;
        out[k++] = *p;
    }
    out[k] = '\0';
    return 1;
}

static void set_param(void *opaque, const char *key, const char *value) {
    dspreset_instance_t *in = opaque;
    ds_catalog_t *c;
    if (!in || !key || !value) return;
    c = atomic_load(&in->catalog);
    if (!strcmp(key, "bank")) {
        int bank = atoi(value);
        if (!c || bank < 0 || bank >= (int)c->bank_count) return;
        if (bank != atomic_load(&in->sel_bank)) { atomic_store(&in->busy, 1); select_bank_preset(in, bank, 0); }
    } else if (!strcmp(key, "preset")) {
        int bank = atomic_load(&in->sel_bank), preset = atoi(value);
        if (!c || bank < 0 || bank >= (int)c->bank_count) return;
        if (preset < 0 || preset >= (int)c->banks[bank].preset_count) return;
        if (preset != atomic_load(&in->sel_preset)) { atomic_store(&in->busy, 1); select_bank_preset(in, bank, preset); }
    } else if (!strcmp(key, "polyphony")) {
        /* an index into Preset, 1..64 — which is also the number itself */
        int n = !strcasecmp(value, "Preset") ? 0 : atoi(value);
        atomic_store(&in->polyphony, n < 0 ? 0 : n > DS_MAX_VOICES ? DS_MAX_VOICES : n);
    } else if (!strcmp(key, "amp_override")) {
        atomic_store(&in->amp_on, !strcmp(value, "1") || !strcasecmp(value, "On"));
    } else if (amp_stage_of(key) >= 0) {
        int stage = amp_stage_of(key);
        float v = strtof(value, NULL);
        atomic_store(&in->amp_value[stage], v < 0 ? 0 : v > AMP_MAX[stage] ? AMP_MAX[stage] : v);
    } else if (!strncmp(key, "ctl_", 4)) {
        int i = atoi(key + 4);
        if (i < 0 || i >= DS_MAX_CONTROLS) return;
        atomic_store(&in->ctl_pending[i], strtof(value, NULL));
        atomic_fetch_or(&in->ctl_dirty, 1ull << i);
    } else if (!strcmp(key, "preset_path")) {
        seqstr_write(&in->request_controls, "");
        request_path(in, value);
    } else if (!strcmp(key, "state")) {
        char path[1024], gain[32], controls[1024];
        if (json_string(value, "gain", gain, sizeof(gain))) atomic_store(&in->gain, strtof(gain, NULL));
        if (!json_string(value, "controls", controls, sizeof(controls))) controls[0] = '\0';
        {
            char amp[128];
            char poly[16];
            if (json_string(value, "polyphony", poly, sizeof(poly))) {
                int n = atoi(poly);
                atomic_store(&in->polyphony, n < 0 ? 0 : n > DS_MAX_VOICES ? DS_MAX_VOICES : n);
            }
            if (json_string(value, "amp", amp, sizeof(amp))) {           /* "on;attack;decay;sustain;release" */
                char *q = amp, *tail;
                atomic_store(&in->amp_on, strtol(q, &tail, 10) != 0);
                for (int i = 0; i < 4 && *tail == ';'; ++i) {
                    float v = strtof(q = tail + 1, &tail);
                    if (tail == q) break;
                    atomic_store(&in->amp_value[i], v < 0 ? 0 : v > AMP_MAX[i] ? AMP_MAX[i] : v);
                }
            }
        }
        seqstr_write(&in->request_controls, controls);
        if (json_string(value, "preset_path", path, sizeof(path)) && path[0]) request_path(in, path);
    } else if (!strcmp(key, "gain")) {
        atomic_store(&in->gain, strtof(value, NULL));
    }
}

/* Appends `text` to out as JSON string content. */
static int json_escape(char *out, int n, const char *text) {
    int k = 0;
    for (; *text && k < n - 2; ++text) {
        if (*text == '"' || *text == '\\') out[k++] = '\\';
        out[k++] = (unsigned char)*text < 0x20 ? ' ' : *text;
    }
    out[k] = '\0';
    return k;
}

static int finish(int n, int out_len) { return n < 0 ? -1 : n < out_len ? n : out_len - 1; }

/* Appends to out[k..n); returns the new length, never past n-1. */
static int append(char *out, int k, int n, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
static int append(char *out, int k, int n, const char *fmt, ...) {
    va_list ap;
    int w;
    if (k >= n - 1) return k;
    va_start(ap, fmt);
    w = vsnprintf(out + k, (size_t)(n - k), fmt, ap);
    va_end(ap);
    return w < 0 ? k : (k + w < n ? k + w : n - 1);
}

/* The preset's own controls come first on the knobs, Gain after them: the
 * page you land on plays the preset the way its author laid it out. */
/* The Amp Envelope page puts Attack..Release on knobs 1-4 — one ROW of the
 * grid, which is what lets both hosts draw them as an envelope (a graphic
 * never straddles the row break; with Override first they drew as four
 * faders) — and Override on knob 5. tests/test_pages.mjs checks it with the
 * hosts' own planner. */
static int write_hierarchy(const ds_native_engine_t *e, char *out, int n) {
    int k = append(out, 0, n, "{\"levels\":{\"root\":{\"name\":\"DSPreset\",\"list_param\":\"preset\","
                   "\"count_param\":\"preset_count\",\"name_param\":\"preset_name\","
                   "\"params\":[{\"level\":\"banks\",\"label\":\"Banks\"}");
    unsigned controls = e ? e->model.control_count : 0;
    for (unsigned i = 0; i < controls; ++i) k = append(out, k, n, ",\"ctl_%u\"", i);
    k = append(out, k, n, ",{\"level\":\"amp\",\"label\":\"Amp Envelope\"},\"gain\"],\"knobs\":[");
    for (unsigned i = 0; i < controls; ++i) k = append(out, k, n, "\"ctl_%u\",", i);
    return append(out, k, n, "\"gain\"]},\"banks\":{\"name\":\"Banks\",\"label\":\"Select Bank\","
                  "\"items_param\":\"bank_list\",\"select_param\":\"bank\",\"navigate_to\":\"root\"},"
                  "\"amp\":{\"name\":\"Amp Envelope\",\"params\":[\"amp_attack\",\"amp_decay\",\"amp_sustain\",\"amp_release\",\"amp_override\",\"polyphony\"],"
                  "\"knobs\":[\"amp_attack\",\"amp_decay\",\"amp_sustain\",\"amp_release\",\"amp_override\",\"polyphony\"]}}}");
}

static int write_chain_params(const ds_native_engine_t *e, unsigned banks, unsigned presets, char *out, int n) {
    int k = append(out, 0, n, "[{\"key\":\"preset\",\"name\":\"Preset\",\"type\":\"int\",\"min\":0,\"max\":%u},"
                   "{\"key\":\"bank\",\"name\":\"Bank\",\"type\":\"int\",\"min\":0,\"max\":%u},"
                   "{\"key\":\"gain\",\"name\":\"Gain\",\"type\":\"float\",\"min\":0,\"max\":2,\"step\":0.02,\"default\":0.7}",
                   presets ? presets - 1 : 0, banks ? banks - 1 : 0);
    k = append(out, k, n, ",{\"key\":\"polyphony\",\"name\":\"Polyphony\",\"type\":\"enum\",\"options\":[\"Preset\"");
    for (int v = 1; v <= DS_MAX_VOICES; ++v) k = append(out, k, n, ",\"%d\"", v);
    k = append(out, k, n, "],\"default\":0}");
    k = append(out, k, n, ",{\"key\":\"amp_override\",\"name\":\"Override\",\"type\":\"enum\",\"options\":[\"Off\",\"On\"],\"default\":0}"
                   ",{\"key\":\"amp_attack\",\"name\":\"Attack\",\"type\":\"float\",\"min\":0,\"max\":10,\"step\":0.001,\"unit\":\"sec\",\"default\":0,"
                   "\"viz\":{\"kind\":\"envelope\",\"group\":\"amp_env\",\"role\":\"attack\"}}"
                   ",{\"key\":\"amp_decay\",\"name\":\"Decay\",\"type\":\"float\",\"min\":0,\"max\":10,\"step\":0.001,\"unit\":\"sec\",\"default\":0,"
                   "\"viz\":{\"kind\":\"envelope\",\"group\":\"amp_env\",\"role\":\"decay\"}}"
                   ",{\"key\":\"amp_sustain\",\"name\":\"Sustain\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01,\"unit\":\"%%\",\"default\":1,"
                   "\"viz\":{\"kind\":\"envelope\",\"group\":\"amp_env\",\"role\":\"sustain\"}}"
                   ",{\"key\":\"amp_release\",\"name\":\"Release\",\"type\":\"float\",\"min\":0,\"max\":20,\"step\":0.001,\"unit\":\"sec\",\"default\":0.5,"
                   "\"viz\":{\"kind\":\"envelope\",\"group\":\"amp_env\",\"role\":\"release\"}}");
    for (unsigned i = 0; e && i < e->model.control_count; ++i) {
        const ds_control_t *c = &e->model.controls[i];
        char name[80];
        json_escape(name, sizeof(name), c->name);
        if (c->kind == DS_CONTROL_KNOB && !c->integer) {
            k = append(out, k, n, ",{\"key\":\"ctl_%u\",\"name\":\"%s\",\"type\":\"float\",\"min\":%g,\"max\":%g,"
                       "\"step\":%g,\"default\":%g}", i, name, (double)c->min, (double)c->max,
                       (double)((c->max - c->min) / 100.0f), (double)c->def);
        } else if (c->kind == DS_CONTROL_KNOB) {
            k = append(out, k, n, ",{\"key\":\"ctl_%u\",\"name\":\"%s\",\"type\":\"int\",\"min\":%g,\"max\":%g,\"default\":%g}",
                       i, name, (double)c->min, (double)c->max, (double)c->def);
        } else {
            k = append(out, k, n, ",{\"key\":\"ctl_%u\",\"name\":\"%s\",\"type\":\"enum\",\"options\":[", i, name);
            for (unsigned o = 0; o < c->choice_count; ++o) {
                char option[80];
                json_escape(option, sizeof(option), e->model.choices[c->first_choice + o].name);
                k = append(out, k, n, "%s\"%s\"", o ? "," : "", option);
            }
            k = append(out, k, n, "],\"default\":%d}", (int)c->def);
        }
    }
    return append(out, k, n, "]");
}

static int get_param(void *opaque, const char *key, char *out, int out_len) {
    dspreset_instance_t *in = opaque;
    ds_catalog_t *c;
    int bank, preset;
    const ds_bank_t *b;
    if (!in || !key || !out || out_len <= 0) return -1;
    c = atomic_load(&in->catalog);
    bank = atomic_load(&in->sel_bank);
    preset = atomic_load(&in->sel_preset);
    b = (c && bank >= 0 && bank < (int)c->bank_count) ? &c->banks[bank] : NULL;

    if (!strcmp(key, "ui_hierarchy") || !strcmp(key, "chain_params") || !strncmp(key, "ctl_", 4) || !strcmp(key, "state")) {
        /* Everything that reads the loaded preset holds it against a swap. */
        ds_native_engine_t *engine;
        int k = -1;
        atomic_fetch_add(&in->audio_users, 1);
        engine = atomic_load(&in->active);
        if (!strcmp(key, "ui_hierarchy")) k = write_hierarchy(engine, out, out_len);
        else if (!strcmp(key, "chain_params")) k = write_chain_params(engine, c ? c->bank_count : 0, b ? b->preset_count : 0, out, out_len);
        else if (!strcmp(key, "state")) {
            char path[1024], escaped[1100];
            seqstr_read(&in->loaded_path, path, sizeof(path));
            json_escape(escaped, sizeof(escaped), path);
            k = append(out, 0, out_len, "{\"preset_path\":\"%s\",\"gain\":\"%.3f\",\"controls\":\"", escaped,
                       (double)atomic_load(&in->gain));
            for (unsigned i = 0; engine && i < engine->model.control_count; ++i)
                k = append(out, k, out_len, "%s%g", i ? ";" : "", (double)engine->control_value[i]);
            k = append(out, k, out_len, "\",\"amp\":\"%d;%g;%g;%g;%g\"", atomic_load(&in->amp_on),
                       (double)atomic_load(&in->amp_value[0]), (double)atomic_load(&in->amp_value[1]),
                       (double)atomic_load(&in->amp_value[2]), (double)atomic_load(&in->amp_value[3]));
            k = append(out, k, out_len, ",\"polyphony\":\"%d\"}", atomic_load(&in->polyphony));
        } else {
            unsigned i = (unsigned)atoi(key + 4);
            if (engine && i < engine->model.control_count) {
                float v = (atomic_load(&in->ctl_dirty) & (1ull << i)) ? atomic_load(&in->ctl_pending[i]) : engine->control_value[i];
                k = engine->model.controls[i].kind == DS_CONTROL_KNOB && !engine->model.controls[i].integer
                    ? append(out, 0, out_len, "%.4f", (double)v) : append(out, 0, out_len, "%d", (int)v);
            }
        }
        atomic_fetch_sub(&in->audio_users, 1);
        return k;
    }
    if (!strcmp(key, "polyphony")) return finish(snprintf(out, (size_t)out_len, "%d", atomic_load(&in->polyphony)), out_len);
    if (!strcmp(key, "amp_override")) return finish(snprintf(out, (size_t)out_len, "%d", atomic_load(&in->amp_on)), out_len);
    if (amp_stage_of(key) >= 0) return finish(snprintf(out, (size_t)out_len, "%.4f", (double)atomic_load(&in->amp_value[amp_stage_of(key)])), out_len);
    if (!strcmp(key, "is_loading")) return finish(snprintf(out, (size_t)out_len, "%d", atomic_load(&in->busy) ? 1 : 0), out_len);
    if (!strcmp(key, "bank_list")) {
        int k = snprintf(out, (size_t)out_len, "[");
        for (unsigned i = 0; c && i < c->bank_count; ++i) {
            char label[300];
            int need;
            json_escape(label, sizeof(label), c->banks[i].name);
            need = snprintf(NULL, 0, "%s{\"label\":\"%s\",\"index\":%u}", i ? "," : "", label, i);
            if (k + need + 2 >= out_len) break;           /* keep the JSON whole */
            k += snprintf(out + k, (size_t)(out_len - k), "%s{\"label\":\"%s\",\"index\":%u}", i ? "," : "", label, i);
        }
        return finish(k + snprintf(out + k, (size_t)(out_len - k), "]"), out_len);
    }
    if (!strcmp(key, "bank")) return finish(snprintf(out, (size_t)out_len, "%d", bank < 0 ? 0 : bank), out_len);
    if (!strcmp(key, "bank_count")) return finish(snprintf(out, (size_t)out_len, "%u", c ? c->bank_count : 0), out_len);
    if (!strcmp(key, "bank_name")) return finish(snprintf(out, (size_t)out_len, "%s", b ? b->name : bank == FILE_BANK ? "File" : ""), out_len);
    if (!strcmp(key, "preset")) return finish(snprintf(out, (size_t)out_len, "%d", preset), out_len);
    if (!strcmp(key, "preset_count")) return finish(snprintf(out, (size_t)out_len, "%u", b ? b->preset_count : bank == FILE_BANK ? 1 : 0), out_len);
    if (!strcmp(key, "preset_name")) {
        const char *name;
        char path[1024];
        if (b && preset >= 0 && preset < (int)b->preset_count) name = b->preset_names[preset];
        else if (b && atomic_load(&in->unpacking_bank) == bank) name = "Unpacking...";
        else if (b && b->kind == DS_BANK_DSLIBRARY && !b->prepared) name = "Not unpacked";
        else if (bank == FILE_BANK) { seqstr_read(&in->loaded_path, path, sizeof(path)); name = strrchr(path, '/') ? strrchr(path, '/') + 1 : path; }
        else if (bank == NO_BANK) name = c && c->bank_count ? "Choose a bank" : "No libraries";
        else name = "No presets";
        return finish(snprintf(out, (size_t)out_len, "%s", name), out_len);
    }
    if (!strcmp(key, "preset_path")) { seqstr_read(&in->loaded_path, out, (size_t)out_len); return (int)strlen(out); }
    if (!strcmp(key, "status")) { seqstr_read(&in->status, out, (size_t)out_len); return (int)strlen(out); }
    if (!strcmp(key, "gain")) return finish(snprintf(out, (size_t)out_len, "%.3f", (double)atomic_load(&in->gain)), out_len);
    if (!strcmp(key, "loading")) return finish(snprintf(out, (size_t)out_len, "%d", atomic_load(&in->loading)), out_len);
    if (!strcmp(key, "load_count")) return finish(snprintf(out, (size_t)out_len, "%u", atomic_load(&in->load_count)), out_len);
    if (!strcmp(key, "underruns") || !strcmp(key, "voices")) {
        ds_native_engine_t *engine;
        unsigned value = 0;
        atomic_fetch_add(&in->audio_users, 1);
        engine = atomic_load(&in->active);
        if (engine) value = !strcmp(key, "voices") ? ds_native_engine_active_voices(engine) : atomic_load(&engine->underruns);
        atomic_fetch_sub(&in->audio_users, 1);
        return finish(snprintf(out, (size_t)out_len, "%u", value), out_len);
    }
    return -1;
}

static int get_error(void *opaque, char *out, int out_len) {
    dspreset_instance_t *in = opaque;
    char status[1024];
    if (!in || !out || out_len <= 0) return -1;
    seqstr_read(&in->status, status, sizeof(status));
    return finish(snprintf(out, (size_t)out_len, "%s", strncmp(status, "Error: ", 7) ? "" : status + 7), out_len);
}

/* Exact below 0.9 (-0.9 dBFS); above, it bends smoothly to a ceiling of 1.0
 * instead of squaring off. A preset's EQ can add +10 dB (Capture's does, by
 * default), and a hard clip there is the sound of something broken. */
static inline float soft_clip(float x) {
    float a = fabsf(x);
    if (a <= 0.9f) return x;
    a = 0.9f + 0.1f * tanhf((a - 0.9f) / 0.1f);
    return x < 0 ? -a : a;
}

static void render_block(void *opaque, int16_t *out, int frames) {
    dspreset_instance_t *in = opaque;
    ds_native_engine_t *engine;
    float buffer[2 * 256], gain;
    struct timespec t0, t1;
    if (!in || !out || frames <= 0) return;
    if (frames > 256) frames = 256;
    clock_gettime(CLOCK_MONOTONIC, &t0);             /* vDSO: no syscall, safe here */
    memset(buffer, 0, (size_t)frames * 2 * sizeof(float));
    atomic_fetch_add(&in->audio_users, 1);
    engine = atomic_load(&in->active);
    if (engine) {
        uint64_t dirty = atomic_exchange(&in->ctl_dirty, 0);
        sync_amp(in, engine);
        for (unsigned i = 0; dirty; ++i, dirty >>= 1)
            if (dirty & 1) ds_native_engine_set_control(engine, i, atomic_load(&in->ctl_pending[i]));
        ds_native_engine_render(engine, buffer, (unsigned)frames);
        {
            unsigned voices = ds_native_engine_active_voices(engine);
            if (voices > atomic_load_explicit(&in->perf_voices_max, memory_order_relaxed))
                atomic_store_explicit(&in->perf_voices_max, voices, memory_order_relaxed);
        }
    }
    atomic_fetch_sub(&in->audio_users, 1);
    gain = atomic_load(&in->gain);
    for (int i = 0; i < frames * 2; ++i) out[i] = (int16_t)(soft_clip(buffer[i] * gain) * 32767);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    {
        uint64_t ns = (uint64_t)(t1.tv_sec - t0.tv_sec) * 1000000000u + (uint64_t)(t1.tv_nsec - t0.tv_nsec);
        atomic_fetch_add_explicit(&in->perf_ns_sum, ns, memory_order_relaxed);
        atomic_fetch_add_explicit(&in->perf_blocks, 1, memory_order_relaxed);
        if (ns > atomic_load_explicit(&in->perf_ns_max, memory_order_relaxed))
            atomic_store_explicit(&in->perf_ns_max, ns, memory_order_relaxed);
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
