/*
 * worldsync: share world changes between the two games by replaying level-script methods (effect capture).
 * Capture runs inside the call_method hook (game thread); apply goes through replay.c.
 *
 * Each rule: when `method` hits entity `eid` (of `type`) whose owner composite is `owner`, send the `apply`
 * methods for entities of the same composite instance (same path). The archetypes below have the same IDs in
 * every story level (checked with `levelmod ids` on all levels).
 *
 *   Doors (Archetypes\Misc_Subarchetypes\Door_Package): IsOpen.set_true -> Door.request_open. Opens and
 *     auto-closes like a normal door; a locked door stays locked until one of you unlocks it (see Locks).
 *   Pickups (Required_Assets\Pickups\pickup_common): SetPersistentIntegerValue.trigger (the item was collected)
 *     -> the same trigger (marks it taken, as a save would) and IntegerEquals_1.trigger (the check the game runs
 *     on load, which fires hide_pickup). The item disappears for the partner; they don't get a copy.
 *
 *   Keycards (Required_Assets\Pickups\keycard): SetPlayerHasKeycard.trigger -> the same: the partner gets the card.
 *   Tools (Required_Assets\Archetypes\Equipment\Cutting_Tool): SetPlayerHasGatingTool.trigger -> the same.
 *   Alien (host only): NPC_WithdrawAlien / NPC_AlienConfig / NPC_SetAlienDevelopmentStage triggers in mission
 *     scripts and Archetypes\Gameplay\BackstagePlayerDetection -> the same on the joiner. The host's alien
 *     director decides when the alien comes out, retreats into the vents or changes behaviour; the joiner's alien
 *     follows. Joiners never send these and the host never accepts them.
 *
 *   Locks (Archetypes\Misc_Subarchetypes\Door_Mechanism): IsLocked.set_false -> the same. Every way a door gets
 *     unlocked ends here: hacking, keycard, keycode, lever, button. So a door one player unlocks is unlocked for both.
 *   Cutting (Door_Mechanism IsCuttingCompleted, persistent): covered by the automatic STATE kind below.
 *   Level change (Archetypes\Script\Mission\SwitchLevel): SwitchLevel_1.trigger -> the same. Every lift, tram and
 *     mission exit that loads another level goes through this script, so when one player leaves the level the other
 *     loads the same one. After a level change, more level changes are ignored for a while so it can't bounce back.
 *
 *   Story (the 35 SCRIPT_StoryMission composites): LogicOnce.trigger -> the same LogicOnce on the partner.
 *     Mission scripts gate their progress with LogicOnce nodes, which fire once by design, so replaying the
 *     one that just fired can't double-fire anything; the partner's own mission script continues from there.
 *     Entity wildcard (eid 0 = the captured entity); on apply, its owner must be a story composite.
 *
 * Automatic (syncmap.h, generated from every level's scripts by tools/syncmap): on top of the rules above, any
 *   level-script entity of these two kinds is shared, in every level, with no per-object rule:
 *     STATE  a persistent VariableBool / LogicSwitch (what a save keeps: cut doors, power, puzzle results, story
 *            flags): set_true / set_false -> the same.
 *     ONCE   a LogicOnce nothing resets (one-time story steps, cutscene starts, scripted events, puzzle
 *            completions): trigger -> the same. It fires at most once on each side.
 *   Player-owned composites (thrown items, weapons, cameras, NPC brains, UI, hiding) are left out by the generator.
 *
 * Only methods that appear as an `apply` here, or entities in syncmap.h, are accepted from the network.
 * Echo guard: an entity changed by a remote event is not reported back for a few seconds.
 */
#include <windows.h>
#include <stdint.h>
#include <string.h>
#include "replay.h"
#include "worldsync.h"
#include "syncmap.h"

