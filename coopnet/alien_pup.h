/* What the joiner's alien does each frame to stay with the host's (pure code; tested in test/sync_test.c).
 *
 * The joiner's alien doesn't think for itself (its behaviour tree isn't run, alien.c): each frame it is given the
 * game's own move-in-direction order towards where the host's alien is, at the host's alien's speed, so it walks
 * and runs with its own animations. Close enough, it stops and turns to the host's alien's facing. Too far off (it
 * fell behind, a door, the host's alien jumped) or in the vents, it is put there instead.
 */
#pragma once
#include <math.h>

#define PUP_SNAP_M   3.0f     /* further than this (flat), or PUP_SNAP_DY up or down: put it there */
#define PUP_SNAP_DY  1.8f
#define PUP_GO_M     0.35f    /* start moving when further than this */
#define PUP_ARRIVE_M 0.20f    /* and stop once nearer than this */
#define PUP_HURRY_M  1.5f     /* behind by more than this: one speed up */
#define PUP_TURN     0.15f    /* standing: turn when the facing is off by more than this (radians) */

enum { PUP_STOP = 0, PUP_STEER = 1, PUP_TELEPORT = 2 };

typedef struct { int steering; } PupState;
typedef struct { int act; float heading; int speed; int set_yaw; float yaw; } PupStep;

static inline float pup_wrap(float a) {
    while (a > 3.14159265f) a -= 6.2831853f;
    while (a <= -3.14159265f) a += 6.2831853f;
    return a;
}

static inline PupStep pup_decide(PupState *s, const float me[3], float my_yaw, const float want[3], float want_yaw,
                                 int want_speed, int host_vents) {
    PupStep r = {PUP_STOP, 0.f, 0, 0, my_yaw};
    float dx = want[0] - me[0], dy = want[1] - me[1], dz = want[2] - me[2];
    float d = sqrtf(dx * dx + dz * dz);
    if (host_vents) {   /* the vents are the game's abstract map: no walking there, follow the point */
        s->steering = 0;
        if (d > PUP_ARRIVE_M || fabsf(dy) > PUP_ARRIVE_M) r.act = PUP_TELEPORT;
        return r;
    }
    if (d > PUP_SNAP_M || fabsf(dy) > PUP_SNAP_DY) { s->steering = 0; r.act = PUP_TELEPORT; return r; }
    if (d > PUP_GO_M || (s->steering && d > PUP_ARRIVE_M)) {
        s->steering = 1;
        r.act = PUP_STEER;
        r.heading = atan2f(dx, dz);
        r.speed = want_speed + (d > PUP_HURRY_M ? 1 : 0);
        if (r.speed > 3) r.speed = 3;
        if (r.speed < 0) r.speed = 0;
        return r;
    }
    s->steering = 0;
    float off = pup_wrap(want_yaw - my_yaw);
    if (fabsf(off) > PUP_TURN) { r.set_yaw = 1; r.yaw = pup_wrap(my_yaw + off * 0.25f); }
    return r;
}

/* The partner running near the host's alien makes noise it can hear: at most once a second, when faster than this */
#define PUP_NOISE_MPS 2.6f
#define PUP_NOISE_MS  1000u
