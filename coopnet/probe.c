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
typedef struct { uint32_t self, arg[3], tvt, mvt, l1[3][8], l2[3][8][32], tobj[64]; } Sample;
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
    InterlockedIncrement(&g_nsamp_done);
}

static uint32_t __attribute__((thiscall)) cm_hook(void *self, void *a1, void *a2, void *a3) {
    InterlockedIncrement(&g_calls);
    Pair *p = record(self, a1, a3);
    if (g_mode == 2 && p) sample(self, a1, a2, a3, p);
    return g_cm_tramp(self, a1, a2, a3);
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
}

static DWORD WINAPI probe_thread(LPVOID p) {
    (void)p;
    for (;;) { Sleep(3000); probe_dump(); }
    return 0;
}

/* Returns 1 if the hook is installed. */
int probe_install(const char *dir, const char *profile, void (*logf)(const char *, ...)) {
    char b[8];
    if (!GetEnvironmentVariableA("AIC_PROBE", b, sizeof b) || (b[0] != '1' && b[0] != '2')) return 0;
    g_mode = b[0] - '0';
    uint8_t *target = (uint8_t *)GetModuleHandleW(NULL) + RVA_CALL_METHOD;
    if (memcmp(target, CM_PROLOGUE, 6) != 0) { logf("probe: call_method prologue mismatch, not hooking"); return 0; }
    char path[MAX_PATH + 64];
    snprintf(path, sizeof path, "%s\\coop_logs\\probe_%s_%lu.log", dir, profile, GetCurrentProcessId());
    g_plog = fopen(path, "w");
    if (g_mode == 2) {
        g_samp = VirtualAlloc(NULL, sizeof(Sample) * NSAMPLE, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        snprintf(g_spath, sizeof g_spath, "%s\\coop_logs\\samples_%s_%lu.bin", dir, profile, GetCurrentProcessId());
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
    CloseHandle(CreateThread(NULL, 0, probe_thread, NULL, 0, NULL));
    logf("probe: call_method hooked at %p, trampoline %p, log %s", (void *)target, (void *)tr, path);
    return 1;
}
