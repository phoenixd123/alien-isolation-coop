/* Partner body ("puppet"). Characters added through the level file are never spawned by the engine (tested twice), so
 * the body is an NPC the level already has, and `levelmod drivenpc` adds next to it a PositionMarker seeded with a
 * magic position, CMD_GoTo -> marker, TriggerBindCharacter -> the NPC, and a 0.25 s LogicDelay loop (fixed entity IDs:
 * ShortGuids of their names). Tested 2026-11-04 in HAB_AIRPORT: after `spawn`, the NPC walked to each new position
 * written into the marker's parameter block in memory.
 * Here: when a level with a known body loads, move the body's spawn position (its parameter block) to the partner,
 * spawn it, start the loop, then keep writing the partner's feet into the marker.
 */
#include <windows.h>
#include <stdint.h>
#include <string.h>
#include "replay.h"

static const float MAGIC[3] = {1234.5f, 987.25f, 4321.75f};
#define MAXHITS 64
static float *g_hits[MAXHITS];
static int g_nhits;
static DWORD g_last_scan;

static int same3(const float *p, const float *q) { return p[0] == q[0] && p[1] == q[1] && p[2] == q[2]; }

/* Scan committed, writable, private memory for the magic triple. Returns number of hits. */
static int puppet_scan(void (*logf)(const char *, ...)) {
    g_nhits = 0;
    MEMORY_BASIC_INFORMATION mi;
    uint8_t *a = (uint8_t *)0x10000;
    while ((uintptr_t)a < 0x7FFE0000 && VirtualQuery(a, &mi, sizeof mi)) {
        uint8_t *base = (uint8_t *)mi.BaseAddress; SIZE_T sz = mi.RegionSize;
        if (mi.State == MEM_COMMIT && (mi.Protect == PAGE_READWRITE) && mi.Type == MEM_PRIVATE && sz < (256u << 20)) {
            const uint32_t m0 = *(const uint32_t *)&MAGIC[0];
            for (uint8_t *p = base; p + 12 <= base + sz && g_nhits < MAXHITS; p += 4) {
                if (*(const uint32_t *)p == m0 && same3((const float *)p, MAGIC)) g_hits[g_nhits++] = (float *)p;
            }
        }
        a = base + sz;
    }
    if (logf) {
        logf("puppet: scan found %d copies of the marker", g_nhits);
        for (int i = 0; i < g_nhits && i < 8; i++) logf("puppet:   copy at %p", (void *)g_hits[i]);
    }
    return g_nhits;
}

/* Bodies per level: the NPC's spawn position as the level file has it, the Character inside it, and the loop. */
typedef struct {
    const char *what; float spawn_pos[3];
    uint32_t chr_path[5]; int chr_npath;     /* path of the NPC's inner Character (NPC_Logic -> NPC_LOGIC_DUDE) */
    uint32_t loop_path[4]; int loop_npath;   /* path of the composite that holds COOP_Loop */
} Body;
static const Body BODIES[] = {
    { "HAB_AIRPORT M04_Pt01 scared_civvie_01", { -91.0351f, 15.9138f, 29.1772f },
      { 0xa958c068u, 0xbcd8075au, 0xfee8d709u, 0x59ba8d60u, 0x4af05bfeu }, 5,
      { 0xfee8d709u, 0x59ba8d60u, 0x4af05bfeu }, 3 },
};
#define SG_SPAWN      0x38bd237au
#define SG_TRIGGER    0xd9d0ca29u
#define E_NPC_DUDE    0xb360341cu
#define T_CHARACTER   0xc561798au
#define E_COOP_LOOP   0xa51f7823u
#define T_LOGICDELAY  0xf3f97946u

static float *find_triple(const float v[3]) {
    MEMORY_BASIC_INFORMATION mi;
    for (uint8_t *a = (uint8_t *)0x10000; (uintptr_t)a < 0x7FFE0000 && VirtualQuery(a, &mi, sizeof mi); a = (uint8_t *)mi.BaseAddress + mi.RegionSize) {
        uint8_t *base = (uint8_t *)mi.BaseAddress; SIZE_T sz = mi.RegionSize;
        if (mi.State != MEM_COMMIT || mi.Protect != PAGE_READWRITE || mi.Type != MEM_PRIVATE || sz >= (256u << 20)) continue;
        const uint32_t m0 = *(const uint32_t *)&v[0];
        for (uint8_t *p = base; p + 12 <= base + sz; p += 4)
            if (*(const uint32_t *)p == m0 && same3((const float *)p, v)) return (float *)p;
    }
    return NULL;
}

static void body_spawn(const float feet[3], void (*logf)(const char *, ...)) {
    for (unsigned b = 0; b < sizeof BODIES / sizeof BODIES[0]; b++) {
        const Body *B = &BODIES[b];
        float *sp = find_triple(B->spawn_pos);
        if (!sp) continue;   /* not this level */
        memcpy(sp, feet, 12);   /* spawn where the partner is */
        ReplayKey k; memset(&k, 0, sizeof k);
        k.method = SG_SPAWN; k.eid = E_NPC_DUDE; k.type = T_CHARACTER; k.npath = B->chr_npath; memcpy(k.path, B->chr_path, sizeof B->chr_path);
        replay_request(&k);
        memset(&k, 0, sizeof k);
        k.method = SG_TRIGGER; k.eid = E_COOP_LOOP; k.type = T_LOGICDELAY; k.npath = B->loop_npath; memcpy(k.path, B->loop_path, sizeof B->loop_path);
        replay_request(&k);
        if (logf) logf("puppet: body %s spawned at the partner (%.1f %.1f %.1f), walk loop started", B->what, feet[0], feet[1], feet[2]);
        return;
    }
    if (logf) logf("puppet: this level has no partner body set up");
}

/* Called from the net thread at 20 Hz. in_level: our camera is valid and not the menu camera. */
void puppet_tick(int in_level, int peer_live, const float peer_cam_pos[3], void (*logf)(const char *, ...)) {
    static int scanned_once, spawned;
    DWORD now = GetTickCount();
    if (!in_level) { g_nhits = 0; scanned_once = 0; spawned = 0; return; }
    if (g_nhits == 0 && (!scanned_once || now - g_last_scan > 10000)) {
        g_last_scan = now; scanned_once = 1;
        puppet_scan(logf);
    }
    if (!g_nhits || !peer_live || (peer_cam_pos[0] == 0.f && peer_cam_pos[2] == 0.f)) return;
    /* partner's feet: camera position minus eye height (measured eye height ~1.65 m above the floor). */
    float t[3] = {peer_cam_pos[0], peer_cam_pos[1] - 1.65f, peer_cam_pos[2]};
    if (!spawned) { spawned = 1; body_spawn(t, logf); }
    for (int i = 0; i < g_nhits; i++) {
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery(g_hits[i], &mi, sizeof mi) || mi.State != MEM_COMMIT || mi.Protect != PAGE_READWRITE) { g_nhits = 0; return; }
        memcpy(g_hits[i], t, 12);
    }
}
