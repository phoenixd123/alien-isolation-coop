/* Research probe (opt-in with AIC_PROBE=1): hooks CATHODE::EntityManager::call_method (RVA 0x526680, thiscall,
 * 3 stack args, ret 0Ch) and records each distinct (target class, message class) pair with a count, resolved
 * through MSVC RTTI. Layout of the arguments (smart-pointer slots) follows coop-anything PR #156 notes:
 *   target object = *(*(a1) + 0xC), message type word = *(*(a3) + 0x8) (a vtable address), message object +0xC.
 * Dumps new pairs to coop_logs\probe_<profile>_<pid>.log every few seconds.
 */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "replay.h"
#include "worldsync.h"

static int g_sync;   /* world sync (worldsync.c) on; AIC_SYNC=0 turns it off */

/* Co-op death (life.c): coopnet.c decides whether a GameOver trigger runs now or is held back. */
#define SG_TRIGGER_  0xd9d0ca29u
#define T_GAMEOVER   0x2f97adeeu
static int (*g_gameover_cb)(uint32_t hb, const uint32_t *msg);
void probe_set_gameover_cb(int (*cb)(uint32_t hb, const uint32_t *msg)) { g_gameover_cb = cb; }

#define RVA_CALL_METHOD 0x526680u
static const uint8_t CM_PROLOGUE[10] = {0x55, 0x8B, 0xEC, 0x6A, 0xFF, 0x68}; /* first 6 checked; imm32 is relocated */

typedef uint32_t(__attribute__((thiscall)) *cm_fn)(void *self, void *a1, void *a2, void *a3);
static cm_fn g_cm_tramp;
static FILE *g_plog;

typedef struct { uint32_t tvt, mvt, count, reported; uint32_t last_target, last_self; } Pair;
#define NPAIR 8192
static Pair g_pairs[NPAIR];
static volatile LONG g_plock;
static volatile LONG g_calls;

static int rd(uintptr_t a, void *o, size_t n) {
    SIZE_T got = 0;
    return a > 0x10000 && a < 0x7FFF0000 && ReadProcessMemory(GetCurrentProcess(), (LPCVOID)a, o, n, &got) && got == n;
}
static uint32_t rd32(uintptr_t a) { uint32_t v = 0; rd(a, &v, 4); return v; }

/* MSVC RTTI: vtable[-1] -> CompleteObjectLocator; COL+0xC -> TypeDescriptor; name at TD+8 (".?AVName@NS@@"). */
static void rtti_name(uint32_t vt, char *out, size_t n) {
    out[0] = 0;
    if (!vt) { snprintf(out, n, "null"); return; }
    uint32_t col = rd32(vt - 4); if (!col) { snprintf(out, n, "vt_%08x", vt); return; }
    uint32_t td = rd32(col + 0xC); if (!td) { snprintf(out, n, "vt_%08x", vt); return; }
    char raw[96] = {0};
    if (!rd(td + 8, raw, sizeof raw - 1) || raw[0] != '.') { snprintf(out, n, "vt_%08x", vt); return; }
    raw[sizeof raw - 1] = 0;
    /* ".?AVSetPosition@CATHODE@@" -> "CATHODE::SetPosition" (good enough for logs) */
    char *s = raw + 4, *at = strchr(s, '@');
    if (at) { *at = 0; char *ns = at + 1, *at2 = strchr(ns, '@'); if (at2) *at2 = 0;
              snprintf(out, n, "%s%s%s", ns[0] ? ns : "", ns[0] ? "::" : "", s); }
    else snprintf(out, n, "%s", s);
}

static Pair *record(void *self, void *a1, void *a3) {
    uint32_t c1 = rd32((uintptr_t)a1), c3 = rd32((uintptr_t)a3);
    uint32_t target = c1 ? rd32(c1 + 0xC) : 0;
    uint32_t tvt = target ? rd32(target) : 0;
    /* Ghidra (caller at RVA 0x526A60): the message's first dword is the method name's ShortGuid
       (compared with the atoms proxy_enable / proxy_disable / callback); +8 is the target handle, not a vtable. */
    uint32_t mvt = c3 ? rd32(c3 + 0x0) : 0;
    uint32_t h = (tvt * 2654435761u) ^ (mvt * 40503u);
    Pair *hit = NULL;
    while (InterlockedExchange(&g_plock, 1)) YieldProcessor();
    for (uint32_t i = 0; i < NPAIR; i++) {
        Pair *p = &g_pairs[(h + i) & (NPAIR - 1)];
        if (p->count && p->tvt == tvt && p->mvt == mvt) { p->count++; p->last_target = target; p->last_self = (uint32_t)(uintptr_t)self; hit = p; break; }
        if (!p->count) { p->tvt = tvt; p->mvt = mvt; p->count = 1; p->last_target = target; p->last_self = (uint32_t)(uintptr_t)self; hit = p; break; }
    }
    InterlockedExchange(&g_plock, 0);
    return hit;
}

