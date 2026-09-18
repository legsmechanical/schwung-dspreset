/* Shared by the tests: synthetic WAV/preset writers, and a driver that runs the
 * REAL plugin (worker thread, engine swap, streaming) at real-time pace, the
 * way the Move's audio thread calls it. */
#ifndef DSPRESET_TEST_SUPPORT_H
#define DSPRESET_TEST_SUPPORT_H

#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); exit(1); } } while (0)

static void put16(FILE *f, unsigned v) { fputc(v & 255, f); fputc((v >> 8) & 255, f); }
static void put32(FILE *f, uint32_t v) { put16(f, v & 0xffff); put16(f, v >> 16); }

/* The integer sample the test signal holds at (frame, channel), for 24-bit. */
static int32_t test_signal24(uint64_t frame, unsigned channel) {
    double x = 0.45 * sin((double)frame * (0.013 + 0.007 * channel)) + 0.2 * sin((double)frame * 0.0011);
    return (int32_t)lrint(x * 8388607.0);
}

/* 24-bit PCM, optional 'smpl' loop (inclusive end) written AFTER 'data', as
 * many editors do. */
static void write_wav24(const char *path, unsigned rate, unsigned channels, unsigned frames,
                        long loop_start, long loop_end) {
    FILE *f = fopen(path, "wb");
    uint32_t data = frames * channels * 3, smpl = loop_start >= 0 ? 36 + 24 : 0;
    CHECK(f);
    fwrite("RIFF", 1, 4, f); put32(f, 4 + 8 + 16 + 8 + data + (data & 1) + (smpl ? 8 + smpl : 0));
    fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f); put32(f, 16); put16(f, 1); put16(f, channels); put32(f, rate);
    put32(f, rate * channels * 3); put16(f, channels * 3); put16(f, 24);
    fwrite("data", 1, 4, f); put32(f, data);
    for (unsigned i = 0; i < frames; ++i) for (unsigned c = 0; c < channels; ++c) {
        int32_t v = test_signal24(i, c);
        fputc(v & 255, f); fputc((v >> 8) & 255, f); fputc((v >> 16) & 255, f);
    }
    if (data & 1) fputc(0, f);
    if (smpl) {
        fwrite("smpl", 1, 4, f); put32(f, smpl);
        for (int i = 0; i < 7; ++i) put32(f, 0);
        put32(f, 1); put32(f, 0);                      /* one loop, no sampler data */
        put32(f, 0); put32(f, 0);                      /* cue id, type forward */
        put32(f, (uint32_t)loop_start); put32(f, (uint32_t)loop_end);
        put32(f, 0); put32(f, 0);
    }
    fclose(f);
}

/* A mono 24-bit sine, for measuring what a filter does to one frequency. */
static void write_sine24(const char *path, unsigned frames, double hz, double amplitude) {
    FILE *f = fopen(path, "wb");
    uint32_t data = frames * 3;
    CHECK(f);
    fwrite("RIFF", 1, 4, f); put32(f, 4 + 8 + 16 + 8 + data + (data & 1));
    fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f); put32(f, 16); put16(f, 1); put16(f, 1); put32(f, 44100);
    put32(f, 44100 * 3); put16(f, 3); put16(f, 24);
    fwrite("data", 1, 4, f); put32(f, data);
    for (unsigned i = 0; i < frames; ++i) {
        int32_t v = (int32_t)lrint(amplitude * sin(2 * M_PI * hz * i / 44100.0) * 8388607.0);
        fputc(v & 255, f); fputc((v >> 8) & 255, f); fputc((v >> 16) & 255, f);
    }
    if (data & 1) fputc(0, f);
    fclose(f);
}

/* 24-bit big-endian AIFF carrying the same signal as write_wav24, with an
 * INST sustain loop over MARK markers when loop_start >= 0 (end exclusive). */
static void put16be(FILE *f, unsigned v) { fputc((v >> 8) & 255, f); fputc(v & 255, f); }
static void put32be(FILE *f, uint32_t v) { put16be(f, v >> 16); put16be(f, v & 0xffff); }
static void write_aiff24(const char *path, unsigned channels, unsigned frames, long loop_start, long loop_end) {
    FILE *f = fopen(path, "wb");
    uint32_t data = frames * channels * 3, mark = loop_start >= 0 ? 2 + 2 * (6 + 4) : 0, inst = loop_start >= 0 ? 20 : 0;
    static const unsigned char rate44100[10] = {0x40, 0x0e, 0xac, 0x44, 0, 0, 0, 0, 0, 0};
    CHECK(f);
    fwrite("FORM", 1, 4, f);
    put32be(f, 4 + 8 + 18 + (mark ? 8 + mark : 0) + (inst ? 8 + inst : 0) + 8 + 8 + data + (data & 1));
    fwrite("AIFF", 1, 4, f);
    fwrite("COMM", 1, 4, f); put32be(f, 18); put16be(f, channels); put32be(f, frames); put16be(f, 24); fwrite(rate44100, 1, 10, f);
    if (mark) {   /* two markers, each: id, position, pstring "Lp" (len 2 + text, padded to 4) */
        fwrite("MARK", 1, 4, f); put32be(f, mark); put16be(f, 2);
        put16be(f, 1); put32be(f, (uint32_t)loop_start); fputc(2, f); fwrite("Lb", 1, 2, f); fputc(0, f);
        put16be(f, 2); put32be(f, (uint32_t)loop_end); fputc(2, f); fwrite("Le", 1, 2, f); fputc(0, f);
        fwrite("INST", 1, 4, f); put32be(f, inst);
        fputc(60, f); fputc(0, f); fputc(0, f); fputc(127, f); fputc(0, f); fputc(127, f); put16be(f, 0);
        put16be(f, 1); put16be(f, 1); put16be(f, 2);   /* sustain loop: forward, marker 1 .. marker 2 */
        put16be(f, 0); put16be(f, 0); put16be(f, 0);   /* release loop: off */
    }
    fwrite("SSND", 1, 4, f); put32be(f, 8 + data); put32be(f, 0); put32be(f, 0);
    for (unsigned i = 0; i < frames; ++i) for (unsigned c = 0; c < channels; ++c) {
        int32_t v = test_signal24(i, c);
        fputc((v >> 16) & 255, f); fputc((v >> 8) & 255, f); fputc(v & 255, f);
    }
    if (data & 1) fputc(0, f);
    fclose(f);
}

