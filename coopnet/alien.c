/* alien: one alien for both players.
 *
 * The host's alien is the real one: it thinks, hunts and kills as in the single-player game, and it sees your
 * partner's body (skin.c) like any other target. The joiner's alien is a copy of it:
 *   - its behaviour tree isn't run, so it never decides anything of its own;
 *   - each frame it gets the game's move-in-direction order towards where the host's alien is, at the same speed
 *     (alien_pup.h), so it walks, runs and turns with its own animations; when it's close it stops and turns to the
 *     host's alien's facing; too far off, or in the vents, it is put there;
 *   - its alien setup and what the game has let it learn (its tricks and the counters behind them) are the host's;
 *   - when the joiner's body dies in the host's game (the alien got them), the joiner dies in their own game.
 * And on the host: your partner running near the alien is heard, as the player's own running would be.
 *
 * Method, addresses and layouts from OpenCAGE's RuntimeUtils by Matt Filer, https://github.com/MattFiler/OpenCAGE,
 * MIT license (third_party/licenses/OpenCAGE-LICENSE.txt): POSSESS.cpp (skipping the tree, the move order),
 * AI_ALIEN.cpp (learning, setups, the sensed message), AI_STATE.cpp (force kill), DEBUG_MENU.cpp (fields).
 * Steam build 4940658:
 *   behaviour_update   RVA 0x3342A0 thiscall(behaviour, float elapsed, float time)   hooked; +0x128 its character
 *   tree_update        RVA 0x32B880 thiscall(tree, bool full)                       hooked; skipped for our puppet
 *   create_move_order  RVA 0x2823B0 thiscall(orders, Shared *out)    queue_order 0x283CE0, withdraw_order 0x283DB0
 *   set_alien_config   RVA 0x393E70 thiscall(alien, const uint32_t *setup, bool notify)
 *   make_sensed        RVA 0x0AB520 thiscall(msg, type, entity, composite, pos, seen, glass, threshold, time, step)
 *   post_sensed        RVA 0x0E1C20 thiscall(events, msg, bool now)
 *   force_kill         RVA 0x3D0C10 thiscall(character, killer)
 *   reposition         RVA 0x3ECCA0 thiscall(controller, float pos[4])  controllers exe+0x1307A48, 0xB0 each
 *   globals [exe+0x12F0C88] (+4 valid): +0x08 characters (+0x80/+0x84 players, +0xD0/+0xD4 the others),
 *     +0x84 alien setups (count at +0x0C), +0xA8 learning (+0 asked for, +4 has, +8 four counters, +0x18 mission)
 *   game time exe+0x12410A8 (double; the step at +0x18)
 *   character: +0x50 id, +0x54 entity handle (2 ids), +0x60 controller, +0x64 spawned as (class at +4; 1 alien),
 *     +0x70 position, +0x174 facing (yaw), +0x33C health (u16), +0x2CD0 orders, +0x3BA8 events, +0x42A4 setup,
 *     +0x42A8 locomotion (+0x190/+0x194 speed asked for, +0x198 speed now, +0x1E3 in the vents)
 *   order: +0x0C status (3..6 over), +0x10 type (9 move in a direction), +0x18 given, +0x1C heading, +0x20 turn,
 *     +0x24 distance ahead, +0x34 changed
 */
#include <windows.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <stdio.h>
#include "alien.h"
#include "alien_pup.h"
#include "alien_interp.h"

#define RVA_BEHAVIOUR   0x3342A0u
#define RVA_TREE        0x32B880u
#define RVA_MOVE_ORDER  0x2823B0u
#define RVA_QUEUE       0x283CE0u
#define RVA_WITHDRAW    0x283DB0u
#define RVA_SET_SETUP   0x393E70u
#define RVA_MAKE_SENSED 0x0AB520u
#define RVA_POST_SENSED 0x0E1C20u
#define RVA_FORCE_KILL  0x3D0C10u
#define RVA_REPOSITION  0x3ECCA0u
#define RVA_CONTROLLERS 0x1307A48u
#define RVA_GLOBALS     0x12F0C88u
#define RVA_GAME_TIME   0x12410A8u