/* Sampling mode (AIC_PROBE=2): for the first SAMPLES_PER calls of each (target, message) pair, keep the raw
 * dwords each argument points to, and one level deeper, so IDs can be located offline (tools/re/match_ids.py). */
#define SAMPLES_PER 3
#define NSAMPLE 3000
/* l1[k] = dwords at arg k; l2[k][j] = dwords at l1[k][j]; tobj = the target entity object (*(*a1 + 0xC)). */
/* chain[i] = first 8 dwords of the object reached by following +8 i+1 times from the target (owner instances). */
typedef struct { uint32_t self, arg[3], tvt, mvt, l1[3][8], l2[3][8][32], tobj[64], chain[8][8]; } Sample;
static Sample *g_samp;
static volatile LONG g_nsamp, g_nsamp_done;
static LONG g_nsamp_written;
static int g_mode;
static char g_spath[MAX_PATH + 64];
static void sample(void *self, void *a1, void *a2, void *a3, const Pair *p) {
    if (!g_samp || p->count > SAMPLES_PER) return;
    LONG i = InterlockedIncrement(&g_nsamp) - 1;
    if (i >= NSAMPLE) return;
    Sample *s = &g_samp[i];
    s->self = (uint32_t)(uintptr_t)self; s->arg[0] = (uint32_t)(uintptr_t)a1; s->arg[1] = (uint32_t)(uintptr_t)a2; s->arg[2] = (uint32_t)(uintptr_t)a3;
    s->tvt = p->tvt; s->mvt = p->mvt;
    for (int k = 0; k < 3; k++) {
        rd(s->arg[k], s->l1[k], sizeof s->l1[k]);
        for (int j = 0; j < 8; j++) rd(s->l1[k][j], s->l2[k][j], sizeof s->l2[k][j]);
    }
    rd(s->l2[0][0][3], s->tobj, sizeof s->tobj);
    uint32_t up = s->tobj[2];
    for (int c = 0; c < 8 && up; c++) { if (!rd(up, s->chain[c], sizeof s->chain[c])) break; up = s->chain[c][2]; }
    InterlockedIncrement(&g_nsamp_done);
}

/* Trace mode (AIC_PROBE=3): log each distinct (method, target object) once, with the target's entity ID (+0xC),
 * type (+0x10) and the IDs of its owner chain (+8 links). New lines appear when something new happens in the
 * world (a door opens, an item is taken), which is what effect capture has to send. */
typedef struct { uint32_t t, method, eid, type, path[6], ptype[6]; } Event;
#define NEV 65536
#define NSEEN (1 << 18)
static Event *g_ev; static volatile LONG g_nev; static LONG g_nev_written;
static uint64_t *g_seen;
static char g_epath[MAX_PATH + 64];
static void trace(void *a1, void *a3) {
    uint32_t c1 = rd32((uintptr_t)a1), c3 = rd32((uintptr_t)a3);
    uint32_t obj = c1 ? rd32(c1 + 0xC) : 0, method = c3 ? rd32(c3) : 0;
    if (!obj) return;
    uint64_t key = ((uint64_t)method << 32) ^ obj, h = (key * 0x9E3779B97F4A7C15ull) >> 46;
    while (InterlockedExchange(&g_plock, 1)) YieldProcessor();
    int fresh = 0;
    for (uint32_t i = 0; i < 64; i++) {
        uint64_t *slot = &g_seen[(h + i) & (NSEEN - 1)];
        if (*slot == key + 1) break;
        if (!*slot) { *slot = key + 1; fresh = 1; break; }
    }
    InterlockedExchange(&g_plock, 0);
    if (!fresh) return;
    LONG n = InterlockedIncrement(&g_nev) - 1;
    if (n >= NEV) return;
    Event *e = &g_ev[n];
    uint32_t w[5] = {0};
    rd(obj, w, sizeof w);
    e->method = method; e->eid = w[3]; e->type = w[4];
    /* entity +8 -> owner instance; owner +0xC -> the entity that instances it (its +0xC is a level-file entity ID) */
    uint32_t up = w[2];
    for (int c = 0; c < 6; c++) {
        uint32_t o[4] = {0}, u[5] = {0};
        if (!up || !rd(up, o, sizeof o) || !o[3] || !rd(o[3], u, sizeof u)) { e->path[c] = e->ptype[c] = 0; up = 0; continue; }
        e->path[c] = u[3]; e->ptype[c] = u[4]; up = u[2];
    }
    e->t = GetTickCount();
}

