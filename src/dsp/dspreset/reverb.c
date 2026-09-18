#include "reverb.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Dattorro's delay lengths are given at 29761 Hz; they are scaled to the
 * running rate. Excursion is the modulated all-passes' swing, in samples. */
#define DATTORRO_RATE 29761.0f
#define EXCURSION 16.0f
#define LFO_HZ 0.9f

/* Wet level: JUCE's Freeverb gets louder than the plate as the room grows
 * (its comb energy rises with RT). Measured ratio (tools/reverb_calibrate.c
 * level) 1.75..3.0 over roomSize 0.1..0.9 x damping 0..0.9; scaling by
 * 1.85 + 0.5*roomSize keeps every point within +2.3/-0.7 dB of Freeverb. */
#define WET_BASE 1.85f
#define WET_PER_ROOM 0.5f

typedef struct { float *buf; unsigned mask, pos; } line_t;

enum {
    IN1, IN2, IN3, IN4,                 /* input diffusers */
    L_AP1, L_D1, L_AP2, L_D2,           /* tank, left half */
    R_AP1, R_D1, R_AP2, R_D2,           /* tank, right half */
    LINES
};

/* Dattorro's lengths (at 29761 Hz). */
static const float LENGTH[LINES] = {142, 107, 379, 277, 672, 4453, 1800, 3720, 908, 4217, 2656, 3163};

static float clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

struct ds_reverb {
    line_t line[LINES];
    float len[LINES];                   /* scaled lengths */
    float scale;                        /* rate / 29761 */
    float rate, phase, bandwidth_z, damp_l, damp_r;
    float decay, damping, wet;          /* targets */
    float decay_now, damping_now, wet_now;   /* smoothed per block */
};

static int line_init(line_t *l, float length) {
    unsigned size = 1;
    while (size < (unsigned)length + 4) size <<= 1;
    l->buf = calloc(size, sizeof(float));
    l->mask = size - 1;
    l->pos = 0;
    return l->buf ? 0 : -1;
}

static inline void line_write(line_t *l, float v) { l->buf[l->pos] = v; l->pos = (l->pos + 1) & l->mask; }
/* The value written `d` samples ago (d >= 1). */
static inline float line_read(const line_t *l, unsigned d) { return l->buf[(l->pos - d) & l->mask]; }
static inline float line_read_frac(const line_t *l, float d) {
    unsigned i = (unsigned)d;
    float f = d - (float)i, a = line_read(l, i), b = line_read(l, i + 1);
    return a + (b - a) * f;
}

/* Schroeder all-pass on a line of length d: s = x + g*z, y = z - g*s. */
static inline float allpass(line_t *l, float d, float g, float x) {
    float z = line_read(l, (unsigned)d), s = x + g * z;
    line_write(l, s);
    return z - g * s;
}

static inline float allpass_mod(line_t *l, float d, float g, float x) {
    float z = line_read_frac(l, d), s = x + g * z;
    line_write(l, s);
    return z - g * s;
}

static inline float delay(line_t *l, float d, float x) {
    float z = line_read(l, (unsigned)d);
    line_write(l, x);
    return z;
}

/* A tap into a delay line, `at` samples (Dattorro's units) behind its input. */
static inline float tap(const ds_reverb_t *r, int which, float at) {
    return line_read(&r->line[which], (unsigned)(at * r->scale) + 1);
}

ds_reverb_t *ds_reverb_create(float sample_rate) {
    ds_reverb_t *r = calloc(1, sizeof(*r));
    if (!r) return NULL;
    r->rate = sample_rate;
    r->scale = sample_rate / DATTORRO_RATE;
    for (int i = 0; i < LINES; ++i) {
        r->len[i] = LENGTH[i] * r->scale;
        if (line_init(&r->line[i], r->len[i] + EXCURSION * r->scale + 2)) { ds_reverb_destroy(r); return NULL; }
    }
    ds_reverb_set(r, 0.7f, 0.3f, 0.0f);
    r->decay_now = r->decay; r->damping_now = r->damping; r->wet_now = r->wet;
    return r;
}

void ds_reverb_destroy(ds_reverb_t *r) {
    if (!r) return;
    for (int i = 0; i < LINES; ++i) free(r->line[i].buf);
    free(r);
}

void ds_reverb_clear(ds_reverb_t *r) {
    for (int i = 0; i < LINES; ++i) memset(r->line[i].buf, 0, (r->line[i].mask + 1) * sizeof(float));
    r->bandwidth_z = r->damp_l = r->damp_r = 0;
}

/* Tank decay per roomSize 0.0, 0.1 .. 1.0, fitted so the plate's RT60 (no
 * damping) equals JUCE Freeverb's at the same roomSize: 0.64 s at 0, 2.06 s at
 * 0.7, 4.59 s at 0.9, 11.2 s at 1.0 (tools/reverb_calibrate.c fit). */
static const float DECAY_FOR_ROOM[11] = {0.0887f, 0.1383f, 0.1878f, 0.2434f, 0.3005f, 0.3705f,
                                         0.4594f, 0.5527f, 0.6615f, 0.7793f, 0.9057f};

float ds_reverb_decay_for_room(float room) {
    float x = clamp01(room) * 10.0f;
    int i = (int)x;
    if (i >= 10) return DECAY_FOR_ROOM[10];
    return DECAY_FOR_ROOM[i] + (DECAY_FOR_ROOM[i + 1] - DECAY_FOR_ROOM[i]) * (x - (float)i);
}

/* Tank damping per DecentSampler damping 0.0, 0.1 .. 1.0, fitted so how much
 * faster 4 kHz dies than 300 Hz (RT60 ratio) matches Freeverb's, at roomSize
 * 0.7. At 0.1 the plate's own high loss already matches: no extra damping. */