static void write_text(const char *path, const char *text) {
    FILE *f = fopen(path, "wb");
    CHECK(f);
    fputs(text, f);
    fclose(f);
}

/* ---- the plugin, as the host sees it ---------------------------------- */

typedef struct host_api_v1 { uint32_t api_version; int sample_rate, frames_per_block; uint8_t *mapped_memory; int audio_out_offset, audio_in_offset; void (*log)(const char *); int (*midi_send_internal)(const uint8_t *, int); int (*midi_send_external)(const uint8_t *, int); } host_api_v1_t;
typedef struct plugin_api_v2 { uint32_t api_version; void *(*create_instance)(const char *, const char *); void (*destroy_instance)(void *); void (*on_midi)(void *, const uint8_t *, int, int); void (*set_param)(void *, const char *, const char *); int (*get_param)(void *, const char *, char *, int); int (*get_error)(void *, char *, int); void (*render_block)(void *, int16_t *, int); } plugin_api_v2_t;
extern plugin_api_v2_t *move_plugin_init_v2(const host_api_v1_t *host);

static void test_log(const char *line) { fprintf(stderr, "  [log] %s\n", line); }
static host_api_v1_t g_test_host = {.api_version = 1, .sample_rate = 44100, .frames_per_block = 128, .log = test_log};

typedef struct {
    plugin_api_v2_t *api;
    void *instance;
    struct timespec next;               /* real-time pacing deadline */
} plugin_t;

/* `module_dir` holds instruments/ — the catalog the Banks list is built from. */
static void plugin_open_in(plugin_t *p, const char *module_dir) {
    CHECK(module_dir);
    p->api = move_plugin_init_v2(&g_test_host);
    CHECK(p->api && p->api->api_version == 2);
    p->instance = p->api->create_instance(module_dir, "{}");
    CHECK(p->instance);
    clock_gettime(CLOCK_MONOTONIC, &p->next);
}

static void plugin_open(plugin_t *p) {
    char dir[512];
    snprintf(dir, sizeof(dir), "%s/empty-module", getenv("TEST_TMP") ? getenv("TEST_TMP") : "/tmp");
    mkdir(dir, 0777);
    plugin_open_in(p, dir);
}

static void plugin_close(plugin_t *p) { p->api->destroy_instance(p->instance); }

static void plugin_get(plugin_t *p, const char *key, char *out, int n) {
    out[0] = '\0';
    p->api->get_param(p->instance, key, out, n);
}

static unsigned plugin_uint(plugin_t *p, const char *key) {
    char v[64]; plugin_get(p, key, v, sizeof(v)); return (unsigned)strtoul(v, NULL, 10);
}

/* Sets the path and waits until THIS request has been handled: an engine was
 * built (load_count rose) or the status turned to an error. Waiting only for
 * "not Loading" returned while the PREVIOUS preset's status still stood, and
 * the swap then landed under a sounding note. */
static const char *plugin_load(plugin_t *p, const char *path, char *status, int n) {
    unsigned before = plugin_uint(p, "load_count");
    p->api->set_param(p->instance, "preset_path", path);
    for (int i = 0; i < 6000; ++i) {                 /* 60 s */
        usleep(10000);
        plugin_get(p, "status", status, n);
        if (plugin_uint(p, "load_count") != before && !plugin_uint(p, "loading")) break;
        if (!strncmp(status, "Error", 5) && !plugin_uint(p, "loading")) break;
    }
    clock_gettime(CLOCK_MONOTONIC, &p->next);        /* pace from now, not from before the load */
    return status;
}

static void plugin_midi(plugin_t *p, uint8_t a, uint8_t b, uint8_t c) {
    uint8_t msg[3] = {a, b, c};
    p->api->on_midi(p->instance, msg, 3, 0);
}

/* One 128-frame block, released no earlier than real time would. */
static void plugin_render(plugin_t *p, int16_t *out) {
    struct timespec now, wait;
    clock_gettime(CLOCK_MONOTONIC, &now);
    wait.tv_sec = p->next.tv_sec - now.tv_sec;
    wait.tv_nsec = p->next.tv_nsec - now.tv_nsec;
    if (wait.tv_nsec < 0) { wait.tv_sec--; wait.tv_nsec += 1000000000L; }
    if (wait.tv_sec >= 0) nanosleep(&wait, NULL);
    p->next.tv_nsec += 128L * 1000000000L / 44100;
    if (p->next.tv_nsec >= 1000000000L) { p->next.tv_sec++; p->next.tv_nsec -= 1000000000L; }
    p->api->render_block(p->instance, out, 128);
}

/* The plugin's own int16 conversion, for exact comparisons. */
static int16_t expected_out(float x, float gain) {
    x *= gain;
    if (x > 1) x = 1;
    if (x < -1) x = -1;
    return (int16_t)(x * 32767);
}

#endif
