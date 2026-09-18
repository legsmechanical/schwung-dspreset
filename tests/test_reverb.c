/* The plate behind <effect type="reverb">: its calibration against JUCE's
 * Freeverb (tail length per roomSize), that it only ADDS (dry untouched, wet 0
 * silent), that it is stereo, that the tail dies away, and that a preset's
 * reverb knob reaches it. */
#include "test_support.h"

#include "../src/dsp/dspreset/native_engine.h"

#define SR 44100

/* RT60 (T30 x2) of the left channel of an impulse response. */
static double rt60(const float *lr, int n) {
    double total = 0, acc, t5 = -1, t35 = -1;
    for (int i = 0; i < n; ++i) total += (double)lr[2 * i] * lr[2 * i];
    acc = total;
    for (int i = 0; i < n; ++i) {
        double db = 10 * log10(acc / total + 1e-30);
        if (t5 < 0 && db <= -5) t5 = i;
        if (db <= -35) { t35 = i; break; }
        acc -= (double)lr[2 * i] * lr[2 * i];
    }
    return t35 < 0 ? -1 : 2.0 * (t35 - t5) / SR;
}

static float *impulse(float room, float damping, int n) {
    float *ir = calloc((size_t)n * 2, sizeof(float));
    ds_reverb_t *r = ds_reverb_create(SR);
    CHECK(ir && r);
    ds_reverb_set(r, room, damping, 1.0f);
    for (int b = 0; b + 128 <= n; b += 128) {
        if (b == 0) ir[0] = ir[1] = 1.0f;
        ds_reverb_process(r, ir + 2 * b, 128);
    }
    ir[0] -= 1.0f; ir[1] -= 1.0f;                      /* the dry impulse it left in place */
    ds_reverb_destroy(r);
    return ir;
}

int main(void) {
    const char *dir = getenv("TEST_TMP");
    const int n = SR * 10;
    float *ir;
    double corr = 0, el = 0, er = 0, tail = 0;
    CHECK(dir);

    /* tail length tracks Freeverb's per roomSize (tools/reverb_calibrate.c) */
    {
        const float rooms[3] = {0.3f, 0.7f, 0.9f};
        const double want[3] = {0.93, 2.06, 4.59};
        for (int k = 0; k < 3; ++k) {
            double got;
            ir = impulse(rooms[k], 0, n);
            got = rt60(ir, n);
            printf("  roomSize %.1f: RT60 %.2f s (Freeverb %.2f)\n", rooms[k], got, want[k]);
            CHECK(fabs(got - want[k]) < 0.08 * want[k]);
            free(ir);
        }
    }

    /* stereo, and a tail that dies away with nothing odd in it */
    ir = impulse(0.9f, 0.6f, n);
    for (int i = 0; i < n; ++i) {
        CHECK(isfinite(ir[2 * i]) && isfinite(ir[2 * i + 1]));
        corr += (double)ir[2 * i] * ir[2 * i + 1]; el += (double)ir[2 * i] * ir[2 * i]; er += (double)ir[2 * i + 1] * ir[2 * i + 1];
    }
    for (int i = n - SR / 2; i < n; ++i) tail += (double)ir[2 * i] * ir[2 * i];
    printf("  left/right correlation %.3f; last 0.5 s at %.1f dB of the whole\n", corr / sqrt(el * er), 10 * log10(tail / el));
    CHECK(fabs(corr / sqrt(el * er)) < 0.3);
    CHECK(10 * log10(tail / el) < -60);
    free(ir);

    /* through the engine: a preset reverb only ADDS; wet 0 is exactly dry; its knob reaches it */
    {
        static ds_native_engine_t dry, wet;
        char path[512], error[128];
        float a[256], b[256];
        double diff = 0;
        snprintf(path, sizeof(path), "mkdir -p '%s/rv'", dir); CHECK(system(path) == 0);
        snprintf(path, sizeof(path), "%s/rv/t.wav", dir); write_sine24(path, 30000, 440, 0.5);
        snprintf(path, sizeof(path), "%s/rv/dry.dspreset", dir);
        write_text(path, "<DecentSampler><groups attack=\"0\"><group><sample path=\"t.wav\" rootNote=\"60\"/></group></groups></DecentSampler>");
        CHECK(ds_native_engine_load(&dry, path, SR, NULL, NULL, error, sizeof(error)) == 0);
        snprintf(path, sizeof(path), "%s/rv/wet.dspreset", dir);
        write_text(path, "<DecentSampler><ui><tab><labeled-knob minValue=\"0\" maxValue=\"100\" value=\"0\">"
                         "<binding type=\"effect\" level=\"instrument\" position=\"0\" parameter=\"FX_REVERB_WET_LEVEL\" factor=\"0.01\"/>"
                         "</labeled-knob></tab></ui>"
                         "<groups attack=\"0\"><group><sample path=\"t.wav\" rootNote=\"60\"/></group></groups>"
                         "<effects><effect type=\"reverb\" roomSize=\"0.9\" damping=\"0.6\"/></effects></DecentSampler>");
        CHECK(ds_native_engine_load(&wet, path, SR, NULL, NULL, error, sizeof(error)) == 0);
        CHECK(!strcmp(wet.model.controls[0].name, "Reverb") && wet.reverb[0]);
        ds_native_engine_note_on(&dry, 60, 127); ds_native_engine_note_on(&wet, 60, 127);
        for (int blk = 0; blk < 20; ++blk) {                   /* knob at 0: identical to no reverb */
            memset(a, 0, sizeof(a)); memset(b, 0, sizeof(b));
            ds_native_engine_render(&dry, a, 128); ds_native_engine_render(&wet, b, 128);
            for (int i = 0; i < 256; ++i) CHECK(a[i] == b[i]);
        }
        ds_native_engine_set_control(&wet, 0, 60);             /* Reverb up: dry + a tail */
        for (int blk = 0; blk < 60; ++blk) {
            memset(a, 0, sizeof(a)); memset(b, 0, sizeof(b));
            ds_native_engine_render(&dry, a, 128); ds_native_engine_render(&wet, b, 128);
            for (int i = 0; i < 256; ++i) diff += (double)(b[i] - a[i]) * (b[i] - a[i]);
        }
        printf("  preset Reverb knob 0 -> 60: wet adds %.4f rms\n", sqrt(diff / (60 * 256)));
        CHECK(sqrt(diff / (60 * 256)) > 0.02);
        ds_native_engine_destroy(&dry); ds_native_engine_destroy(&wet);
    }
    puts("reverb test passed");
    return 0;
}