#define CLASS_ALIEN 1
#define ORDER_MOVE  9
#define DELAY_MS    50
#define STALE_MS    500

typedef struct { uint8_t *object, *counts; } Shared;
typedef void(__attribute__((thiscall)) *behaviour_fn)(void *, float, float);
typedef void(__attribute__((thiscall)) *tree_fn)(void *, int);
typedef Shared *(__attribute__((thiscall)) *create_fn)(void *, Shared *);
typedef int(__attribute__((thiscall)) *order_fn)(void *);
typedef void(__attribute__((thiscall)) *withdraw_fn)(void *);
typedef void(__attribute__((thiscall)) *setup_fn)(void *, const uint32_t *, int);
typedef void(__attribute__((thiscall)) *make_fn)(void *, int, uint32_t, uint32_t, const float *, float, int, int, float, float);
typedef void(__attribute__((thiscall)) *post_fn)(void *, const void *, int);
typedef void(__attribute__((thiscall)) *kill_fn)(void *, const void *);
typedef void(__attribute__((thiscall)) *repos_fn)(void *, const float *);
typedef void(__attribute__((thiscall)) *counts_fn)(void *);

static void (*g_log)(const char *, ...);
static uintptr_t g_base;
static int g_on, g_host;
static behaviour_fn g_behaviour_orig;
static tree_fn g_tree_orig;
static create_fn g_create; static order_fn g_queue; static withdraw_fn g_withdraw; static setup_fn g_set_setup;
static make_fn g_make; static post_fn g_post; static kill_fn g_kill; static repos_fn g_repos;

static CRITICAL_SECTION g_cs;
/* host: the alien as read on the AI thread (for the net thread to send) */
static AlienExPkt g_mine; static volatile DWORD g_mine_t;
static uint32_t g_kills; static int g_body_alive;
/* host: the partner, for the noise */
static float g_partner[3]; static DWORD g_partner_t; static float g_partner_mps; static DWORD g_noise_t;
/* joiner: what the host sent */
static AlienExPkt g_theirs; static volatile DWORD g_theirs_t, g_theirs_any_t; static AlienTrack g_track;
static int g_kills_known; static uint32_t g_kills_seen; static volatile LONG g_kill_due;

/* AI-thread state (joiner's puppet) */
static __thread int t_skip_tree;
static uint8_t *g_pup; static uint32_t g_pup_id;
static Shared g_order; static PupState g_ps;
static DWORD g_setup_t, g_learn_log_t, g_drive_log_t; static unsigned g_tps, g_steers;

static int rd(uintptr_t a, void *o, size_t n) {
    SIZE_T got = 0;
    return a > 0x10000 && a < 0x7FFF0000 && ReadProcessMemory(GetCurrentProcess(), (LPCVOID)a, o, n, &got) && got == n;
}
static int wr(uintptr_t a, const void *v, size_t n) {
    SIZE_T put = 0;
    return a > 0x10000 && a < 0x7FFF0000 && WriteProcessMemory(GetCurrentProcess(), (LPVOID)a, v, n, &put) && put == n;
}
static uint32_t u32(uintptr_t a) { uint32_t v = 0; return rd(a, &v, 4) ? v : 0; }

static uint32_t globals(void) {
    uint32_t g = 0; uint8_t valid = 0;
    if (!rd(g_base + RVA_GLOBALS, &g, 4) || !rd(g_base + RVA_GLOBALS + 4, &valid, 1) || !valid) return 0;
    return g;
}
static int char_class(uint32_t c) { uint32_t s = u32(c + 0x64); int k = -1; return s && rd(s + 4, &k, 4) ? k : -1; }
static uint16_t health(uint32_t c) { uint16_t h = 0; rd(c + 0x33C, &h, 2); return h; }
static uint32_t first_player(void) {
    uint32_t g = globals(), mgr = g ? u32(g + 0x08) : 0, list = mgr ? u32(mgr + 0x80) : 0, n = mgr ? u32(mgr + 0x84) : 0;
    return list && n && n <= 18 ? u32(list) : 0;
}
static float game_time(double *step) {
    double t = 0, s = 0;
    rd(g_base + RVA_GAME_TIME, &t, 8); rd(g_base + RVA_GAME_TIME + 0x18, &s, 8);
    if (step) *step = s;
    return (float)t;
}

