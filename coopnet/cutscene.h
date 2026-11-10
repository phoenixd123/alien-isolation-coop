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

#define CUT_TRAIL       64        /* points at least CUT_STEP_M apart: the last ~20+ m walked */
#define CUT_BACK_M      1.0f      /* land this far behind the partner */
#define CUT_TRAIL_MS    60000     /* older points aren't used */
#define CUT_STEP_M      0.3f      /* a new point only after moving this far (standing still keeps the old ones) */
#define CUT_NEAR_M      3.0f      /* already this close: don't move */
#define CUT_BOTH_MS     2000      /* both started one within this: the joiner goes to the host */

typedef struct { uint32_t t[CUT_TRAIL]; float p[CUT_TRAIL][3]; int n, head; } CutTrail;

static inline void cut_trail_push(CutTrail *tr, uint32_t t, const float p[3]) {
    if (tr->n) {   /* standing still: keep the points behind us (in game a still partner made us land inside them) */
        const float *last = tr->p[(tr->head + CUT_TRAIL - 1) % CUT_TRAIL];
        float dx = p[0] - last[0], dy = p[1] - last[1], dz = p[2] - last[2];
        if (dx * dx + dy * dy + dz * dz < CUT_STEP_M * CUT_STEP_M) { tr->t[(tr->head + CUT_TRAIL - 1) % CUT_TRAIL] = t; return; }
    }
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

/* Carry-along: when the game moves you by script in one go (a lift ride between floors, a scripted move), your
   partner standing right next to you is moved by the same amount, so you ride together. Nothing about lifts is
   known here: a jump of more than CARRY_JUMP_M between two position samples (30 a second) that the mod didn't
   make, while alive and well into a level, counts as a scripted move. */
#define CARRY_JUMP_M   2.5f
#define CARRY_MAX_M    400.f      /* bigger: a reload or a level, not a move */
#define CARRY_GAP_MS   150        /* the two samples at most this far apart */
#define CARRY_QUIET_MS 3000       /* not this soon after a mod teleport or entering the level */
#define CARRY_NEAR_M   3.0f       /* the partner was this close to where you jumped from */
static inline int carry_is_jump(const float a[3], const float b[3], int32_t dt_ms, int32_t since_tp_ms, int32_t in_level_ms, int alive) {
    float dx = b[0] - a[0], dy = b[1] - a[1], dz = b[2] - a[2], d = sqrtf(dx * dx + dy * dy + dz * dz);
    if (!alive || dt_ms < 0 || dt_ms > CARRY_GAP_MS || in_level_ms < CARRY_QUIET_MS) return 0;
    if (since_tp_ms >= 0 && since_tp_ms < CARRY_QUIET_MS) return 0;
    return d == d && d > CARRY_JUMP_M && d < CARRY_MAX_M;
}
/* A move only counts once it has settled: one jump, then the player stays near where it landed for CARRY_SETTLE_MS
   with no other jump. Kill and grab animations (found in game: the alien's kill moved the player 3-4 m back and forth
   several times a second) and menus jump again and again and never settle. At most one every CARRY_EVERY_MS. */
#define CARRY_SETTLE_MS 500
#define CARRY_SHAKY_MS  2000      /* a jump this soon after another one: not a clean move */
#define CARRY_EVERY_MS  8000
typedef struct { int pending; float from[3], to[3]; uint32_t t_jump, last_jump, last_sent; } CarryWatch;
/* Feed every position sample (prev -> cur). 1 when a settled move should be told to the partner (from/to set). */
static inline int carry_watch(CarryWatch *w, const float prev[3], const float cur[3], uint32_t now, int32_t dt_ms,
                              int32_t since_tp_ms, int32_t in_level_ms, int alive, float from[3], float to[3]) {
    if (carry_is_jump(prev, cur, dt_ms, since_tp_ms, in_level_ms, alive)) {
        int shaky = w->pending || (w->last_jump && (int32_t)(now - w->last_jump) < CARRY_SHAKY_MS);
        w->last_jump = now | 1;
        if (shaky) { w->pending = 0; return 0; }
        w->pending = 1; memcpy(w->from, prev, 12); memcpy(w->to, cur, 12); w->t_jump = now;
        return 0;
    }
    if (!w->pending) return 0;
    float dx = cur[0] - w->to[0], dy = cur[1] - w->to[1], dz = cur[2] - w->to[2];
    if (!alive || dx * dx + dy * dy + dz * dz > 1.0f) { w->pending = 0; return 0; }
    if ((int32_t)(now - w->t_jump) < CARRY_SETTLE_MS) return 0;
    w->pending = 0;
    if (w->last_sent && (int32_t)(now - w->last_sent) < CARRY_EVERY_MS) return 0;
    w->last_sent = now | 1;
    memcpy(from, w->from, 12); memcpy(to, w->to, 12);
    return 1;
}
/* The partner jumped from `from` to `to`; we're at `me`. 1 and the spot to move to if we were with them. */
static inline int carry_follow(int alive, int have_me, const float me[3], const float from[3], const float to[3], float out[3]) {
    if (!alive || !have_me) return 0;
    float dx = me[0] - from[0], dy = me[1] - from[1], dz = me[2] - from[2];
    if (!(dx * dx + dy * dy + dz * dz <= CARRY_NEAR_M * CARRY_NEAR_M)) return 0;
    for (int i = 0; i < 3; i++) out[i] = me[i] + (to[i] - from[i]);
    return 1;
}
