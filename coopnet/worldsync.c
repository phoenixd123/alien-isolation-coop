/*
 * worldsync: share world changes between the two games by replaying level-script methods (effect capture, first
 * slice). Runs inside the call_method hook (game thread) for capture and through replay.c for apply.
 *
 * Doors (every story level uses the same Archetypes\Misc_Subarchetypes\Door_Package IDs):
 *   capture: set_true on the package's IsOpen LogicSwitch -> this door just opened here.
 *   apply:   request_open on the package's Door entity at the same path. The door opens and auto-closes like a
 *            normal door, and locked doors stay locked.
 * Echo guard: a door opened from a remote event is not reported back for a few seconds.
 */
#include <windows.h>
#include <stdint.h>
#include <string.h>
#include "replay.h"
#include "worldsync.h"

#define SG_SET_TRUE      0x5eb0e648u
#define SG_REQUEST_OPEN  0x8da169a3u
#define SG_LOGICSWITCH   0x0e423fccu
#define SG_DOOR_TYPE     0xaf6549bbu
#define DP_COMPOSITE     0x56d0e3beu   /* Archetypes\Misc_Subarchetypes\Door_Package */
#define DP_ISOPEN        0x95de1d55u   /* IsOpen (LogicSwitch) inside Door_Package */
#define DP_DOOR          0x11c160bdu   /* Door (Door) inside Door_Package */
#define ECHO_MS          4000

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
    DWORD now = GetTickCount(); int oldest = 0;
    EnterCriticalSection(&g_cs);
    for (int i = 0; i < 64; i++) {
        if (g_echo[i].h == h) { oldest = i; break; }
        if ((now - g_echo[i].t) > (now - g_echo[oldest].t)) oldest = i;
    }
    g_echo[oldest].h = h; g_echo[oldest].t = now;
    LeaveCriticalSection(&g_cs);
}

void ws_init(void (*logf)(const char *, ...)) {
    g_logf = logf;
    InitializeCriticalSection(&g_cs);
    g_on = 1;
    logf("worldsync: doors on");
}

/* Owner chain of an entity: path[0] = ID of the instance entity that owns it, then its owner, ... */
static int read_path(uint32_t ent, uint32_t *path, uint32_t *first_type) {
    uint32_t e[5];
    if (!rd(ent, e, sizeof e)) return 0;
    uint32_t hb = e[2]; int n = 0;
    while (hb && n < 6) {
        uint32_t b[4], p[5];
        if (!rd(hb, b, sizeof b) || !rd(b[3], p, sizeof p)) break;
        if (n == 0 && first_type) *first_type = p[4];
        path[n++] = p[3]; hb = p[2];
    }
    return n;
}

void ws_on_call(const uint32_t *msg, const uint32_t *target) {
    /* Hot path: the engine is about to use both pointers, so they are valid here. */
    if (!g_on || !msg || msg[0] != SG_SET_TRUE || !target || target[3] != DP_ISOPEN || target[4] != SG_LOGICSWITCH) return;
    WorldEvt ev; memset(&ev, 0, sizeof ev);
    uint32_t owner_type = 0;
    ev.npath = read_path((uint32_t)(uintptr_t)target, ev.path, &owner_type);
    if (ev.npath == 0 || owner_type != DP_COMPOSITE) return;   /* IsOpen of some other composite */
    uint32_t h = path_hash(ev.path, ev.npath);
    if (echo_recent(h)) return;
    ev.method = SG_REQUEST_OPEN; ev.eid = DP_DOOR; ev.type = SG_DOOR_TYPE;
    LONG w = g_out_w;
    if (w - g_out_r >= NOUT) return;
    g_out[w % NOUT] = ev;
    InterlockedIncrement(&g_out_w);
}

int ws_pop(WorldEvt *ev) {
    if (g_out_r == g_out_w) return 0;
    *ev = g_out[g_out_r % NOUT];
    InterlockedIncrement(&g_out_r);
    return 1;
}

void ws_apply(const WorldEvt *ev) {
    if (!g_on) return;
    if (ev->method != SG_REQUEST_OPEN || ev->eid != DP_DOOR) return;   /* only what we know how to apply */
    ReplayKey k; memset(&k, 0, sizeof k);
    k.method = ev->method; k.eid = ev->eid; k.type = ev->type; k.npath = ev->npath;
    memcpy(k.path, ev->path, (size_t)ev->npath * 4);
    echo_mark(path_hash(ev->path, ev->npath));
    replay_request(&k);
}