/* lets go of a shared pointer as the game does (POSSESS.cpp Drop) */
static void drop(Shared *s) {
    uint8_t *counts = s->counts;
    s->object = s->counts = NULL;
    if (!counts) return;
    if (InterlockedDecrement((volatile LONG *)(counts + 4)) == 0) {
        void **table = *(void ***)counts;
        ((counts_fn)table[0])(counts);
        if (InterlockedDecrement((volatile LONG *)(counts + 8)) == 0) ((counts_fn)table[1])(counts);
    }
}
static int order_over(const uint8_t *o) { int st = 0; rd((uintptr_t)o + 0x0C, &st, 4); return st >= 3 && st <= 6; }
static void withdraw(Shared *s, int gone) {
    if (gone) { s->object = s->counts = NULL; return; }
    if (s->object && !order_over(s->object)) g_withdraw(s->object);
    drop(s);
}

static void release(const char *why, int gone) {
    if (!g_pup) return;
    withdraw(&g_order, gone);
    g_ps.steering = 0;
    if (g_log) g_log("alien: the joiner's alien thinks for itself again (%s)", why);
    g_pup = NULL; g_pup_id = 0;
}

static int steer(uint8_t *c, float heading, int speed) {
    if (g_order.object) {
        int type = 0; rd((uintptr_t)g_order.object + 0x10, &type, 4);
        if (type != ORDER_MOVE || order_over(g_order.object)) drop(&g_order);
    }
    if (!g_order.object) {
        Shared fresh = {0};
        g_create(c + 0x2CD0, &fresh);
        int type = 0;
        if (!fresh.object || !rd((uintptr_t)fresh.object + 0x10, &type, 4) || type != ORDER_MOVE) { drop(&fresh); return 0; }
        g_queue(fresh.object);
        fresh.object[0x18] = 1;
        g_order = fresh;
    }
    *(float *)(g_order.object + 0x1C) = heading;
    *(float *)(g_order.object + 0x20) = 1.5707964f;
    *(float *)(g_order.object + 0x24) = 10.0f;
    g_order.object[0x34] = 1;
    uint32_t loco = u32((uintptr_t)c + 0x42A8);
    if (loco) { int sp = speed, zero = 0; wr(loco + 0x194, &sp, 4); wr(loco + 0x190, &zero, 4); }
    return 1;
}
static void coast(void) {
    if (!g_order.object || order_over(g_order.object)) return;
    float ahead = *(float *)(g_order.object + 0x24);
    if (ahead > 2.0f) { *(float *)(g_order.object + 0x24) = 2.0f; g_order.object[0x34] = 1; }
}
static void put_at(uint32_t c, const float p[3]) {
    int32_t idx = -1;
    if (!rd(c + 0x60, &idx, 4) || idx < 0 || idx >= 18) return;
    uintptr_t ctl = g_base + RVA_CONTROLLERS + (uintptr_t)idx * 0xB0u;
    uint8_t ready = 0;
    if (u32(ctl) != c || !rd(ctl + 0xA8, &ready, 1) || !ready) return;
    float pos[4] __attribute__((aligned(16))) = {p[0], p[1], p[2], 0.f};
    g_repos((void *)ctl, pos);
}

