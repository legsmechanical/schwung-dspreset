/* <effect type="chorus">: the sweep (a sine shared by both channels moving a
 * linearly interpolated delay of max(1, 7 + 10 * depth * lfo) ms) held to a
 * plain formula sample by sample; the linear mix; the 50 ms fade to dry at
 * mix 0; that it stops work in silence and wakes for sound; and that a
 * preset's knob reaches it. */
#include "test_support.h"

#include "../src/dsp/dspreset/native_engine.h"

#define SR 44100

static float signal_at(long n) { return n < 0 ? 0.0f : (float)(0.5 * sin(n * 0.071) + 0.3 * sin(n * 0.0137)); }

/* The chorus as a formula, in double: the wet is the input `delay` samples
 * back, read between its two neighbours. */
static double ref_out(long n, double mix, double depth, double rate) {
    double phase = fmod(2 * M_PI * rate * n / SR, 2 * M_PI);
    double ms = 7 + 10 * depth * -sin(phase), delay, frac;
    long whole;
    if (ms < 1) ms = 1;
    delay = ms * SR / 1000;
    whole = (long)delay;
    frac = delay - whole;
    return signal_at(n) * (1 - mix) + (signal_at(n - whole) * (1 - frac) + signal_at(n - whole - 1) * frac) * mix;
}