static uint32_t __attribute__((thiscall)) cm_hook(void *self, void *a1, void *a2, void *a3) {
    InterlockedIncrement(&g_calls);
    Pair *p = g_mode ? record(self, a1, a3) : NULL;
    if (g_mode == 2 && p) sample(self, a1, a2, a3, p);
    if (g_mode == 3) trace(a1, a3);
    if (g_sync) {
        const uint32_t *msg = *(const uint32_t **)a3, *hb = *(const uint32_t **)a1;
        const uint32_t *target = hb ? (const uint32_t *)(uintptr_t)hb[3] : NULL;
        if (msg && target && msg[0] == SG_TRIGGER_ && target[4] == T_GAMEOVER && !replay_active() && g_gameover_cb &&
            g_gameover_cb((uint32_t)(uintptr_t)hb, msg))
            return 1;   /* held: life.c runs it later through replay_post_raw */
        ws_on_call(msg, target);
    }
    uint32_t r = g_cm_tramp(self, a1, a2, a3);
    replay_pump(self, g_cm_tramp, (const void *)rd32((uintptr_t)a3));
    return r;
}

static void probe_dump(void) {
    if (!g_plog) return;
    static Pair snap[NPAIR];
    while (InterlockedExchange(&g_plock, 1)) YieldProcessor();
    memcpy(snap, g_pairs, sizeof snap);
    for (int i = 0; i < NPAIR; i++) if (g_pairs[i].count) g_pairs[i].reported = g_pairs[i].count;
    InterlockedExchange(&g_plock, 0);
    int fresh = 0;
    for (int i = 0; i < NPAIR; i++) {
        Pair *p = &snap[i];
        if (!p->count || p->reported) continue;
        char tn[128], mn[128]; rtti_name(p->tvt, tn, sizeof tn); rtti_name(p->mvt, mn, sizeof mn);
        fprintf(g_plog, "NEW target=%s msg=%s count=%u target_obj=%08x self=%08x tvt=%08x mvt=%08x\n",
                tn, mn, p->count, p->last_target, p->last_self, p->tvt, p->mvt);
        fresh++;
    }
    fprintf(g_plog, "-- calls=%ld new_pairs=%d samples=%ld\n", g_calls, fresh, g_nsamp_done);
    fflush(g_plog);
    /* write finished samples (all started ones are done when the two counters agree) */
    LONG n = g_nsamp; if (n > NSAMPLE) n = NSAMPLE;
    if (g_samp && n > g_nsamp_written && g_nsamp_done >= n) {
        FILE *f = fopen(g_spath, "ab");
        if (f) { fwrite(&g_samp[g_nsamp_written], sizeof(Sample), (size_t)(n - g_nsamp_written), f); fclose(f); }
        g_nsamp_written = n;
    }
    LONG ne = g_nev; if (ne > NEV) ne = NEV;
    if (g_ev && ne > g_nev_written) {
        Sleep(5);   /* let writers that already claimed a slot finish it */
        FILE *f = fopen(g_epath, "a");
        if (f) {
            for (LONG i = g_nev_written; i < ne; i++) {
                Event *e = &g_ev[i];
                fprintf(f, "%u %08x %08x %08x", e->t, e->method, e->eid, e->type);
                for (int c = 0; c < 6; c++) fprintf(f, " %08x:%08x", e->path[c], e->ptype[c]);
                fputc('\n', f);
            }
            fclose(f);
        }
        g_nev_written = ne;
    }
}

static DWORD WINAPI probe_thread(LPVOID p) {
    (void)p;
    for (;;) { Sleep(3000); probe_dump(); }
    return 0;
}

/* AIC_REPLAY_TEST="<delay s>:<method>:<entity id>:<type>:<path0>,<path1>,..." (hex IDs): after the delay, run
   that method on that entity once. For checking replay with a single copy of the game. */