/* ---- host: read the alien, watch the partner's body, make the partner heard ---- */
static void host_frame(uint32_t c) {
    static uint32_t seen_id;
    uint32_t cid = u32(c + 0x50);
    if (cid != seen_id) { seen_id = cid; if (g_log) g_log("alien: host's alien found (character %u), sharing it", cid); }
    AlienExPkt k; memset(&k, 0, sizeof k);
    k.flags = ALX_FOUND;
    rd(c + 0x70, k.pos, 12); rd(c + 0x174, &k.yaw, 4);
    uint32_t loco = u32(c + 0x42A8);
    if (loco) {
        int sp = 0; uint8_t vents = 0;
        rd(loco + 0x198, &sp, 4); rd(loco + 0x1E3, &vents, 1);
        k.speed = (uint8_t)(sp < 0 ? 0 : sp > 3 ? 3 : sp);
        if (vents) k.flags |= ALX_VENTS;
    }
    k.setup = 0xFFFFFFFFu; rd(c + 0x42A4, &k.setup, 4);
    uint32_t g = globals(), learn = g ? u32(g + 0xA8) : 0;
    if (learn && rd(learn, &k.learn_req, 4) && rd(learn + 4, &k.learn_have, 4) && rd(learn + 8, k.counters, 16) && rd(learn + 0x18, &k.mission, 4))
        k.flags |= ALX_LEARN;

    /* the partner's body in our game dying = the partner dies (they are the ones it got) */
    uint32_t body = alien_body_hook ? alien_body_hook() : 0;
    if (body) {
        int alive = health(body) > 0;
        if (g_body_alive && !alive) { g_kills++; if (g_log) g_log("alien: the partner's body died in our game, telling them (%u)", g_kills); }
        g_body_alive = alive;
    } else g_body_alive = 0;
    k.kills = g_kills;

    EnterCriticalSection(&g_cs);
    g_mine = k; g_mine_t = GetTickCount();
    float pp[3]; memcpy(pp, g_partner, 12); DWORD pt = g_partner_t; float mps = g_partner_mps;
    LeaveCriticalSection(&g_cs);

    DWORD now = GetTickCount();
    if (now - pt < 500 && mps > PUP_NOISE_MPS && now - g_noise_t > PUP_NOISE_MS && !(k.flags & ALX_VENTS)) {
        uint32_t who = body ? body : first_player();
        uint32_t handle[2];
        if (who && rd(who + 0x54, handle, 8)) {
            uint8_t msg[0x40] __attribute__((aligned(16)));
            double step; float t = game_time(&step);
            memset(msg, 0, sizeof msg);
            g_make(msg, 11, handle[0], handle[1], pp, 1.0f, 0, 1, t, (float)step);   /* heard moving, "suspicious" threshold */
            g_post((void *)(uintptr_t)(c + 0x3BA8), msg, 0);
            g_noise_t = now;
        }
    }
}

/* ---- joiner: copy the setup and the learning, drive the puppet ---- */
static void joiner_learning(uint32_t c, const AlienExPkt *h) {
    DWORD now = GetTickCount();
    if (h->setup != 0xFFFFFFFFu && now - g_setup_t > 2000) {
        uint32_t mine = 0xFFFFFFFFu, g = globals(), setups = g ? u32(g + 0x84) : 0, n = setups ? u32(setups + 0x0C) : 0;
        rd(c + 0x42A4, &mine, 4);
        if (mine != h->setup && h->setup < n) {
            uint32_t s = h->setup;
            g_set_setup((void *)(uintptr_t)c, &s, 1);
            g_setup_t = now;
            if (g_log) g_log("alien: setup %u -> %u (the host's)", mine, s);
        }
    }
    if (h->flags & ALX_LEARN) {
        uint32_t g = globals(), learn = g ? u32(g + 0xA8) : 0;
        uint8_t now_b[0x1C], want[0x1C];
        if (learn && rd(learn, now_b, sizeof now_b)) {
            memcpy(want, now_b, sizeof want);
            memcpy(want, &h->learn_req, 4); memcpy(want + 4, &h->learn_have, 4); memcpy(want + 8, h->counters, 16); memcpy(want + 0x18, &h->mission, 4);
            if (memcmp(want, now_b, sizeof want) && wr(learn, want, sizeof want) && g_log && now - g_learn_log_t > 10000) {
                g_learn_log_t = now;
                g_log("alien: learning set to the host's (tricks 0x%x, counters %d %d %d %d)", h->learn_have, h->counters[0], h->counters[1], h->counters[2], h->counters[3]);
            }
        }
    }
}

