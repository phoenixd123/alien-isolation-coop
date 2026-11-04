/*
 * worldsync: share world changes between the two games by replaying level-script methods (effect capture).
 * Capture runs inside the call_method hook (game thread); apply goes through replay.c.
 *
 * Each rule: when `method` hits entity `eid` (of `type`) whose owner composite is `owner`, send the `apply`
 * methods for entities of the same composite instance (same path). The archetypes below have the same IDs in
 * every story level (checked with `levelmod ids` on all levels).
 *
 *   Doors (Archetypes\Misc_Subarchetypes\Door_Package): IsOpen.set_true -> Door.request_open. Opens and
 *     auto-closes like a normal door; locked doors stay locked.
 *   Pickups (Required_Assets\Pickups\pickup_common): SetPersistentIntegerValue.trigger (the item was collected)
 *     -> the same trigger (marks it taken, as a save would) and IntegerEquals_1.trigger (the check the game runs
 *     on load, which fires hide_pickup). The item disappears for the partner; they don't get a copy.
 *
 *   Story (the 35 SCRIPT_StoryMission composites): LogicOnce.trigger -> the same LogicOnce on the partner.
 *     Mission scripts gate their progress with LogicOnce nodes, which fire once by design, so replaying the
 *     one that just fired can't double-fire anything; the partner's own mission script continues from there.
 *     Entity wildcard (eid 0 = the captured entity); on apply, its owner must be a story composite.
 *
 * Only methods that appear as an `apply` here are accepted from the network.
 * Echo guard: a path changed by a remote event is not reported back for a few seconds.
 */
#include <windows.h>
#include <stdint.h>
#include <string.h>
#include "replay.h"
#include "worldsync.h"

#define SG_TRIGGER       0xd9d0ca29u
#define SG_SET_TRUE      0x5eb0e648u
#define SG_REQUEST_OPEN  0x8da169a3u
#define T_LOGICSWITCH    0x0e423fccu
#define T_DOOR           0xaf6549bbu
#define T_SETINTEGER     0x02a3673bu
#define T_INTEGEREQUALS  0xd2590676u
#define T_LOGICONCE      0x936b070fu
#define OWNER_STORY      0xFFFFFFFFu   /* any composite in STORY[] */
#define ECHO_MS          4000

typedef struct { uint32_t method, eid, type; } Act;   /* eid 0: the captured entity itself */

/* SCRIPT_StoryMission composite GUIDs, from `levelmod ids` over every level. */
static const uint32_t STORY[] = {
    0x054b0197u, 0x096564b1u, 0x09eaf288u, 0x194c9838u, 0x1a02de9cu, 0x1ae88077u,
    0x216596efu, 0x273f61e2u, 0x2a51375du, 0x37c5b44au, 0x404ea802u, 0x469108d2u,
    0x4d193eaeu, 0x4fc3d84fu, 0x571ec324u, 0x5cc5e3d2u, 0x66c34960u, 0x7295b688u,
    0x790f71fdu, 0x79a53d9cu, 0x8cc8d80eu, 0x950a3824u, 0x9c702d43u, 0xa579b9adu,
    0xabc4eb1du, 0xb2632a3du, 0xb4d89c0du, 0xbc974a05u, 0xc525bfa4u, 0xcd1fe962u,
    0xcebfc559u, 0xd74ffa24u, 0xe33271c9u, 0xee20b383u, 0xf4f69debu
};
static int is_story(uint32_t c) {
    for (unsigned i = 0; i < sizeof STORY / sizeof STORY[0]; i++) if (STORY[i] == c) return 1;
    return 0;
}
typedef struct { const char *name; uint32_t owner; Act when; Act apply[3]; } Rule;

static const Rule RULES[] = {
    { "door", 0x56d0e3beu, { SG_SET_TRUE, 0x95de1d55u, T_LOGICSWITCH },
      { { SG_REQUEST_OPEN, 0x11c160bdu, T_DOOR } } },
    { "pickup", 0x65c68783u, { SG_TRIGGER, 0xd74757b0u, T_SETINTEGER },
      { { SG_TRIGGER, 0xd74757b0u, T_SETINTEGER }, { SG_TRIGGER, 0xc29ee5fdu, T_INTEGEREQUALS } } },
    { "story", OWNER_STORY, { SG_TRIGGER, 0, T_LOGICONCE },
      { { SG_TRIGGER, 0, T_LOGICONCE } } },
};
#define NRULES (int)(sizeof RULES / sizeof RULES[0])

static void (*g_logf)(const char *, ...);
static int g_on;

#define NOUT 256
static WorldEvt g_out[NOUT];
static volatile LONG g_out_w, g_out_r;

typedef struct { uint32_t h; DWORD t; } Echo;
static Echo g_echo[64];
static CRITICAL_SECTION g_cs;

