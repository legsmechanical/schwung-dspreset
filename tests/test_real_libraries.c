/* Real libraries through the real plugin at real-time pace. Paths come from
 * DSPRESET_CAPTURE (the .dspreset) and DSPRESET_ASIMOV_DIR (folder of
 * .dspreset files); tests/run.sh refuses to run without them. */
#include "test_support.h"

#include <dirent.h>

#include "../src/dsp/dspreset/wav_source.h"

#define BLOCK 128
#define GAIN 0.7f

/* Descriptors this process holds: the Move host's soft limit is 1024, shared. */
static int open_fds(void) {
    DIR *d = opendir("/dev/fd");
    int n = 0;
    CHECK(d);
    while (readdir(d)) n++;
    closedir(d);
    return n;
}

static double rms(const int16_t *x, unsigned n) {
    double s = 0;
    for (unsigned i = 0; i < n; ++i) s += (double)x[i] * x[i];
    return sqrt(s / n) / 32768.0;
}

/* Capture: note 35, velocity 100, first hit = round-robin 1, hard layer:
 * GO-TO_..._hard_DI_B0_35.wav from frame 1000 (inherited <groups start>),
 * scaled by velocity (ampVelTrack 1) -- checked frame by frame. */
static void capture(plugin_t *p, const char *preset) {
    char status[256], wav[1024], error[128];
    const char *slash = strrchr(preset, '/');
    ds_wav_source_t src;
    static float file[44100 * 3];
    int16_t out[BLOCK * 2];
    unsigned frames, blocks, worst = 0;
    plugin_load(p, preset, status, sizeof(status));
    printf("  status: %s\n", status);
    CHECK(!strcmp(status, "Capture GO-TO Bass.dspreset: 564 zones"));
    {   /* its controls, named from what they drive (the preset gives no labels) */
        char params[8192];
        plugin_get(p, "chain_params", params, sizeof(params));
        CHECK(strstr(params, "\"name\":\"EQ On\",\"type\":\"enum\",\"options\":[\"On\",\"Off\"]"));
        CHECK(strstr(params, "\"name\":\"EQ 80\"") && strstr(params, "\"name\":\"EQ 1.5k\"") && strstr(params, "\"name\":\"Cutoff\""));
    }
    printf("  open descriptors after loading 540 files: %d\n", open_fds());
    CHECK(open_fds() < 40);

    snprintf(wav, sizeof(wav), "%.*s/Samples/DI Samples/GO-TO_Bass_5string_0fret_hard_DI_B0_35.wav",
             (int)(slash - preset), preset);
    CHECK(ds_wav_source_open(&src, wav, error, sizeof(error)) == 0);
    frames = (unsigned)(src.frame_count - 1001 < 44100 * 3 ? src.frame_count - 1001 : 44100 * 3);
    CHECK(ds_wav_source_read_frames(&src, 1000, file, frames, error, sizeof(error)) == (int)frames);
    ds_wav_source_close(&src);
    CHECK(frames > 16384 + 44100);                     /* the comparison crosses into the stream */

    /* Its EQ is ON by default (every band boosted). Switch it off — the cutoff
     * rests at 22 kHz, an exact pass-through — so the output must be the file. */
    p->api->set_param(p->instance, "ctl_0", "1");
    plugin_render(p, out);
    plugin_midi(p, 0x90, 35, 100);
    blocks = frames / BLOCK;
    for (unsigned b = 0; b < blocks; ++b) {
        plugin_render(p, out);
        for (unsigned i = 0; i < BLOCK; ++i) {
            int16_t want = expected_out(file[b * BLOCK + i] * (100 / 127.0f), GAIN);
            unsigned diff = (unsigned)abs(out[2 * i] - want);
            if (diff > worst) worst = diff;
            if (diff > 1 || out[2 * i] != out[2 * i + 1]) {
                fprintf(stderr, "FAIL capture frame %u: got %d/%d want %d\n", b * BLOCK + i, out[2 * i], out[2 * i + 1], want);
                exit(1);
            }
        }
    }
    plugin_midi(p, 0x80, 35, 0);
    printf("  Capture note 35, EQ off: %u frames match the file (worst %u LSB)\n", blocks * BLOCK, worst);
    for (int i = 0; i < 100; ++i) plugin_render(p, out);
    {   /* EQ back on: the same note must come out different, and louder */
        double eq_on = 0, eq_off = 0;
        for (int pass = 0; pass < 2; ++pass) {
            p->api->set_param(p->instance, "ctl_0", pass ? "1" : "0");
            plugin_render(p, out);
            for (int hit = 0; hit < 4; ++hit) {         /* all four round robins, so both passes hear the same set */
                plugin_midi(p, 0x90, 40, 100);
                for (int b = 0; b < 60; ++b) { plugin_render(p, out); *(pass ? &eq_off : &eq_on) += rms(out, BLOCK * 2); }
                plugin_midi(p, 0x80, 40, 0);
                for (int b = 0; b < 60; ++b) plugin_render(p, out);
            }
        }
        printf("  Capture EQ on vs off: %.4f vs %.4f\n", eq_on / 240, eq_off / 240);
        CHECK(eq_on > eq_off * 1.2);
        p->api->set_param(p->instance, "ctl_0", "1");
    }

    /* round robin: the next three hits pick different files */
    for (int hit = 0; hit < 3; ++hit) {
        plugin_midi(p, 0x90, 35, 100);
        for (int i = 0; i < 40; ++i) plugin_render(p, out);
        CHECK(rms(out, BLOCK * 2) > 0.001);
        plugin_midi(p, 0x80, 35, 0);
        for (int i = 0; i < 60; ++i) plugin_render(p, out);
    }
    /* every note in the playable range sounds */
    for (int n = 33; n <= 79; n += 1) {
        double level = 0;
        plugin_midi(p, 0x90, (uint8_t)n, 90);
        for (int i = 0; i < 8; ++i) { plugin_render(p, out); level += rms(out, BLOCK * 2); }
        plugin_midi(p, 0x80, (uint8_t)n, 0);
        if (level < 0.001) { fprintf(stderr, "FAIL capture note %d silent\n", n); exit(1); }
    }
    for (int i = 0; i < 100; ++i) plugin_render(p, out);
    CHECK(plugin_uint(p, "underruns") == 0);
    printf("  open descriptors after 50 notes: %d\n", open_fds());
    CHECK(open_fds() < 40 + 64);
}