static const float DAMPING_FOR[11] = {0.0f, 0.0f, 0.2042f, 0.3660f, 0.4576f, 0.5239f,
                                      0.5744f, 0.6122f, 0.6467f, 0.6817f, 0.7209f};

float ds_reverb_damping_for(float damping) {
    float x = clamp01(damping) * 10.0f;
    int i = (int)x;
    if (i >= 10) return DAMPING_FOR[10];
    return DAMPING_FOR[i] + (DAMPING_FOR[i + 1] - DAMPING_FOR[i]) * (x - (float)i);
}

void ds_reverb_set_raw(ds_reverb_t *r, float decay, float damping, float wet) {
    r->decay = decay; r->damping = damping; r->wet = wet;
}

void ds_reverb_set(ds_reverb_t *r, float room_size, float damping, float wet_level) {
    r->decay = ds_reverb_decay_for_room(room_size);
    r->damping = ds_reverb_damping_for(damping);
    r->wet = clamp01(wet_level) * (WET_BASE + WET_PER_ROOM * clamp01(room_size));
}

void ds_reverb_process(ds_reverb_t *r, float *lr, unsigned frames) {
    const float bandwidth = 0.9995f, in_diff1 = 0.75f, in_diff2 = 0.625f, decay_diff1 = 0.70f;
    /* The 0.9 Hz wobble is worked out at the block's two ends and blended
     * across it (2.9 ms): per-sample sinf/cosf made the plate slower than
     * Freeverb on the Move (9.4 vs 5.8 us a block). */
    float phase_end = r->phase + 2.0f * (float)M_PI * LFO_HZ * (float)frames / r->rate;
    float s0 = sinf(r->phase), c0 = cosf(r->phase), s1 = sinf(phase_end), c1 = cosf(phase_end);
    float ds_ = (s1 - s0) / frames, dc = (c1 - c0) / frames;
    /* settings glide over one block: a knob never clicks */
    float d0 = r->decay_now, dm0 = r->damping_now, w0 = r->wet_now;
    float dd = (r->decay - d0) / frames, ddm = (r->damping - dm0) / frames, dw = (r->wet - w0) / frames;
    float exc = EXCURSION * r->scale;
    for (unsigned i = 0; i < frames; ++i) {
        float decay = d0 + dd * (float)(i + 1), damping = dm0 + ddm * (float)(i + 1), wet = w0 + dw * (float)(i + 1);
        float decay_diff2 = decay + 0.15f < 0.25f ? 0.25f : decay + 0.15f > 0.5f ? 0.5f : decay + 0.15f;
        float x = 0.5f * (lr[2 * i] + lr[2 * i + 1]), l_in, r_in, a, yl, yr;
        float mod_l = exc * (1.0f + s0 + ds_ * (float)i) * 0.5f, mod_r = exc * (1.0f + c0 + dc * (float)i) * 0.5f;

        /* input: band limit, then four diffusers */
        r->bandwidth_z += bandwidth * (x - r->bandwidth_z);
        x = allpass(&r->line[IN1], r->len[IN1], in_diff1, r->bandwidth_z);
        x = allpass(&r->line[IN2], r->len[IN2], in_diff1, x);
        x = allpass(&r->line[IN3], r->len[IN3], in_diff2, x);
        x = allpass(&r->line[IN4], r->len[IN4], in_diff2, x);

        /* the figure eight: each half fed by the other's end */
        l_in = x + decay * line_read(&r->line[R_D2], (unsigned)r->len[R_D2]);
        r_in = x + decay * line_read(&r->line[L_D2], (unsigned)r->len[L_D2]);

        a = allpass_mod(&r->line[L_AP1], r->len[L_AP1] + mod_l, -decay_diff1, l_in);
        a = delay(&r->line[L_D1], r->len[L_D1], a);
        r->damp_l += (1.0f - damping) * (a - r->damp_l);
        a = allpass(&r->line[L_AP2], r->len[L_AP2], decay_diff2, r->damp_l * decay);
        line_write(&r->line[L_D2], a);

        a = allpass_mod(&r->line[R_AP1], r->len[R_AP1] + mod_r, -decay_diff1, r_in);
        a = delay(&r->line[R_D1], r->len[R_D1], a);
        r->damp_r += (1.0f - damping) * (a - r->damp_r);
        a = allpass(&r->line[R_AP2], r->len[R_AP2], decay_diff2, r->damp_r * decay);
        line_write(&r->line[R_D2], a);

        /* Dattorro's output taps */
        yl = tap(r, R_D1, 266) + tap(r, R_D1, 2974) - tap(r, R_AP2, 1913) + tap(r, R_D2, 1996)
           - tap(r, L_D1, 1990) - tap(r, L_AP2, 187) - tap(r, L_D2, 1066);
        yr = tap(r, L_D1, 353) + tap(r, L_D1, 3627) - tap(r, L_AP2, 1228) + tap(r, L_D2, 2673)
           - tap(r, R_D1, 2111) - tap(r, R_AP2, 335) - tap(r, R_D2, 121);
        lr[2 * i] += 0.6f * yl * wet;
        lr[2 * i + 1] += 0.6f * yr * wet;
    }
    r->phase = phase_end - (phase_end > 2.0f * (float)M_PI ? 2.0f * (float)M_PI : 0.0f);
    r->decay_now = r->decay; r->damping_now = r->damping; r->wet_now = r->wet;
    /* no denormals in a tail that decays forever */
    if (fabsf(r->damp_l) < 1e-20f) r->damp_l = 0;
    if (fabsf(r->damp_r) < 1e-20f) r->damp_r = 0;
}