static int rd(uintptr_t a, void *o, size_t n) {
    SIZE_T got = 0;
    return a > 0x10000 && a < 0x7FFF0000 && ReadProcessMemory(GetCurrentProcess(), (LPCVOID)a, o, n, &got) && got == n;
}

static uint32_t path_hash(const uint32_t *p, int n) {
    uint32_t h = 2166136261u;
    for (int i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; }
    return h;
}

static int echo_recent(uint32_t h) {
    DWORD now = GetTickCount(); int hit = 0;
    EnterCriticalSection(&g_cs);
    for (int i = 0; i < 64; i++) if (g_echo[i].h == h && now - g_echo[i].t < ECHO_MS) { hit = 1; break; }
    LeaveCriticalSection(&g_cs);
    return hit;
}
static void echo_mark(uint32_t h) {
    DWORD now = GetTickCount(); int slot = 0;
    EnterCriticalSection(&g_cs);
    for (int i = 0; i < 64; i++) {
        if (g_echo[i].h == h) { slot = i; break; }
        if ((now - g_echo[i].t) > (now - g_echo[slot].t)) slot = i;
    }
    g_echo[slot].h = h; g_echo[slot].t = now;
    LeaveCriticalSection(&g_cs);
}

void ws_init(void (*logf)(const char *, ...)) {
    g_logf = logf;
    InitializeCriticalSection(&g_cs);
    g_on = 1;
    logf("worldsync: %d rules (doors, pickups, story LogicOnce in %d mission composites)", NRULES, (int)(sizeof STORY / sizeof STORY[0]));
}

/* Owner chain of an entity: path[0] = ID of the instance entity that owns it, then its owner, ... */
static int read_path(uint32_t ent, uint32_t *path, uint32_t *owner_type) {
    uint32_t e[5];
    if (!rd(ent, e, sizeof e)) return 0;
    uint32_t hb = e[2]; int n = 0;
    while (hb && n < 6) {
        uint32_t b[4], p[5];
        if (!rd(hb, b, sizeof b) || !rd(b[3], p, sizeof p)) break;
        if (n == 0 && owner_type) *owner_type = p[4];
        path[n++] = p[3]; hb = p[2];
    }
    return n;
}

static void push(const WorldEvt *ev) {
    LONG w = g_out_w;
    if (w - g_out_r >= NOUT) return;
    g_out[w % NOUT] = *ev;
    InterlockedIncrement(&g_out_w);
}

void ws_on_call(const uint32_t *msg, const uint32_t *target) {
    /* Hot path: the engine is about to use both pointers, so they are valid here. */
    if (!g_on || !msg || !target) return;
    uint32_t m = msg[0], eid = target[3], type = target[4];
    for (int r = 0; r < NRULES; r++) {
        const Rule *R = &RULES[r];
        if (m != R->when.method || (R->when.eid && eid != R->when.eid) || type != R->when.type) continue;
        WorldEvt ev; memset(&ev, 0, sizeof ev);
        uint32_t owner = 0;
        ev.npath = read_path((uint32_t)(uintptr_t)target, ev.path, &owner);
        if (ev.npath == 0 || (R->owner == OWNER_STORY ? !is_story(owner) : owner != R->owner)) return;
        if (echo_recent(path_hash(ev.path, ev.npath))) return;
        for (int a = 0; a < 3 && R->apply[a].method; a++) {
            ev.method = R->apply[a].method; ev.eid = R->apply[a].eid ? R->apply[a].eid : eid; ev.type = R->apply[a].type;
            push(&ev);
        }
        return;
    }
}

int ws_pop(WorldEvt *ev) {
    if (g_out_r == g_out_w) return 0;
    *ev = g_out[g_out_r % NOUT];
    InterlockedIncrement(&g_out_r);
    return 1;
}

void ws_apply(const WorldEvt *ev) {
    if (!g_on) return;
    int ok = 0, story = 0;
    for (int r = 0; r < NRULES && !ok; r++)
        for (int a = 0; a < 3 && RULES[r].apply[a].method; a++) {
            const Act *A = &RULES[r].apply[a];
            if (A->method == ev->method && (A->eid ? A->eid == ev->eid : ev->eid != 0) && A->type == ev->type) {
                ok = 1; story = RULES[r].owner == OWNER_STORY; break;
            }
        }
    if (!ok) { g_logf("worldsync: ignored an event that matches no rule (%08x on %08x)", ev->method, ev->eid); return; }
    ReplayKey k; memset(&k, 0, sizeof k);
    k.method = ev->method; k.eid = ev->eid; k.type = ev->type; k.npath = ev->npath;
    memcpy(k.path, ev->path, (size_t)ev->npath * 4);
    if (story) k.owner_ok = is_story;
    echo_mark(path_hash(ev->path, ev->npath));
    replay_request(&k);
}
