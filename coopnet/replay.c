/*
 * replay: run a level-script method on an entity named by its level-file path, on the game thread.
 *
 * Layout (Steam build 4940658):
 *   entity object:  +8 = handle block of its owner (parent composite instance entity), +0xC = entity ID
 *                   (level-file ShortGuid), +0x10 = type ShortGuid.
 *   handle block:   16 bytes { vtable, refcount, flag, entity* }.
 *   message:        +0 method ShortGuid, +8 target handle block, +0xC sender, +0x20 0x80000000, +0x24 2;
 *                   call_method(manager, &msg.target, &msg.target, &msgptr).
 * Lookup: a worker thread scans readable memory for {id, type} at +0xC, checks the owner chain IDs against
 * the path, then finds the handle block that points at the entity. Results are cached per level session.
 * Injection: the call_method hook calls replay_pump() after each real dispatch returns, so replays run on
 * the game thread between messages.
 */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "replay.h"

typedef uint32_t(__attribute__((thiscall)) *cm_fn)(void *self, void *a1, void *a2, void *a3);

#define NREQ 64
typedef struct { volatile LONG state; ReplayKey k; uint32_t hb; } Req;   /* 0 free, 1 resolving, 2 ready */
static Req g_req[NREQ];
static HANDLE g_wake;
static void (*g_logf)(const char *, ...);
static volatile LONG g_in_replay;
static cm_fn g_hook;
void replay_set_hook(cm_fn hook) { g_hook = hook; }

static int rd(uintptr_t a, void *o, size_t n) {
    SIZE_T got = 0;
    return a > 0x10000 && a < 0x7FFF0000 && ReadProcessMemory(GetCurrentProcess(), (LPCVOID)a, o, n, &got) && got == n;
}

static int chain_ok(uint32_t ent, const ReplayKey *k) {
    uint32_t e[5];
    if (!rd(ent, e, sizeof e) || e[3] != k->eid || (k->type && e[4] != k->type)) return 0;
    uint32_t hb = e[2];
    for (int i = 0; i < k->npath; i++) {
        uint32_t b[4], p[5];
        if (!rd(hb, b, sizeof b) || !rd(b[3], p, sizeof p) || p[3] != k->path[i]) return 0;
        if (i == 0 && k->owner_ok && !k->owner_ok(p[4])) return 0;
        hb = p[2];
    }
    return 1;
}

/* Scan committed, readable private memory for pattern dwords at 4-byte alignment. */
typedef int (*hit_fn)(uint32_t addr, void *ctx);
static void scan(const uint32_t *pat, int npat, hit_fn fn, void *ctx) {
    static uint8_t buf[1 << 20];
    MEMORY_BASIC_INFORMATION m;
    for (uintptr_t a = 0x10000; a < 0x7FFF0000 && VirtualQuery((LPCVOID)a, &m, sizeof m); a = (uintptr_t)m.BaseAddress + m.RegionSize) {
        if (m.State != MEM_COMMIT || m.Type != MEM_PRIVATE || (m.Protect & (PAGE_GUARD | PAGE_NOACCESS)) ||
            !(m.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE))) continue;
        for (uintptr_t off = 0; off < m.RegionSize; off += sizeof buf - 64) {
            size_t n = m.RegionSize - off < sizeof buf ? m.RegionSize - off : sizeof buf;
            uintptr_t base = (uintptr_t)m.BaseAddress + off;
            SIZE_T got = 0;
            if (!ReadProcessMemory(GetCurrentProcess(), (LPCVOID)base, buf, n, &got) || got < (size_t)npat * 4) continue;
            const uint32_t *w = (const uint32_t *)buf;
            for (size_t i = 0; i + (size_t)npat <= got / 4; i++) {
                if (w[i] != pat[0] || (npat > 1 && w[i + 1] != pat[1])) continue;
                if (fn((uint32_t)(base + i * 4), ctx)) return;
            }
            if (n < sizeof buf) break;
        }
    }
}

typedef struct { const ReplayKey *k; uint32_t ent, hb; } Find;
static int on_ent(uint32_t addr, void *ctx) {
    Find *f = ctx; uint32_t ent = addr - 0xC;
    if (!chain_ok(ent, f->k)) return 0;
    f->ent = ent; return 1;
}
static int on_ref(uint32_t addr, void *ctx) {
    Find *f = ctx; uint32_t b[4];
    if (!rd(addr - 0xC, b, sizeof b) || b[3] != f->ent || b[1] == 0 || b[1] > 1000000 || b[0] < 0x400000 || b[0] > 0x2000000) return 0;
    f->hb = addr - 0xC; return 1;
}

uint32_t replay_resolve(const ReplayKey *k) {
    Find f = { k, 0, 0 };
    uint32_t pat[2] = { k->eid, k->type };
    scan(pat, k->type ? 2 : 1, on_ent, &f);
    if (!f.ent) return 0;
    scan(&f.ent, 1, on_ref, &f);
    return f.hb;
}

static DWORD WINAPI worker(LPVOID p) {
    (void)p;
    for (;;) {
        WaitForSingleObject(g_wake, 1000);
        for (int i = 0; i < NREQ; i++) {
            if (g_req[i].state != 1) continue;
            DWORD t = GetTickCount();
            uint32_t hb = replay_resolve(&g_req[i].k);
            if (g_logf) g_logf("replay: resolve %08x/%08x (%d up) -> hb %08x in %lu ms", g_req[i].k.eid, g_req[i].k.method,
                               g_req[i].k.npath, hb, GetTickCount() - t);
            if (!hb) { g_req[i].state = 0; continue; }
            g_req[i].hb = hb; InterlockedExchange(&g_req[i].state, 2);
        }
    }
    return 0;
}

void replay_init(void (*logf)(const char *, ...)) {
    g_logf = logf;
    g_wake = CreateEventA(NULL, FALSE, FALSE, NULL);
    CloseHandle(CreateThread(NULL, 0, worker, NULL, 0, NULL));
}

int replay_request(const ReplayKey *k) {
    for (int i = 0; i < NREQ; i++)
        if (InterlockedCompareExchange(&g_req[i].state, -1, 0) == 0) {
            g_req[i].k = *k; InterlockedExchange(&g_req[i].state, 1); SetEvent(g_wake); return 1;
        }
    return 0;
}

int replay_active(void) { return g_in_replay != 0; }

void replay_pump(void *self, cm_fn tramp, const void *cur_msg) {
    if (g_in_replay) return;
    for (int i = 0; i < NREQ; i++) {
        if (g_req[i].state != 2) continue;
        uint32_t m[10] = {0};
        m[0] = g_req[i].k.method; m[2] = g_req[i].hb;
        if (cur_msg) m[3] = ((const uint32_t *)cur_msg)[3];   /* sender: borrow the one of the message just dispatched */
        m[8] = 0x80000000u; m[9] = 2;
        void *pm = m;
        g_in_replay = 1;
        uint32_t r = (g_req[i].k.via_hook && g_hook ? g_hook : tramp)(self, &m[2], &m[2], &pm);
        g_in_replay = 0;
        if (g_logf) g_logf("replay: sent %08x to entity %08x (hb %08x) -> %u", m[0], g_req[i].k.eid, m[2], r & 0xFF);
        InterlockedExchange(&g_req[i].state, 0);
    }
}
