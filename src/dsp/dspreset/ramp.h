#ifndef DSPRESET_RAMP_H
#define DSPRESET_RAMP_H

/* A setting that glides linearly to its target over a fixed number of
 * samples (as juce::SmoothedValue does), so a knob never clicks. */

typedef struct { float now, target, step; int left; } ramp_t;

static inline void ramp_to(ramp_t *r, float target, int steps, int snap) {
    if (target == r->target && !snap) return;
    r->target = target;
    if (snap || steps <= 0) { r->now = target; r->left = 0; return; }
    r->left = steps;
    r->step = (target - r->now) / (float)steps;
}

static inline float ramp_next(ramp_t *r) {
    if (r->left <= 0) return r->target;
    if (--r->left > 0) r->now += r->step; else r->now = r->target;
    return r->now;
}

/* Jump to the target now (used while an effect is skipping its work). */
static inline void ramp_land(ramp_t *r) { ramp_to(r, r->target, 0, 1); }

#endif
