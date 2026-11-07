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

/* Regroup (hold F7): you ask, your partner answers with a landing spot from their trail (same CutPkt, see `flags`),
   and you're moved there. Allowed while you're alive, in a level and not already close. */
#define CUT_SCENE        0        /* CutPkt.flags: a cutscene started, come here */
#define CUT_REGROUP_ASK  1        /* "I want to come to you" */
#define CUT_REGROUP_SPOT 2        /* the answer: land here */
#define REGROUP_HOLD_MS  1500
#define REGROUP_GAP_MS   10000
static inline int regroup_ok(int alive, int in_level, float dist) {
    return alive && in_level && dist == dist && dist >= CUT_NEAR_M;
}

/* Fade around a teleport: dark over TP_FADE_OUT_MS, the move happens at full dark, then back over TP_FADE_IN_MS.
   Returns the darkness 0..1 at `ms` after the fade started. */
#define TP_FADE_OUT_MS 250
#define TP_FADE_IN_MS  450
static inline float tp_fade(int32_t ms) {
    if (ms < 0) return 0.f;
    if (ms < TP_FADE_OUT_MS) return (float)ms / TP_FADE_OUT_MS;
    if (ms < TP_FADE_OUT_MS + TP_FADE_IN_MS) return 1.f - (float)(ms - TP_FADE_OUT_MS) / TP_FADE_IN_MS;
    return 0.f;
}