static void drive(uint8_t *c, const AlienExPkt *h) {
    DWORD now = GetTickCount();
    float want[3], me[3], yaw = 0;
    EnterCriticalSection(&g_cs);
    int have = alien_track_at(&g_track, now - DELAY_MS, want);
    LeaveCriticalSection(&g_cs);
    if (!have || !rd((uintptr_t)c + 0x70, me, 12) || !rd((uintptr_t)c + 0x174, &yaw, 4)) return;
    PupStep s = pup_decide(&g_ps, me, yaw, want, h->yaw, h->speed, (h->flags & ALX_VENTS) != 0);
    if (s.act == PUP_TELEPORT) {
        withdraw(&g_order, 0);
        put_at((uint32_t)(uintptr_t)c, want);
        if (g_log && (++g_tps == 1 || now - g_drive_log_t > 10000)) { g_drive_log_t = now; g_log("alien: put at the host's alien (%u times, %u steers)", g_tps, g_steers); }
    } else if (s.act == PUP_STEER) {
        if (steer(c, s.heading, s.speed)) g_steers++;
    } else {
        coast();
        if (s.set_yaw) wr((uintptr_t)c + 0x174, &s.yaw, 4);
    }
}

static void __attribute__((thiscall)) tree_hook(void *self, int full) {
    if (t_skip_tree) return;
    g_tree_orig(self, full);
}

static void __attribute__((thiscall)) behaviour_hook(void *self, float elapsed, float time) {
    /* the joiner's death runs on this thread (as OpenCAGE's changes do), from whichever character updates first */
    if (!g_host && g_kill_due) {
        uint32_t pl = first_player();
        InterlockedExchange(&g_kill_due, 0);
        if (pl && health(pl) > 0) { g_kill((void *)(uintptr_t)pl, NULL); if (g_log) g_log("alien: the host's alien got us"); }
    }
    uint32_t c = u32((uintptr_t)self + 0x128);
    int alien = c && char_class(c) == CLASS_ALIEN && health(c) > 0;
    /* one puppet: a second alien (rare) thinks for itself while the first is still there */
    if (alien && !g_host && g_pup && (uint32_t)(uintptr_t)g_pup != c && u32((uintptr_t)g_pup + 0x50) == g_pup_id && health((uint32_t)(uintptr_t)g_pup) > 0)
        alien = 0;
    if (!alien) {
        g_behaviour_orig(self, elapsed, time);
        return;
    }
    if (g_host) {
        g_behaviour_orig(self, elapsed, time);
        host_frame(c);
        return;
    }
    AlienExPkt h; DWORD now = GetTickCount();
    EnterCriticalSection(&g_cs); h = g_theirs; DWORD ht = g_theirs_t; LeaveCriticalSection(&g_cs);
    int fresh = ht && now - ht < STALE_MS && (h.flags & ALX_FOUND);
    /* While the host is connected our alien never thinks for itself, even when the host's alien is gone for a moment:
       it would hunt us on its own. It only does again when we hear nothing from the host for a few seconds. */
    DWORD any = g_theirs_any_t;
    int connected = any && now - any < 3000;
    uint32_t id = u32(c + 0x50);
    if (g_pup && ((uint32_t)(uintptr_t)g_pup != c || g_pup_id != id)) release("the alien changed", 1);
    if (!connected) {
        if ((uint32_t)(uintptr_t)g_pup == c) release("no word from the host", 0);
        g_behaviour_orig(self, elapsed, time);
        return;
    }
    if (!fresh) {   /* the host's alien isn't there just now: ours waits where it is */
        if (!g_pup) { g_pup = (uint8_t *)(uintptr_t)c; g_pup_id = id; }
        t_skip_tree = 1;
        g_behaviour_orig(self, elapsed, time);
        t_skip_tree = 0;
        coast();
        return;
    }
    if (!g_pup) { g_pup = (uint8_t *)(uintptr_t)c; g_pup_id = id; if (g_log) g_log("alien: the joiner's alien follows the host's now"); }
    t_skip_tree = 1;
    g_behaviour_orig(self, elapsed, time);
    t_skip_tree = 0;
    joiner_learning(c, &h);
    drive(g_pup, &h);
}