#define SG_TRIGGER       0xd9d0ca29u
#define SG_SET_TRUE      0x5eb0e648u
#define SG_REQUEST_OPEN  0x8da169a3u
#define SG_SET_FALSE     0xd2b3cc2bu
#define T_SWITCHLEVEL    0xac1aa738u
#define C_DOOR_MECHANISM 0x143a5b75u   /* Archetypes\Misc_Subarchetypes\Door_Mechanism */
#define C_SWITCHLEVEL    0xef3722dcu   /* Archetypes\Script\Mission\SwitchLevel */
#define LEVEL_COOLDOWN_MS 20000
#define T_LOGICSWITCH    0x0e423fccu
#define T_VARBOOL        0x0e1cb504u
#define T_DOOR           0xaf6549bbu
#define T_SETINTEGER     0x02a3673bu
#define T_INTEGEREQUALS  0xd2590676u
#define T_LOGICONCE      0x936b070fu
#define T_SETKEYCARD     0xa8f550eau
#define T_SETGATINGTOOL  0xaa1db15du
#define T_WITHDRAWALIEN  0xa8db1715u
#define T_ALIENCONFIG    0x6bf2edeeu
#define T_ALIENSTAGE     0xefdb8f4du
#define C_BACKSTAGE      0x71602656u   /* Archetypes\Gameplay\BackstagePlayerDetection */
#define OWNER_STORY      0xFFFFFFFFu   /* any composite in STORY[] */
#define OWNER_ALIEN      0xFFFFFFFEu   /* STORY[] or BackstagePlayerDetection */
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
static int is_alien_owner(uint32_t c) { return c == C_BACKSTAGE || is_story(c); }
static int is_story_e(uint32_t c, uint32_t eid) { (void)eid; return is_story(c); }
static int is_alien_owner_e(uint32_t c, uint32_t eid) { (void)eid; return is_alien_owner(c); }
static int owner_match(uint32_t want, uint32_t c) {
    return want == OWNER_STORY ? is_story(c) : want == OWNER_ALIEN ? is_alien_owner(c) : c == want;
}
typedef struct { const char *name; uint32_t owner; Act when; Act apply[3]; int host_only; int level; } Rule;

static const Rule RULES[] = {
    { "door", 0x56d0e3beu, { SG_SET_TRUE, 0x95de1d55u, T_LOGICSWITCH },
      { { SG_REQUEST_OPEN, 0x11c160bdu, T_DOOR } }, 0, 0 },
    { "pickup", 0x65c68783u, { SG_TRIGGER, 0xd74757b0u, T_SETINTEGER },
      { { SG_TRIGGER, 0xd74757b0u, T_SETINTEGER }, { SG_TRIGGER, 0xc29ee5fdu, T_INTEGEREQUALS } }, 0, 0 },
    { "story", OWNER_STORY, { SG_TRIGGER, 0, T_LOGICONCE },
      { { SG_TRIGGER, 0, T_LOGICONCE } }, 0, 0 },
    { "keycard", 0xf9f99532u, { SG_TRIGGER, 0x9107fd88u, T_SETKEYCARD },
      { { SG_TRIGGER, 0x9107fd88u, T_SETKEYCARD } }, 0, 0 },
    { "tool", 0x3c207e36u, { SG_TRIGGER, 0x85436974u, T_SETGATINGTOOL },
      { { SG_TRIGGER, 0x85436974u, T_SETGATINGTOOL } }, 0, 0 },
    { "alien-withdraw", OWNER_ALIEN, { SG_TRIGGER, 0, T_WITHDRAWALIEN },
      { { SG_TRIGGER, 0, T_WITHDRAWALIEN } }, 1, 0 },
    { "alien-config", OWNER_ALIEN, { SG_TRIGGER, 0, T_ALIENCONFIG },
      { { SG_TRIGGER, 0, T_ALIENCONFIG } }, 1, 0 },
    { "alien-stage", OWNER_STORY, { SG_TRIGGER, 0, T_ALIENSTAGE },
      { { SG_TRIGGER, 0, T_ALIENSTAGE } }, 1, 0 },
    { "unlock", C_DOOR_MECHANISM, { SG_SET_FALSE, 0x09de34aeu, T_LOGICSWITCH },
      { { SG_SET_FALSE, 0x09de34aeu, T_LOGICSWITCH } }, 0, 0 },
    { "level", C_SWITCHLEVEL, { SG_TRIGGER, 0x92d9160au, T_SWITCHLEVEL },
      { { SG_TRIGGER, 0x92d9160au, T_SWITCHLEVEL } }, 0, 1 },
};
#define NRULES (int)(sizeof RULES / sizeof RULES[0])

static void (*g_logf)(const char *, ...);
static int g_on, g_host;
static volatile LONG g_level_t;   /* tick of the last level change sent or applied (0: none) */
static int level_cooling(void) { LONG t = g_level_t; return t && (DWORD)(GetTickCount() - (DWORD)t) < LEVEL_COOLDOWN_MS; }
void ws_set_host(int host) { g_host = host; }

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
/* Echo key: the entity itself (path + ID), so other changes in the same composite still go out. */
static uint32_t ent_key(const uint32_t *p, int n, uint32_t eid) { uint32_t h = path_hash(p, n) ^ eid; h *= 16777619u; return h ? h : 1; }

/* Which automatic kind a message is: 1 STATE (set_true/set_false on a bool or switch), 2 ONCE (trigger on a LogicOnce). */
static int generic_kind(uint32_t m, uint32_t type) {
    if (m == SG_TRIGGER && type == T_LOGICONCE) return 2;
    if ((m == SG_SET_TRUE || m == SG_SET_FALSE) && (type == T_LOGICSWITCH || type == T_VARBOOL)) return 1;
    return 0;
}
static int generic_owner_ok(uint32_t owner, uint32_t eid) { return syncmap_kind(owner, eid) != 0; }

