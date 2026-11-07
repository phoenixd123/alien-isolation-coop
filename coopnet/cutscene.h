/* Cutscene teleport (pure code, tested in test/sync_test.c).
 *
 * When a cutscene starts in your game (worldsync.c spots it from the cutscene tables in syncmap.h), coopnet sends
 * your partner a CutPkt with a spot to land on. Their game moves them there right away, so you watch the scene
 * together and carry on from the same place. The spot is a point you walked through a moment ago, about a metre
 * behind you: somewhere a player fits, and not inside your body.
 */
#pragma once
#include <stdint.h>
#include <string.h>
#include <math.h>

#define CUT_TRAIL       64        /* ~2 s at 30 Hz */
#define CUT_BACK_M      1.0f      /* land this far behind the partner */
#define CUT_TRAIL_MS    3000
#define CUT_NEAR_M      3.0f      /* already this close: don't move */
#define CUT_BOTH_MS     2000      /* both started one within this: the joiner goes to the host */

typedef struct { uint32_t t[CUT_TRAIL]; float p[CUT_TRAIL][3]; int n, head; } CutTrail;

static inline void cut_trail_push(CutTrail *tr, uint32_t t, const float p[3]) {
    tr->t[tr->head] = t; memcpy(tr->p[tr->head], p, 12);
    tr->head = (tr->head + 1) % CUT_TRAIL;
    if (tr->n < CUT_TRAIL) tr->n++;
}

/* Where the partner should land: the newest trail point at least CUT_BACK_M (on the floor plane) behind the
   newest one and not older than CUT_TRAIL_MS; the newest point itself if there's none. 0 if the trail is empty. */
static inline int cut_safe_spot(const CutTrail *tr, uint32_t now, float out[3]) {
    if (!tr->n) return 0;
    int newest = (tr->head + CUT_TRAIL - 1) % CUT_TRAIL;
    const float *cur = tr->p[newest];
    for (int k = 1; k < tr->n; k++) {
        int i = (newest + CUT_TRAIL - k) % CUT_TRAIL;
        if ((int32_t)(now - tr->t[i]) > CUT_TRAIL_MS) break;
        float dx = tr->p[i][0] - cur[0], dz = tr->p[i][2] - cur[2];
        if (dx * dx + dz * dz >= CUT_BACK_M * CUT_BACK_M) { memcpy(out, tr->p[i], 12); return 1; }
    }
    memcpy(out, cur, 12);
    return 1;
}

/* Should I move to my partner's cutscene? since_my_cut: ms since I started one myself (-1 = not recently). */
static inline int cut_should_follow(int alive, int in_level, float dist, int32_t since_my_cut, int am_host) {
    if (!alive || !in_level) return 0;
    if (!(dist == dist) || dist < CUT_NEAR_M) return 0;
    if (since_my_cut >= 0 && since_my_cut < CUT_BOTH_MS) return !am_host;   /* we both did: the joiner moves */
    return 1;
}