/* ---- install ---- */
static int bytes_ok(uintptr_t a, const uint8_t *want, int n) { uint8_t got[16]; return n <= 16 && rd(a, got, (size_t)n) && !memcmp(got, want, (size_t)n); }

static void *detour(uint8_t *fn, int n, void *hook) {
    uint8_t *tr = VirtualAlloc(NULL, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tr) return NULL;
    memcpy(tr, fn, (size_t)n);
    tr[n] = 0xE9; *(int32_t *)(tr + n + 1) = (int32_t)((fn + n) - (tr + n + 5));
    DWORD old;
    VirtualProtect(fn, (SIZE_T)n, PAGE_EXECUTE_READWRITE, &old);
    fn[0] = 0xE9; *(int32_t *)(fn + 1) = (int32_t)((uint8_t *)hook - (fn + 5));
    for (int i = 5; i < n; i++) fn[i] = 0x90;
    VirtualProtect(fn, (SIZE_T)n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), fn, (SIZE_T)n);
    return tr;
}

uint32_t (*alien_body_hook)(void);

int alien_install(void (*logf)(const char *, ...), int host) {
    if (g_on) return 1;
    g_log = logf; g_host = host;
    g_base = (uintptr_t)GetModuleHandleW(NULL);
    /* whole instructions with no relative addresses in them (the detours copy them) */
    static const uint8_t s_beh[9] = {0x56, 0x8B, 0xF1, 0x8B, 0x8E, 0x28, 0x01, 0x00, 0x00};
    static const uint8_t s_tree[8] = {0x83, 0xEC, 0x08, 0x53, 0x55, 0x56, 0x8B, 0xF1};
    /* the others: the bytes before their first relocated address */
    static const uint8_t s_move[6] = {0x51, 0x56, 0x8B, 0x74, 0x24, 0x0C};
    static const uint8_t s_order[9] = {0x64, 0xA1, 0x00, 0x00, 0x00, 0x00, 0x6A, 0xFF, 0x68};
    static const uint8_t s_setup[9] = {0x8B, 0x44, 0x24, 0x04, 0x8B, 0x00, 0x83, 0xF8, 0xFF};
    static const uint8_t s_make[6] = {0x8B, 0x54, 0x24, 0x04, 0x8B, 0xC1};
    static const uint8_t s_post[3] = {0x6A, 0xFF, 0x68};
    static const uint8_t s_kill[7] = {0x51, 0x56, 0x8B, 0xF1, 0x8B, 0x4E, 0x64};
    static const uint8_t s_repos[6] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0};
    if (!bytes_ok(g_base + RVA_BEHAVIOUR, s_beh, 9) || !bytes_ok(g_base + RVA_TREE, s_tree, 8) || !bytes_ok(g_base + RVA_MOVE_ORDER, s_move, 6) ||
        !bytes_ok(g_base + RVA_QUEUE, s_order, 9) || !bytes_ok(g_base + RVA_WITHDRAW, s_order, 9) || !bytes_ok(g_base + RVA_SET_SETUP, s_setup, 9) ||
        !bytes_ok(g_base + RVA_MAKE_SENSED, s_make, 6) || !bytes_ok(g_base + RVA_POST_SENSED, s_post, 3) || !bytes_ok(g_base + RVA_FORCE_KILL, s_kill, 7) ||
        !bytes_ok(g_base + RVA_REPOSITION, s_repos, 6)) {
        logf("alien: functions don't match this build, the shared alien is off");
        return 0;
    }
    InitializeCriticalSection(&g_cs);
    g_create = (create_fn)(g_base + RVA_MOVE_ORDER); g_queue = (order_fn)(g_base + RVA_QUEUE); g_withdraw = (withdraw_fn)(g_base + RVA_WITHDRAW);
    g_set_setup = (setup_fn)(g_base + RVA_SET_SETUP); g_make = (make_fn)(g_base + RVA_MAKE_SENSED); g_post = (post_fn)(g_base + RVA_POST_SENSED);
    g_kill = (kill_fn)(g_base + RVA_FORCE_KILL); g_repos = (repos_fn)(g_base + RVA_REPOSITION);
    g_tree_orig = (tree_fn)detour((uint8_t *)(g_base + RVA_TREE), 8, (void *)tree_hook);
    g_behaviour_orig = (behaviour_fn)detour((uint8_t *)(g_base + RVA_BEHAVIOUR), 9, (void *)behaviour_hook);
    if (!g_tree_orig || !g_behaviour_orig) { logf("alien: couldn't hook"); return 0; }
    g_on = 1;
    logf("alien: shared alien on");
    return 1;
}