/* ONCE events already sent recently (a LogicOnce keeps receiving trigger after it has fired). */
typedef struct { uint32_t k; DWORD t; } Sent;
static Sent g_sent[256]; static int g_sent_i;
static int sent_recently(uint32_t k) {
    DWORD now = GetTickCount();
    for (int i = 0; i < 256; i++) if (g_sent[i].k == k && now - g_sent[i].t < 30000) return 1;
    g_sent[g_sent_i].k = k; g_sent[g_sent_i].t = now; g_sent_i = (g_sent_i + 1) & 255;
    return 0;
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
    logf("worldsync: %d rules (doors, locks, pickups, keycards, tools, level changes, alien director [host], story LogicOnce in %d mission composites) "
         "+ %d automatic entities from every level (persistent state and one-time steps)",
         NRULES, (int)(sizeof STORY / sizeof STORY[0]), SYNCMAP_N);
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
        if (R->host_only && !g_host) return;
        WorldEvt ev; memset(&ev, 0, sizeof ev);
        uint32_t owner = 0;
        ev.npath = read_path((uint32_t)(uintptr_t)target, ev.path, &owner);
        if (ev.npath == 0 || !owner_match(R->owner, owner)) continue;   /* another rule may share the trigger */
        if (echo_recent(ent_key(ev.path, ev.npath, eid))) return;
        if (R->level) { if (level_cooling()) return; InterlockedExchange(&g_level_t, (LONG)(GetTickCount() | 1)); }
        for (int a = 0; a < 3 && R->apply[a].method; a++) {
            ev.method = R->apply[a].method; ev.eid = R->apply[a].eid ? R->apply[a].eid : eid; ev.type = R->apply[a].type;
            push(&ev);
        }
        return;
    }
    /* automatic: anything in syncmap.h */
    int gk = generic_kind(m, type);
    if (!gk) return;
    WorldEvt ev; memset(&ev, 0, sizeof ev);
    uint32_t owner = 0;
    ev.npath = read_path((uint32_t)(uintptr_t)target, ev.path, &owner);
    if (ev.npath == 0 || syncmap_kind(owner, eid) != gk) return;
    uint32_t key = ent_key(ev.path, ev.npath, eid);
    if (echo_recent(key)) return;
    if (gk == 2 && sent_recently(key ^ m)) return;
    ev.method = m; ev.eid = eid; ev.type = type;
    push(&ev);
}

int ws_pop(WorldEvt *ev) {
    if (g_out_r == g_out_w) return 0;
    *ev = g_out[g_out_r % NOUT];
    InterlockedIncrement(&g_out_r);
    return 1;
}

void ws_apply(const WorldEvt *ev) {
    if (!g_on) return;
    int ok = 0, level = 0, generic = 0; uint32_t owner = 0, when_eid = ev->eid;
    for (int r = 0; r < NRULES && !ok; r++) {
        if (RULES[r].host_only && g_host) continue;   /* the host's alien is the real one */
        for (int a = 0; a < 3 && RULES[r].apply[a].method; a++) {
            const Act *A = &RULES[r].apply[a];
            if (A->method == ev->method && (A->eid ? A->eid == ev->eid : ev->eid != 0) && A->type == ev->type) {
                ok = 1; owner = RULES[r].owner; level = RULES[r].level;
                if (RULES[r].when.eid) when_eid = RULES[r].when.eid;   /* echo on what the partner's capture will see */
                break;
            }
        }
    }
    if (!ok && generic_kind(ev->method, ev->type) && ev->eid) ok = generic = 1;
    if (!ok) { g_logf("worldsync: ignored an event that matches no rule (%08x on %08x)", ev->method, ev->eid); return; }
    if (level) {
        if (level_cooling()) { g_logf("worldsync: level change ignored (one just happened)"); return; }
        InterlockedExchange(&g_level_t, (LONG)(GetTickCount() | 1));
        g_logf("worldsync: partner is changing level, following");
    }
    ReplayKey k; memset(&k, 0, sizeof k);
    k.method = ev->method; k.eid = ev->eid; k.type = ev->type; k.npath = ev->npath;
    memcpy(k.path, ev->path, (size_t)ev->npath * 4);
    if (generic) k.owner_ok = generic_owner_ok;
    else if (owner == OWNER_STORY) k.owner_ok = is_story_e;
    else if (owner == OWNER_ALIEN) k.owner_ok = is_alien_owner_e;
    echo_mark(ent_key(ev->path, ev->npath, when_eid));
    replay_request(&k);
}