int main(void) {
    const char *dir = getenv("TEST_TMP");
    CHECK(dir);

    /* one tap, sample exact: an impulse through depth 0 lands 7 ms later,
     * 308.7 samples: 0.3 of it at 308, 0.7 at 309 */
    {
        ds_chorus_t *c = ds_chorus_create(SR);
        float lr[2 * 512] = {0};
        ds_chorus_set(c, 1, 0, 0.2f);
        lr[0] = lr[1] = 1;
        ds_chorus_process(c, lr, 512);
        for (int i = 0; i < 512; ++i) {
            float want = i == 308 ? 0.3f : i == 309 ? 0.7f : 0.0f;
            CHECK(fabsf(lr[2 * i] - want) < 1e-4f && lr[2 * i] == lr[2 * i + 1]);
        }
        ds_chorus_destroy(c);
    }

    /* the sweep against the formula, at depths that do and do not reach the
     * 1 ms floor, over two seconds */
    {
        static const float settings[][3] = {{0.5f, 0.2f, 0.2f}, {1.0f, 0.9f, 2.0f}, {0.3f, 0.6f, 7.5f}};
        for (unsigned s = 0; s < 3; ++s) {
            ds_chorus_t *c = ds_chorus_create(SR);
            float lr[2 * 128];
            double worst = 0;
            ds_chorus_set(c, settings[s][0], settings[s][1], settings[s][2]);
            for (long start = 0; start < 2 * SR; start += 128) {
                for (int i = 0; i < 128; ++i) lr[2 * i] = lr[2 * i + 1] = signal_at(start + i);
                ds_chorus_process(c, lr, 128);
                for (int i = 0; i < 128; ++i) {
                    double e = fabs(lr[2 * i] - ref_out(start + i, settings[s][0], settings[s][1], settings[s][2]));
                    if (e > worst) worst = e;
                    CHECK(lr[2 * i] == lr[2 * i + 1]);      /* one sweep for both channels */
                }
            }
            printf("  mix %.1f depth %.1f rate %.1f Hz: worst %.2e from the formula\n",
                   settings[s][0], settings[s][1], settings[s][2], worst);
            CHECK(worst < 2e-5);
            ds_chorus_destroy(c);
        }
    }

    /* mix 1 -> 0: gone within 50 ms, then exactly dry; silence -> idle, sound wakes it */
    {
        ds_chorus_t *c = ds_chorus_create(SR);
        float lr[2 * 128];
        long last_wet = -1;
        ds_chorus_set(c, 1, 0.5f, 1);
        for (long start = 0; start < SR / 4; start += 128) {
            for (int i = 0; i < 128; ++i) lr[2 * i] = lr[2 * i + 1] = signal_at(start + i);
            ds_chorus_process(c, lr, 128);
        }
        ds_chorus_set(c, 0, 0.5f, 1);
        for (long start = 0; start < SR / 4; start += 128) {
            for (int i = 0; i < 128; ++i) lr[2 * i] = lr[2 * i + 1] = signal_at(start + i);
            ds_chorus_process(c, lr, 128);
            for (int i = 0; i < 128; ++i) if (lr[2 * i] != signal_at(start + i)) last_wet = start + i;
        }
        printf("  mix to 0: last touched sample %ld (50 ms = %d)\n", last_wet, SR / 20);
        CHECK(last_wet >= SR / 20 - 10 && last_wet < SR / 20 + 128);
        ds_chorus_set(c, 0.5f, 0.5f, 1);
        memset(lr, 0, sizeof(lr));
        for (int blk = 0; blk < 30; ++blk) ds_chorus_process(c, lr, 128);
        CHECK(ds_chorus_idle(c));
        for (int i = 0; i < 256; ++i) CHECK(lr[i] == 0);
        lr[0] = 0.5f;
        ds_chorus_process(c, lr, 128);
        CHECK(!ds_chorus_idle(c) && fabsf(lr[0] - 0.25f) < 1e-6f);   /* the dry half, at once */
        memset(lr, 0, sizeof(lr));
        for (int blk = 0; blk < 30; ++blk) ds_chorus_process(c, lr, 128);   /* and idle again once it has passed */
        CHECK(ds_chorus_idle(c));
        ds_chorus_destroy(c);
    }

    /* through the engine: knob at 0 is exactly dry; up, it changes the sound */
    {
        static ds_native_engine_t dry, wet;
        char path[512], error[128];
        float a[256], b[256];
        double diff = 0;
        snprintf(path, sizeof(path), "mkdir -p '%s/ch'", dir); CHECK(system(path) == 0);
        snprintf(path, sizeof(path), "%s/ch/t.wav", dir); write_sine24(path, 30000, 440, 0.5);
        snprintf(path, sizeof(path), "%s/ch/dry.dspreset", dir);
        write_text(path, "<DecentSampler><groups attack=\"0\"><group><sample path=\"t.wav\" rootNote=\"60\"/></group></groups></DecentSampler>");
        CHECK(ds_native_engine_load(&dry, path, SR, NULL, NULL, error, sizeof(error)) == 0);
        snprintf(path, sizeof(path), "%s/ch/wet.dspreset", dir);
        write_text(path, "<DecentSampler><ui><tab><labeled-knob label=\"Chorus\" minValue=\"0\" maxValue=\"1\" value=\"0\">"
                         "<binding type=\"effect\" level=\"instrument\" position=\"0\" parameter=\"FX_MIX\"/>"
                         "</labeled-knob></tab></ui>"
                         "<groups attack=\"0\"><group><sample path=\"t.wav\" rootNote=\"60\"/></group></groups>"
                         "<effects><effect type=\"chorus\" mix=\"0\" modDepth=\"0.7\" modRate=\"0.25\"/></effects></DecentSampler>");
        CHECK(ds_native_engine_load(&wet, path, SR, NULL, NULL, error, sizeof(error)) == 0);
        CHECK(wet.chorus[0]);
        ds_native_engine_note_on(&dry, 60, 127); ds_native_engine_note_on(&wet, 60, 127);
        for (int blk = 0; blk < 20; ++blk) {
            memset(a, 0, sizeof(a)); memset(b, 0, sizeof(b));
            ds_native_engine_render(&dry, a, 128); ds_native_engine_render(&wet, b, 128);
            for (int i = 0; i < 256; ++i) CHECK(a[i] == b[i]);
        }
        ds_native_engine_set_control(&wet, 0, 0.6f);
        for (int blk = 0; blk < 60; ++blk) {
            memset(a, 0, sizeof(a)); memset(b, 0, sizeof(b));
            ds_native_engine_render(&dry, a, 128); ds_native_engine_render(&wet, b, 128);
            for (int i = 0; i < 256; ++i) diff += (double)(b[i] - a[i]) * (b[i] - a[i]);
        }
        printf("  preset Chorus knob 0 -> 0.6: changes %.4f rms\n", sqrt(diff / (60 * 256)));
        CHECK(sqrt(diff / (60 * 256)) > 0.02);
        ds_native_engine_destroy(&dry); ds_native_engine_destroy(&wet);
    }
    puts("chorus test passed");
    return 0;
}