void alien_set_host(int host) {
    g_host = host;
    if (g_on && g_log) g_log("alien: %s", host ? "host: ours is the real one" : "joiner: ours follows the host's");
}

/* The first living alien in the character manager's list of the others (not players), 0 if none. Any thread. */
static uint32_t find_alien(void) {
    uint32_t g = globals(), mgr = g ? u32(g + 0x08) : 0, list = mgr ? u32(mgr + 0xD0) : 0, n = mgr ? u32(mgr + 0xD4) : 0;
    for (uint32_t i = 0; list && i < n && i < 64; i++) {
        uint32_t c = u32(list + i * 4);
        if (c && char_class(c) == CLASS_ALIEN && health(c) > 0) return c;
    }
    return 0;
}

int alien_read(AlienExPkt *out) {
    if (!g_on || !g_host) return 0;
    EnterCriticalSection(&g_cs);
    int ok = g_mine_t && GetTickCount() - g_mine_t < STALE_MS;
    if (ok) *out = g_mine; else { memset(out, 0, sizeof *out); out->setup = 0xFFFFFFFFu; out->kills = g_kills; }
    LeaveCriticalSection(&g_cs);
    if (!ok) {
        /* No behaviour update for a while (the alien is in a kill or another animation, or the vents): it may still be
           there. Read it straight from its character, so the joiner's alien stays with it instead of letting go
           (found in game: the joiner's alien, let go during the host alien's kill, went for the joiner). */
        uint32_t c = find_alien();
        if (c && rd(c + 0x70, out->pos, 12) && rd(c + 0x174, &out->yaw, 4)) {
            out->flags = ALX_FOUND;
            uint32_t loco = u32(c + 0x42A8); uint8_t vents = 0;
            if (loco && rd(loco + 0x1E3, &vents, 1) && vents) out->flags |= ALX_VENTS;
            rd(c + 0x42A4, &out->setup, 4);
            ok = 1;
        }
    }
    return ok;
}

void alien_from_host(const AlienExPkt *k) {
    if (!g_on || g_host) return;
    DWORD now = GetTickCount();
    EnterCriticalSection(&g_cs);
    g_theirs_any_t = now;
    if (k->seq - g_theirs.seq < 0x80000000u || now - g_theirs_t > 3000) {
        g_theirs = *k;
        if (k->flags & ALX_FOUND) { g_theirs_t = now; alien_track_push(&g_track, now, k->pos); }
        if (!g_kills_known) { g_kills_known = 1; g_kills_seen = k->kills; }
        else if (k->kills != g_kills_seen) { g_kills_seen = k->kills; InterlockedExchange(&g_kill_due, 1); }
    }
    LeaveCriticalSection(&g_cs);
}

