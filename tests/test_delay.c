/* <effect type="delay">: echoes at delayTime -/+ half the stereoOffset,
 * each feedback times the last, added to an untouched dry at wetLevel; a
 * fractional time read between neighbours; feedback capped below 1; time
 * glides without a jump; silence -> idle, sound wakes it; the preset decides
 * how long a line it gets; a knob reaches it; tempo-synced time stays off. */
#include "test_support.h"

#include "../src/dsp/dspreset/native_engine.h"

#define SR 44100

/* An impulse through `d`, `n` frames: interleaved output. */
static float *impulse(ds_delay_t *d, int n) {
    float *lr = calloc((size_t)n * 2, sizeof(float));
    CHECK(lr);
    lr[0] = lr[1] = 1;
    for (int at = 0; at < n; at += 128) ds_delay_process(d, lr + 2 * at, (unsigned)(n - at < 128 ? n - at : 128));
    return lr;
}

int main(void) {
    const char *dir = getenv("TEST_TMP");
    CHECK(dir);

    /* 0.1 s with offset 0.02: left at 0.09 s (3969), right at 0.11 s (4851),
     * each echo half the last; nothing else anywhere */
    {
        ds_delay_t *d = ds_delay_create(SR, 1);
        float *lr;
        ds_delay_set(d, 0.1f, 0.02f, 0.5f, 0.8f);
        lr = impulse(d, 15000);
        for (int i = 0; i < 15000; ++i) {
            float want_l = i == 0 ? 1 : i % 3969 == 0 ? 0.8f * powf(0.5f, (float)(i / 3969 - 1)) : 0;
            float want_r = i == 0 ? 1 : i % 4851 == 0 ? 0.8f * powf(0.5f, (float)(i / 4851 - 1)) : 0;
            CHECK(fabsf(lr[2 * i] - want_l) < 1e-6f && fabsf(lr[2 * i + 1] - want_r) < 1e-6f);
        }
        free(lr);
        ds_delay_destroy(d);
    }

    /* a time between samples: 100.25 samples back, split 0.75 / 0.25 */
    {
        ds_delay_t *d = ds_delay_create(SR, 1);
        float *lr;
        ds_delay_set(d, 100.25f / SR, 0, 0, 1);
        lr = impulse(d, 300);
        CHECK(fabsf(lr[200] - 0.75f) < 1e-3f && fabsf(lr[202] - 0.25f) < 1e-3f);
        for (int i = 1; i < 300; ++i) if (i != 100 && i != 101) CHECK(lr[2 * i] == 0);
        free(lr);
        ds_delay_destroy(d);
    }

    /* feedback 1 is held at 0.99: the tenth echo is 0.99^9 of the first */
    {
        ds_delay_t *d = ds_delay_create(SR, 1);
        float *lr;
        ds_delay_set(d, 0.01f, 0, 1, 1);
        lr = impulse(d, 441 * 11);
        CHECK(fabsf(lr[2 * 441 * 10] - powf(0.99f, 9)) < 1e-4f);
        free(lr);
        ds_delay_destroy(d);
    }

    /* the time knob glides: 0.2 s -> 0.05 s over 50 ms with no jump in a
     * steady sine's echo; afterwards the echo is exactly 0.05 s back */
    {
        ds_delay_t *d = ds_delay_create(SR, 1);
        float lr[2 * 128], prev = 0, worst = 0;
        long n = 0;
        ds_delay_set(d, 0.2f, 0, 0, 1);
        for (; n < SR / 2; n += 128) {
            for (int i = 0; i < 128; ++i) lr[2 * i] = lr[2 * i + 1] = (float)sin((n + i) * 0.01);
            ds_delay_process(d, lr, 128);
            prev = lr[2 * 127] - (float)sin((n + 127) * 0.01);
        }
        ds_delay_set(d, 0.05f, 0, 0, 1);
        for (long end = n + SR / 2; n < end; n += 128) {
            for (int i = 0; i < 128; ++i) lr[2 * i] = lr[2 * i + 1] = (float)sin((n + i) * 0.01);
            ds_delay_process(d, lr, 128);
            for (int i = 0; i < 128; ++i) {
                float echo = lr[2 * i] - (float)sin((n + i) * 0.01);
                if (fabsf(echo - prev) > worst) worst = fabsf(echo - prev);
                prev = echo;
            }
        }
        printf("  time 0.2 -> 0.05 s: largest step in the echo %.4f\n", worst);
        CHECK(worst < 0.06f);          /* the read runs 4x fast mid-glide: ~0.04; a jump would be ~1 */
        for (int i = 0; i < 128; ++i)
            CHECK(fabsf(lr[2 * i] - (float)(sin((n - 128 + i) * 0.01) + sin((n - 128 + i - 2205) * 0.01))) < 1e-3f);
        ds_delay_destroy(d);
    }

    /* silence: idle once the line has emptied; sound wakes it at once */
    {
        ds_delay_t *d = ds_delay_create(SR, 0.1f);
        float lr[2 * 128] = {0};
        int blocks = 0;
        ds_delay_set(d, 0.05f, 0, 0.9f, 1);
        lr[0] = 1;
        ds_delay_process(d, lr, 128);
        CHECK(!ds_delay_idle(d));
        while (!ds_delay_idle(d) && blocks < 10000) { memset(lr, 0, sizeof(lr)); ds_delay_process(d, lr, 128); ++blocks; }
        printf("  feedback 0.9 at 50 ms: idle after %.2f s\n", blocks * 128.0 / SR);
        CHECK(ds_delay_idle(d) && blocks * 128 > SR * 0.05 * 150);   /* 0.9^n reaches -180 dB at n ~ 197 */
        memset(lr, 0, sizeof(lr));
        lr[0] = 0.5f;
        ds_delay_process(d, lr, 128);
        CHECK(!ds_delay_idle(d) && lr[0] == 0.5f);
        ds_delay_destroy(d);
    }

    /* through the engine */
    {
        static ds_native_engine_t dry, wet;
        char path[512], error[128];
        float a[256], b[256];
        double diff = 0;
        snprintf(path, sizeof(path), "mkdir -p '%s/dl'", dir); CHECK(system(path) == 0);
        snprintf(path, sizeof(path), "%s/dl/t.wav", dir); write_sine24(path, 30000, 440, 0.5);
        snprintf(path, sizeof(path), "%s/dl/dry.dspreset", dir);
        write_text(path, "<DecentSampler><groups attack=\"0\"><group><sample path=\"t.wav\" rootNote=\"60\"/></group></groups></DecentSampler>");
        CHECK(ds_native_engine_load(&dry, path, SR, NULL, NULL, error, sizeof(error)) == 0);

        /* the preset's own times decide the line: 0.7 +/- 0.1 s -> 0.8 s */
        snprintf(path, sizeof(path), "%s/dl/wet.dspreset", dir);
        write_text(path, "<DecentSampler><ui><tab><labeled-knob label=\"Delay\" minValue=\"0\" maxValue=\"1\" value=\"0\">"
                         "<binding type=\"effect\" level=\"instrument\" position=\"0\" parameter=\"FX_WET_LEVEL\"/>"
                         "</labeled-knob></tab></ui>"
                         "<groups attack=\"0\"><group><sample path=\"t.wav\" rootNote=\"60\"/></group></groups>"
                         "<effects><effect type=\"delay\" delayTime=\"0.7\" stereoOffset=\"0.2\" feedback=\"0.5\" wetLevel=\"0.0\"/></effects></DecentSampler>");
        CHECK(ds_native_engine_load(&wet, path, SR, NULL, NULL, error, sizeof(error)) == 0);
        CHECK(wet.delay[0] && fabsf(ds_delay_longest(wet.delay[0]) - 0.8f) < 1e-3f);
        ds_native_engine_note_on(&dry, 60, 127); ds_native_engine_note_on(&wet, 60, 127);
        for (int blk = 0; blk < 20; ++blk) {                   /* wet 0: exactly dry */
            memset(a, 0, sizeof(a)); memset(b, 0, sizeof(b));
            ds_native_engine_render(&dry, a, 128); ds_native_engine_render(&wet, b, 128);
            for (int i = 0; i < 256; ++i) CHECK(a[i] == b[i]);
        }
        ds_native_engine_set_control(&wet, 0, 0.7f);
        {   /* the note's first echo: left at 0.6 s, right at 0.8 s, not a moment before */
            long first[2] = {-1, -1};
            for (int blk = 20; blk < 420; ++blk) {
                memset(a, 0, sizeof(a)); memset(b, 0, sizeof(b));
                ds_native_engine_render(&dry, a, 128); ds_native_engine_render(&wet, b, 128);
                for (int i = 0; i < 256; ++i) {
                    diff += (double)(b[i] - a[i]) * (b[i] - a[i]);
                    if (b[i] != a[i] && first[i & 1] < 0) first[i & 1] = blk * 128L + i / 2;
                }
            }
            printf("  preset delay 0.7 s, offset 0.2: first echo left %ld, right %ld (want %d, %d)\n",
                   first[0], first[1], SR * 6 / 10, SR * 8 / 10);
            /* (+1: the note's first sample can be a zero crossing, with a silent echo) */
            CHECK(first[0] - SR * 6 / 10 <= 1 && first[0] >= SR * 6 / 10);
            CHECK(first[1] - SR * 8 / 10 <= 1 && first[1] >= SR * 8 / 10);
        }
        printf("  preset Delay knob 0 -> 0.7: echoes add %.4f rms\n", sqrt(diff / (400 * 256)));
        CHECK(sqrt(diff / (400 * 256)) > 0.02);
        ds_native_engine_destroy(&wet);

        /* a knob on the TIME gets the whole range: 20 s + half of 10 s */
        write_text(path, "<DecentSampler><ui><tab><labeled-knob minValue=\"0\" maxValue=\"1\" value=\"0.5\">"
                         "<binding type=\"effect\" level=\"instrument\" position=\"0\" parameter=\"FX_DELAY_TIME\"/>"
                         "</labeled-knob></tab></ui>"
                         "<groups attack=\"0\"><group><sample path=\"t.wav\" rootNote=\"60\"/></group></groups>"
                         "<effects><effect type=\"delay\" delayTime=\"0.5\"/></effects></DecentSampler>");
        CHECK(ds_native_engine_load(&wet, path, SR, NULL, NULL, error, sizeof(error)) == 0);
        CHECK(wet.delay[0] && fabsf(ds_delay_longest(wet.delay[0]) - 25) < 1e-3f);
        ds_native_engine_destroy(&wet);

        /* tempo-synced time: not rendered (no documented mapping), so dry */
        write_text(path, "<DecentSampler><groups attack=\"0\"><group><sample path=\"t.wav\" rootNote=\"60\"/></group></groups>"
                         "<effects><effect type=\"delay\" delayTimeFormat=\"musical_time\" delayTime=\"10\" wetLevel=\"0.5\"/></effects></DecentSampler>");
        CHECK(ds_native_engine_load(&wet, path, SR, NULL, NULL, error, sizeof(error)) == 0);
        CHECK(wet.fx_coeffs[0].kind == DS_FX_UNSUPPORTED);
        ds_native_engine_destroy(&dry); ds_native_engine_destroy(&wet);
    }
    puts("delay test passed");
    return 0;
}
