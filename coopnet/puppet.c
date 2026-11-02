/* Partner body ("puppet"): the level-script mod (tools/levelmod) adds an actor that keeps walking to a PositionMarker
 * seeded with a magic position. Here we find every in-memory copy of that magic float3 and overwrite it with the
 * partner's foot position, so the actor walks to wherever the partner is.
 */
#include <windows.h>
#include <stdint.h>
#include <string.h>

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

/* Called from the net thread at 20 Hz. in_level: our camera is valid and not the menu camera. */
void puppet_tick(int in_level, int peer_live, const float peer_cam_pos[3], void (*logf)(const char *, ...)) {
    static int scanned_once;
    DWORD now = GetTickCount();
    if (!in_level) { g_nhits = 0; scanned_once = 0; return; }
    if (g_nhits == 0 && (!scanned_once || now - g_last_scan > 10000)) {
        g_last_scan = now; scanned_once = 1;
        puppet_scan(logf);
    }
    if (!g_nhits || !peer_live) return;
    /* partner's feet: camera position minus eye height (measured eye height ~1.65 m above the floor). */
    float t[3] = {peer_cam_pos[0], peer_cam_pos[1] - 1.65f, peer_cam_pos[2]};
    for (int i = 0; i < g_nhits; i++) {
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery(g_hits[i], &mi, sizeof mi) || mi.State != MEM_COMMIT || mi.Protect != PAGE_READWRITE) { g_nhits = 0; return; }
        memcpy(g_hits[i], t, 12);
    }
}