void alien_partner_at(const float pos[3]) {
    if (!g_on || !g_host) return;
    DWORD now = GetTickCount();
    EnterCriticalSection(&g_cs);
    if (g_partner_t && now - g_partner_t > 0 && now - g_partner_t < 1000) {
        float dx = pos[0] - g_partner[0], dz = pos[2] - g_partner[2];
        float mps = sqrtf(dx * dx + dz * dz) * 1000.f / (float)(now - g_partner_t);
        g_partner_mps = g_partner_mps * 0.6f + mps * 0.4f;
    }
    memcpy(g_partner, pos, 12); g_partner_t = now;
    LeaveCriticalSection(&g_cs);
}

int alien_following(void) { return g_on && !g_host && g_pup != NULL; }

/* Tests only: AIC_DEBUG_SPAWN=<entity hex>:<instance hex> calls the generic "spawn" method on that script entity
   (a level's alien character) once, when alien_debug_arm() says so. Runs on the script thread (the call_method hook),
   the way OpenCAGE's AI_STATE::CallSpawnMethod does it: the entity manager's access lock, its lookup by handle, the
   queued method call, and the release of the counted pointer. Same addresses (Steam build). */
static volatile LONG g_dbg_armed;
void alien_debug_arm(void) { InterlockedExchange(&g_dbg_armed, 1); }
void alien_entity_tick(void) {
    if (!InterlockedExchange(&g_dbg_armed, 0)) return;
    char v[48]; unsigned ent = 0, inst = 0;
    if (!GetEnvironmentVariableA("AIC_DEBUG_SPAWN", v, sizeof v) || sscanf(v, "%x:%x", &ent, &inst) != 2) return;
    uintptr_t b = (uintptr_t)GetModuleHandleW(NULL);
    static const uint8_t s_lock[2] = {0x56, 0x68}, s_find[3] = {0x55, 0x8B, 0xEC}, s_rel[3] = {0x56, 0x8B, 0xF1};
    if (!bytes_ok(b + 0x51E1B0u, s_lock, 2) || !bytes_ok(b + 0x5236A0u, s_find, 3) || !bytes_ok(b + 0x5425A0u, s_find, 3) || !bytes_ok(b + 0x198230u, s_rel, 3)) {
        if (g_log) g_log("alien: debug spawn: functions don't match this build"); return;
    }
    typedef void(__attribute__((thiscall)) *acc_fn)(void *);
    typedef void **(__attribute__((thiscall)) *find_fn)(void *, void **, const uint32_t *);
    typedef void(__attribute__((thiscall)) *call_fn)(void *, void **, const uint32_t *, uint32_t);
    typedef void(__attribute__((thiscall)) *rel_fn)(void **);
    uint32_t mgr = u32(b + 0x134EF40u);
    if (!mgr) { if (g_log) g_log("alien: debug spawn: no entity manager"); return; }
    uint8_t access[8] = {0}; void *found = NULL;
    const uint32_t handle[2] = {ent, inst}, method = 0x38BD237Au;
    ((acc_fn)(b + 0x51E1B0u))(access);
    ((find_fn)(b + 0x5236A0u))((void *)(uintptr_t)mgr, &found, handle);
    int called = 0;
    if (found && u32((uintptr_t)found + 12)) {
        uint8_t tmp[0x40] __attribute__((aligned(8))); memset(tmp, 0, sizeof tmp);
        *(void **)(tmp + 0x24) = found;
        ((call_fn)(b + 0x5425A0u))(tmp, &found, &method, 0);
        called = 1;
    }
    if (found) ((rel_fn)(b + 0x198230u))(&found);
    ((acc_fn)(b + 0x51E1D0u))(access);
    if (g_log) g_log("alien: debug spawn %08x:%08x -> %s", ent, inst, called ? "spawn called" : "entity not found");
}
