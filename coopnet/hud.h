/* Co-op horror HUD math: the shared motion tracker. Pure functions, shared by the DLL and test/hud_test.c.
 *
 * The tracker works like the one in the game: it shows movement, not people. Contacts are characters from the
 * engine's character table (coopnet.c) that moved in the last second, plus your partner (always shown, green).
 * It turns with your view (yaw only), forward is up.
 */
#pragma once
#include <math.h>
#include <string.h>
#include "view.h"

#define TRACK_RANGE   25.f    /* metres to the tracker's edge */
#define TRACK_MOVE    0.25f   /* metres in TRACK_WINDOW_MS that count as moving */
#define TRACK_WINDOW_MS 1000u
#define TRACK_SLOTS   64

/* Flat (yaw only) forward of a view quaternion; identity looks along +Z. */
static inline void hud_forward(const float q[4], float f[2]) {
    float c[4] = {-q[0], -q[1], -q[2], q[3]}, z[3] = {0, 0, 1}, o[3];
    view_rot_inv(c, z, o);   /* rotate by q = inverse-rotate by conj(q) */
    float n = sqrtf(o[0] * o[0] + o[2] * o[2]);
    if (n < 1e-4f) { f[0] = 0; f[1] = 1; return; }   /* looking straight up or down: keep +Z */
    f[0] = o[0] / n; f[1] = o[2] / n;
}

/* Tracker position of world point p for a viewer at me facing f (from hud_forward): x right, y forward,
   both in units of TRACK_RANGE. Returns the flat distance in metres. */
static inline float hud_track(const float me[3], const float f[2], const float p[3], float *x, float *y) {
    float dx = p[0] - me[0], dz = p[2] - me[2];
    /* right = up x forward = (f.z, -f.x) in the XZ plane */
    *x = (dx * f[1] - dz * f[0]) / TRACK_RANGE;
    *y = (dx * f[0] + dz * f[1]) / TRACK_RANGE;
    return sqrtf(dx * dx + dz * dz);
}

/* Motion memory per character-table slot: where it was about a second ago. */
typedef struct { float p[3]; unsigned t; int used; float moved; unsigned t_moved; } TrackSlot;

static inline void hud_motion_update(TrackSlot *s, const float p[3], unsigned now) {
    if (!s->used) { memcpy(s->p, p, 12); s->t = now; s->used = 1; s->moved = 0; return; }
    if (now - s->t >= TRACK_WINDOW_MS) {
        float dx = p[0] - s->p[0], dy = p[1] - s->p[1], dz = p[2] - s->p[2];
        s->moved = sqrtf(dx * dx + dy * dy + dz * dz);
        if (s->moved > 30.f) s->moved = 0;   /* teleport (spawn, level change): not motion */
        if (s->moved >= TRACK_MOVE) s->t_moved = now;
        memcpy(s->p, p, 12); s->t = now;
    }
}
/* A contact shows for 1.5 s after it last moved, like the game's tracker blips fading. */
static inline int hud_motion_visible(const TrackSlot *s, unsigned now) { return s->used && s->t_moved && now - s->t_moved < 1500u; }

/* Ping interval (ms) for the nearest moving contact at distance d: faster when closer. 0 = silent. */
static inline unsigned hud_ping_ms(float d) {
    if (d < 0 || d > TRACK_RANGE) return 0;
    return 350u + (unsigned)(1250.f * d / TRACK_RANGE);
}