static int by_name(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

/* ASIMOV: every preset loads, and a held note still sounds 3 s in (its
 * samples sustain on the loops stored in the WAV files). */
static void asimov(plugin_t *p, const char *dir_path) {
    DIR *dir = opendir(dir_path);
    struct dirent *entry;
    char *names[64];
    int count = 0;
    CHECK(dir);
    while ((entry = readdir(dir)) != NULL && count < 64) {
        size_t len = strlen(entry->d_name);
        if (len > 9 && !strcmp(entry->d_name + len - 9, ".dspreset")) names[count++] = strdup(entry->d_name);
    }
    closedir(dir);
    CHECK(count == 15);
    qsort(names, (size_t)count, sizeof(names[0]), by_name);
    for (int k = 0; k < count; ++k) {
        char path[1024], status[256];
        int16_t out[BLOCK * 2];
        double early = 0, late = 0;
        snprintf(path, sizeof(path), "%s/%s", dir_path, names[k]);
        plugin_load(p, path, status, sizeof(status));
        CHECK(strstr(status, ": 11 zones") && !strstr(status, "missing"));
        plugin_midi(p, 0x90, 48, 100);
        for (int b = 0; b < (int)(3.0 * 44100 / BLOCK); ++b) {
            plugin_render(p, out);
            if (b >= 20 && b < 60) early += rms(out, BLOCK * 2);
            if (b >= 990) late += rms(out, BLOCK * 2);
        }
        plugin_midi(p, 0x80, 48, 0);
        printf("  %-28s early %.4f  at 3 s %.4f\n", names[k], early / 40, late / (1033 - 990));
        CHECK(early > 0.01 && late > 0.001);
        if (k == 0) {
            /* 1 - Off World: eight knobs, and the Attack knob really moves the attack */
            char params[8192];
            double slow = 0;
            const char *want[] = {"Cutoff", "Reverb", "Dly Wet", "Chr Mix", "Attack", "Decay", "Sustain", "Release"};
            plugin_get(p, "chain_params", params, sizeof(params));
            for (int i = 0; i < 8; ++i) {
                char needle[64];
                snprintf(needle, sizeof(needle), "\"key\":\"ctl_%d\",\"name\":\"%s\"", i, want[i]);
                if (!strstr(params, needle)) { fprintf(stderr, "FAIL ASIMOV control %d is not %s\n", i, want[i]); exit(1); }
            }
            for (int b = 0; b < 400 && plugin_uint(p, "voices"); ++b) plugin_render(p, out);
            /* Its Reverb (a 4.6 s room at 80%) would still be ringing from the last
             * note: take it out for these two checks. At 0 it fades out and stops. */
            p->api->set_param(p->instance, "ctl_1", "0");
            p->api->set_param(p->instance, "ctl_4", "10");          /* attack 10 s */
            plugin_render(p, out);
            plugin_midi(p, 0x90, 48, 100);
            for (int b = 20; b < 60; ++b) { plugin_render(p, out); slow += rms(out, BLOCK * 2); }
            plugin_midi(p, 0x80, 48, 0);
            printf("  Off World, Attack 0 vs 10 s over the first 0.2 s: %.4f vs %.4f\n", early / 40, slow / 40);
            CHECK(slow / 40 < early / 40 / 10);
            p->api->set_param(p->instance, "ctl_4", "0");
            for (int b = 0; b < 400 && plugin_uint(p, "voices"); ++b) plugin_render(p, out);
            {   /* and its Cutoff knob, at the bottom of its table, darkens a held note */
                double open = 0, shut = 0;
                plugin_midi(p, 0x90, 60, 100);
                for (int b = 0; b < 200; ++b) { plugin_render(p, out); if (b >= 100) open += rms(out, BLOCK * 2); }
                p->api->set_param(p->instance, "ctl_0", "1");
                for (int b = 0; b < 200; ++b) { plugin_render(p, out); if (b >= 100) shut += rms(out, BLOCK * 2); }
                plugin_midi(p, 0x80, 60, 0);
                printf("  Off World, Cutoff open vs shut: %.4f vs %.4f\n", open / 100, shut / 100);
                CHECK(shut < open / 3);
                p->api->set_param(p->instance, "ctl_0", "10");
                for (int b = 0; b < 400 && plugin_uint(p, "voices"); ++b) plugin_render(p, out);
            }
        }
        for (int b = 0; b < 400 && plugin_uint(p, "voices"); ++b) plugin_render(p, out);
        free(names[k]);
    }
    CHECK(plugin_uint(p, "underruns") == 0);
}

/* Yamaha CS-20M: a .dsbundle (a folder macOS shows as one file) of 21 presets,
 * one of them on AIFF samples. Reached the way a user reaches it — as a bank. */
static void cs20m(const char *bundle) {
    const char *tmp = getenv("TEST_TMP");
    char mod[512], cmd[2048], value[4096], status[256], name[128];
    plugin_t p;
    int16_t out[BLOCK * 2];
    double level = 0, late = 0;
    CHECK(tmp);
    snprintf(mod, sizeof(mod), "%s/cs20m-module", tmp);
    snprintf(cmd, sizeof(cmd), "rm -rf '%s' && mkdir -p '%s/instruments' && ln -s '%s' '%s/instruments/Yamaha CS-20M.dsbundle'",
             mod, mod, bundle, mod);
    CHECK(system(cmd) == 0);
    plugin_open_in(&p, mod);
    usleep(700000);
    plugin_get(&p, "bank_list", value, sizeof(value));
    CHECK(!strcmp(value, "[{\"label\":\"Yamaha CS-20M\",\"index\":0}]"));
    CHECK(plugin_uint(&p, "preset_count") == 21);
    p.api->set_param(p.instance, "preset", "2");
    plugin_get(&p, "preset_name", name, sizeof(name));
    CHECK(!strcmp(name, "03 Blue Moon"));
    for (int i = 0; i < 500; ++i) { usleep(10000); if (i > 30 && !plugin_uint(&p, "is_loading")) break; }
    plugin_get(&p, "status", status, sizeof(status));
    printf("  CS-20M: %s\n", status);
    CHECK(!strcmp(status, "03 Blue Moon.dspreset: 45 zones"));   /* AIFF, none missing */
    clock_gettime(CLOCK_MONOTONIC, &p.next);
    plugin_midi(&p, 0x90, 48, 100);
    for (int b = 0; b < 700; ++b) {
        plugin_render(&p, out);
        if (b >= 20 && b < 60) level += rms(out, BLOCK * 2) / 40;
        if (b >= 650) late += rms(out, BLOCK * 2) / 50;
    }
    plugin_midi(&p, 0x80, 48, 0);
    printf("  Blue Moon (AIFF) note 48: %.4f, at 2 s %.4f\n", level, late);
    CHECK(level > 0.01 && late > 0.001 && plugin_uint(&p, "underruns") == 0);
    for (int b = 0; b < 400 && plugin_uint(&p, "voices"); ++b) plugin_render(&p, out);
    /* 12 Buzzy Bass rests its filters at 33 Hz: only its envelope modulator opens them */
    p.api->set_param(p.instance, "preset", "11");
    plugin_get(&p, "preset_name", name, sizeof(name));
    CHECK(!strcmp(name, "12 Buzzy Bass"));
    for (int i = 0; i < 500; ++i) { usleep(10000); if (i > 30 && !plugin_uint(&p, "is_loading")) break; }
    clock_gettime(CLOCK_MONOTONIC, &p.next);
    level = 0;
    plugin_midi(&p, 0x90, 36, 110);
    for (int b = 0; b < 100; ++b) { plugin_render(&p, out); if (b >= 10) level += rms(out, BLOCK * 2) / 90; }
    plugin_midi(&p, 0x80, 36, 0);
    printf("  Buzzy Bass (filter opened by its envelope): %.4f\n", level);
    CHECK(level > 0.01);
    plugin_close(&p);
}

int main(void) {
    const char *capture_path = getenv("DSPRESET_CAPTURE");
    const char *asimov_dir = getenv("DSPRESET_ASIMOV_DIR");
    const char *cs20m_bundle = getenv("DSPRESET_CS20M_BUNDLE");
    plugin_t p;
    CHECK(capture_path && asimov_dir && cs20m_bundle);
    cs20m(cs20m_bundle);
    plugin_open(&p);
    capture(&p, capture_path);
    asimov(&p, asimov_dir);
    plugin_close(&p);
    puts("real library test passed");
    return 0;
}
