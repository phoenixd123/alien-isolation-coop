/* Playback of the host's alien positions on the joiner (pure code; tested in test/alien_test.c).
 * Samples arrive about 30 times a second with network jitter. Playing them back a little behind real time and
 * interpolating between the two samples around that moment gives a smooth, exact path instead of jumps.
 * A jump bigger than ALIEN_TELEPORT_M between samples (the alien leaving or entering a vent somewhere else) is
 * taken as a teleport: no sliding across the map. */
#pragma once
#include <stdint.h>
#include <string.h>

#define ALIEN_SAMPLES 8
#define ALIEN_TELEPORT_M 6.0f

typedef struct { uint32_t t[ALIEN_SAMPLES]; float p[ALIEN_SAMPLES][3]; int n, head; } AlienTrack;

static inline void alien_track_push(AlienTrack *a, uint32_t t, const float p[3]) {
    if (a->n && (int32_t)(t - a->t[(a->head + ALIEN_SAMPLES - 1) % ALIEN_SAMPLES]) < 0) return;   /* out of order */
    a->t[a->head] = t; memcpy(a->p[a->head], p, 12);
    a->head = (a->head + 1) % ALIEN_SAMPLES;
    if (a->n < ALIEN_SAMPLES) a->n++;
}

/* Position at time `at`: interpolated between the samples around it, the oldest sample if `at` is older than
   all of them, and the newest if `at` is newer (no guessing ahead). 0 if there are no samples. */
static inline int alien_track_at(const AlienTrack *a, uint32_t at, float out[3]) {
    if (!a->n) return 0;
    int newest = (a->head + ALIEN_SAMPLES - 1) % ALIEN_SAMPLES, oldest = (a->head + ALIEN_SAMPLES - a->n) % ALIEN_SAMPLES;
    if ((int32_t)(at - a->t[newest]) >= 0) { memcpy(out, a->p[newest], 12); return 1; }
    if ((int32_t)(at - a->t[oldest]) <= 0) { memcpy(out, a->p[oldest], 12); return 1; }
    for (int k = a->n - 1; k > 0; k--) {
        int i1 = (oldest + k) % ALIEN_SAMPLES, i0 = (oldest + k - 1) % ALIEN_SAMPLES;
        if ((int32_t)(at - a->t[i0]) >= 0) {
            float dx = a->p[i1][0] - a->p[i0][0], dy = a->p[i1][1] - a->p[i0][1], dz = a->p[i1][2] - a->p[i0][2];
            if (dx * dx + dy * dy + dz * dz > ALIEN_TELEPORT_M * ALIEN_TELEPORT_M) { memcpy(out, a->p[i0], 12); return 1; }
            uint32_t span = a->t[i1] - a->t[i0];
            float f = span ? (float)(at - a->t[i0]) / (float)span : 1.f;
            for (int c = 0; c < 3; c++) out[c] = a->p[i0][c] + (a->p[i1][c] - a->p[i0][c]) * f;
            return 1;
        }
    }
    memcpy(out, a->p[oldest], 12);
    return 1;
}