static void (*g_test_logf)(const char *, ...);
static char g_cmd_path[MAX_PATH + 64];
static int parse_key(const char *s, ReplayKey *k) {
    char *q;
    memset(k, 0, sizeof *k);
    if (*s == 'h') { k->via_hook = 1; s++; }   /* "h..." = dispatch through the hook, like a game event */
    k->method = strtoul(s, &q, 16); if (*q != ':') return 0;
    k->eid = strtoul(q + 1, &q, 16); if (*q != ':') return 0;
    k->type = strtoul(q + 1, &q, 16);
    while (*q == ':' || *q == ',') { if (k->npath >= 8) break; k->path[k->npath++] = strtoul(q + 1, &q, 16); }
    return k->method && k->eid;
}
static DWORD WINAPI replay_test_thread(LPVOID p) {
    char *s = p, *q;
    int delay = (int)strtoul(s, &q, 10);
    ReplayKey k;
    if (*q == ':' && parse_key(q + 1, &k)) {
        g_test_logf("replay test: method %08x entity %08x type %08x, %d path steps, in %d s", k.method, k.eid, k.type, k.npath, delay);
        Sleep((DWORD)delay * 1000);
        g_test_logf("replay test: requesting (tick %lu)", GetTickCount());
        replay_request(&k);
    }
    /* then keep taking commands from coop_logs\replay_<profile>.cmd, one key per line (same format, no delay) */
    for (;;) {
        Sleep(500);
        FILE *f = fopen(g_cmd_path, "r");
        if (!f) continue;
        char line[256];
        while (fgets(line, sizeof line, f))
            if (parse_key(line, &k)) { g_test_logf("replay cmd: %08x -> %08x (tick %lu)", k.method, k.eid, GetTickCount()); replay_request(&k); }
        fclose(f);
        DeleteFileA(g_cmd_path);
    }
    return 0;
}

/* Returns 1 if the hook is installed. */
int probe_install(const char *dir, const char *profile, void (*logf)(const char *, ...)) {
    char b[8];
    static char test[256];
    int want_test = GetEnvironmentVariableA("AIC_REPLAY_TEST", test, sizeof test) > 0;
    char sy[8];
    g_sync = !(GetEnvironmentVariableA("AIC_SYNC", sy, sizeof sy) && sy[0] == '0');
    if (!GetEnvironmentVariableA("AIC_PROBE", b, sizeof b) || b[0] < '1' || b[0] > '3') {
        if (!want_test && !g_sync) return 0;
        b[0] = '0';   /* hook only: world sync and replay */
    }
    if (g_sync) ws_init(logf);
    g_mode = b[0] - '0';
    replay_init(logf);
    replay_set_hook(cm_hook);
    snprintf(g_cmd_path, sizeof g_cmd_path, "%s\\coop_logs\\replay_%s.cmd", dir, profile);
    if (want_test) { g_test_logf = logf; CloseHandle(CreateThread(NULL, 0, replay_test_thread, test, 0, NULL)); }
    uint8_t *target = (uint8_t *)GetModuleHandleW(NULL) + RVA_CALL_METHOD;
    if (memcmp(target, CM_PROLOGUE, 6) != 0) { logf("probe: call_method prologue mismatch, not hooking"); return 0; }
    char path[MAX_PATH + 64];
    snprintf(path, sizeof path, "%s\\coop_logs\\probe_%s_%lu.log", dir, profile, GetCurrentProcessId());
    if (g_mode) g_plog = fopen(path, "w");
    if (g_mode == 2) {
        g_samp = VirtualAlloc(NULL, sizeof(Sample) * NSAMPLE, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        snprintf(g_spath, sizeof g_spath, "%s\\coop_logs\\samples_%s_%lu.bin", dir, profile, GetCurrentProcessId());
    }
    if (g_mode == 3) {
        g_ev = VirtualAlloc(NULL, sizeof(Event) * NEV, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        g_seen = VirtualAlloc(NULL, sizeof(uint64_t) * NSEEN, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!g_ev || !g_seen) g_mode = 1;
        snprintf(g_epath, sizeof g_epath, "%s\\coop_logs\\events_%s_%lu.txt", dir, profile, GetCurrentProcessId());
    }
    /* trampoline: the 10 relocated prologue bytes, then jmp back to target+10 */
    uint8_t *tr = VirtualAlloc(NULL, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tr) return 0;
    memcpy(tr, target, 10);
    tr[10] = 0xE9; *(int32_t *)(tr + 11) = (int32_t)((target + 10) - (tr + 15));
    g_cm_tramp = (cm_fn)tr;
    DWORD old;
    VirtualProtect(target, 5, PAGE_EXECUTE_READWRITE, &old);
    uint8_t jmp[5] = {0xE9}; *(int32_t *)(jmp + 1) = (int32_t)((uint8_t *)cm_hook - (target + 5));
    memcpy(target, jmp, 5);   /* 5-byte write; the game may already be running other threads, see notes */
    VirtualProtect(target, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), target, 5);
    if (g_mode) CloseHandle(CreateThread(NULL, 0, probe_thread, NULL, 0, NULL));
    logf("probe: call_method hooked at %p, trampoline %p, log %s", (void *)target, (void *)tr, path);
    return 1;
}
