/* Skin bodies: spawn your partner in your game as a story character (see skins.h) and keep them where they are.
 *
 * Engine side (Steam build 4940658; addresses and the spawn-request layout from AI-MP by Dropper7856,
 * https://github.com/Dropper7856/AIMP, non-commercial attribution license, third_party/AIMP-LICENSE.txt):
 *   CharacterManager::Update   RVA 0x3CC080  thiscall(manager)      - hooked; our engine-thread tick
 *   ReceiveSpawnRequest130     RVA 0x014C90  cdecl(sink, request)   - sink = manager + 0x14
 *   character runtime-id counter RVA 0x12AE64C (spawn keys)
 *   TeleportCharacter          RVA 0x3ECCA0  thiscall(registry entry, float pos[4] aligned 16)
 *   character registry         RVA 0x1307A48, 18 entries of 0xB0: +0 Character*, +0x0C/+0x44 bodies, +0xA8 active
 * The level being played comes from the files the game opens (...\PRODUCTION\<LEVEL>\WORLD\...).
 *
 * Off unless AIC_SKINS=1 (not tested in game yet). AIC_SKIN=<name> picks the skin your partner sees you as.
 *
 * Shared alien (joiner, AIC_SHARED_ALIEN=1, not tested in game yet): the host sends its alien's position 30 times
 * a second. The joiner keeps the last few samples and plays them back 50 ms behind, interpolated (alien_interp.h),
 * and on every engine frame puts its own alien (registry tag at +0x8C) on that point whenever it is more than
 * 10 cm off. So the joiner's alien is where the host's alien is, to within a frame or two. Its facing and
 * animation are still its own.
 */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "skins.h"
#include "skin.h"
#include "alien_interp.h"

#define RVA_MGR_UPDATE  0x3CC080u
#define RVA_SPAWN130    0x014C90u
#define RVA_SPAWN_CTR   0x12AE64Cu
#define RVA_TELEPORT    0x3ECCA0u
#define RVA_REGISTRY    0x1307A48u
#define REG_STRIDE      0xB0u
#define REG_COUNT       18

static void (*g_log)(const char *, ...);
static uintptr_t g_base;
static int g_on;

/* level, from file opens (any thread) */
static CRITICAL_SECTION g_lcs;
static char g_level[48];
static volatile LONG g_level_gen;
static volatile DWORD g_level_t;

/* wanted state, from the net thread */
static char g_want[24] = "TAYLOR";            /* partner's pick (their SkinPkt) */
static volatile LONG g_target_ok;
static float g_target[3];                       /* partner's feet */
static volatile DWORD g_target_t;

/* engine-thread state */
typedef void(__attribute__((thiscall)) *mgr_fn)(void *);
typedef uint32_t(__cdecl *spawn_fn)(void *, const void *);
typedef void(__attribute__((thiscall)) *tp_fn)(void *, const float *);
static mgr_fn g_mgr_orig;
static spawn_fn g_spawn;
static tp_fn g_tp;
static LONG g_seen_gen = -1;
static uint32_t g_before[REG_COUNT];
static uint32_t g_bound_chr; static int g_bound_slot = -1;
static DWORD g_pending_t, g_retry_t; static int g_tries; static uint32_t g_nonce;
static const Skin *g_cur;
static volatile LONG g_active;
static int g_bodies = 1;

/* shared alien: the host's alien position (net thread) */
static volatile LONG g_alien_on, g_alien_ok;
static int g_alien_tag = 3;
static AlienTrack g_atrack;                      /* samples from the host (guarded by g_lcs) */
static volatile DWORD g_alien_t;
static unsigned g_alien_tps; static DWORD g_alien_log_t;
#define ALIEN_EPS_M    0.10f                     /* correct when further off than this */
#define ALIEN_DELAY_MS 50
static char g_status[96] = "off";

static int rd(uintptr_t a, void *o, size_t n) {
    SIZE_T got = 0;
    return a > 0x10000 && a < 0x7FFF0000 && ReadProcessMemory(GetCurrentProcess(), (LPCVOID)a, o, n, &got) && got == n;
}
static uintptr_t reg_entry(int i) { return g_base + RVA_REGISTRY + (uintptr_t)i * REG_STRIDE; }
static uint32_t reg_chr(int i, int need_active) {
    uint32_t c = 0, b1 = 0, b2 = 0; uint8_t act = 0; uintptr_t e = reg_entry(i);
    if (!rd(e, &c, 4) || !c) return 0;
    if (!need_active) return c;
    if (!rd(e + 0x0C, &b1, 4) || !rd(e + 0x44, &b2, 4) || !rd(e + 0xA8, &act, 1) || !b1 || !b2 || !act) return 0;
    return c;
}
static void status(const char *s) { lstrcpynA(g_status, s, sizeof g_status); if (g_log) g_log("skin: %s", s); }

static void reset_body(const char *why) {
    if (g_bound_chr || g_pending_t) { char m[96]; snprintf(m, sizeof m, "body released (%s)", why); status(m); }
    g_bound_chr = 0; g_bound_slot = -1; g_pending_t = 0; g_tries = 0; g_retry_t = 0; g_cur = NULL;
    InterlockedExchange(&g_active, 0);
}

static void alien_tick(void) {
    DWORD now = GetTickCount();
    if (!g_alien_on || !g_alien_ok || now - g_alien_t > 500) return;
    float t[3]; int have;
    EnterCriticalSection(&g_lcs); have = alien_track_at(&g_atrack, now - ALIEN_DELAY_MS, t); LeaveCriticalSection(&g_lcs);
    if (!have) return;
    for (int i = 0; i < REG_COUNT; i++) {
        uint32_t tag = 0; float cur[3];
        if (i == g_bound_slot || !reg_chr(i, 1)) continue;
        if (!rd(reg_entry(i) + 0x8C, &tag, 4) || (int)tag != g_alien_tag || !rd(reg_entry(i) + 0x80, cur, 12)) continue;
        float dx = t[0] - cur[0], dy = t[1] - cur[1], dz = t[2] - cur[2];
        if (dx * dx + dy * dy + dz * dz > ALIEN_EPS_M * ALIEN_EPS_M) {
            float pos[8] __attribute__((aligned(16))) = {t[0], t[1], t[2], 0};
            g_tp((void *)reg_entry(i), pos);
            g_alien_tps++;
            if (g_log && (g_alien_tps == 1 || now - g_alien_log_t > 10000)) {
                g_alien_log_t = now;
                g_log("alien: following the host's alien (%.2f m off before this frame, slot %d, %u corrections)", sqrtf(dx * dx + dy * dy + dz * dz), i, g_alien_tps);
            }
        }
        return;
    }
}

static void engine_tick(void *mgr) {
    alien_tick();
    if (!g_bodies) return;
    DWORD now = GetTickCount();
    LONG gen = g_level_gen;
    if (gen != g_seen_gen) { g_seen_gen = gen; reset_body("level changed"); }
    char level[48]; EnterCriticalSection(&g_lcs); memcpy(level, g_level, sizeof level); LeaveCriticalSection(&g_lcs);
    int target_ok = g_target_ok && now - g_target_t < 2000;
    float t[3]; memcpy(t, g_target, 12);

    if (g_bound_chr) {
        if (reg_chr(g_bound_slot, 1) != g_bound_chr) { reset_body("character gone"); return; }
        if (target_ok) {
            float pos[8] __attribute__((aligned(16))) = {t[0], t[1], t[2], 0};
            float cur[3];
            if (rd(reg_entry(g_bound_slot) + 0x80, cur, 12)) {
                float dx = t[0] - cur[0], dy = t[1] - cur[1], dz = t[2] - cur[2];
                if (dx * dx + dy * dy + dz * dz > 0.0025f) g_tp((void *)reg_entry(g_bound_slot), pos);
            }
        }
        return;
    }
    if (g_pending_t) {   /* find the character our request created: a registry slot that wasn't there before */
        for (int i = 0; i < REG_COUNT; i++) {
            uint32_t c = reg_chr(i, 1);
            if (!c || c == g_before[i]) continue;
            int old = 0; for (int k = 0; k < REG_COUNT; k++) if (g_before[k] == c) old = 1;
            if (old) continue;
            g_bound_chr = c; g_bound_slot = i; g_pending_t = 0;
            InterlockedExchange(&g_active, 1);
            char m[96]; snprintf(m, sizeof m, "%s body spawned (registry slot %d)", g_cur ? g_cur->name : "?", i); status(m);
            return;
        }
        if (now - g_pending_t > 6000) {
            g_pending_t = 0; g_retry_t = now + 10000;
            status(g_tries >= 3 ? "spawn not confirmed 3 times, giving up in this level" : "spawn not confirmed, retrying in 10 s");
        }
        return;
    }
    if (!target_ok || !level[0] || g_tries >= 3 || (g_retry_t && (int32_t)(now - g_retry_t) < 0)) return;
    if (now - g_level_t < 5000) return;                 /* let the level settle */
    const Skin *s = skin_choose(g_want, level);
    if (!s) {
        static LONG said = -2;
        if (said != gen) { said = gen; char m[96]; snprintf(m, sizeof m, "no skin model in %s", level); status(m); }
        return;
    }
    uint32_t vt = 0, sink = (uint32_t)(uintptr_t)mgr + 0x14;
    if (!rd(sink, &vt, 4) || !vt) return;
    LONG key = InterlockedExchangeAdd((volatile LONG *)(g_base + RVA_SPAWN_CTR), 1);
    if (key == 0 || key == -1) return;
    uint8_t req[SKIN_REQ_SIZE];
    g_nonce++;
    skin_build_request(req, s, t, (uint32_t)key, skin_entity_key((uint32_t)key, g_nonce), 1u + g_nonce);
    for (int i = 0; i < REG_COUNT; i++) g_before[i] = reg_chr(i, 0);
    g_cur = s; g_tries++; g_pending_t = now;
    uint32_t r = g_spawn((void *)(uintptr_t)sink, req);
    char m[128]; snprintf(m, sizeof m, "spawn request %s in %s at (%.1f %.1f %.1f), key %ld, result %u", s->name, level, t[0], t[1], t[2], key, r);
    status(m);
}

static void __attribute__((thiscall)) mgr_update_hook(void *mgr) {
    if (g_on) engine_tick(mgr);
    g_mgr_orig(mgr);
}

/* ---------- level from file opens ---------- */
typedef HANDLE(WINAPI *cfa_fn)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
typedef HANDLE(WINAPI *cfw_fn)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
static cfa_fn g_cfa; static cfw_fn g_cfw;
static void saw_path(const char *p) {
    char lv[48];
    if (!skin_level_from_path(p, lv, sizeof lv)) return;
    EnterCriticalSection(&g_lcs);
    int changed = strcmp(lv, g_level) != 0;
    if (changed) { strcpy(g_level, lv); g_level_t = GetTickCount(); InterlockedIncrement(&g_level_gen); }
    LeaveCriticalSection(&g_lcs);
    if (changed && g_log) g_log("skin: level %s", lv);
}
static HANDLE WINAPI cfa_hook(LPCSTR n, DWORD a, DWORD s, LPSECURITY_ATTRIBUTES sa, DWORD c, DWORD f, HANDLE t) {
    if (n) saw_path(n);
    return g_cfa(n, a, s, sa, c, f, t);
}
static HANDLE WINAPI cfw_hook(LPCWSTR n, DWORD a, DWORD s, LPSECURITY_ATTRIBUTES sa, DWORD c, DWORD f, HANDLE t) {
    if (n) { char b[MAX_PATH]; if (WideCharToMultiByte(CP_ACP, 0, n, -1, b, sizeof b, NULL, NULL) > 0) saw_path(b); }
    return g_cfw(n, a, s, sa, c, f, t);
}
static void iat_patch(HMODULE mod, const char *dll, const char *fn, void *hook, void **orig) {
    uint8_t *b = (uint8_t *)mod;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(b + ((IMAGE_DOS_HEADER *)b)->e_lfanew);
    IMAGE_DATA_DIRECTORY d = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    for (IMAGE_IMPORT_DESCRIPTOR *im = (IMAGE_IMPORT_DESCRIPTOR *)(b + d.VirtualAddress); im->Name; im++) {
        if (lstrcmpiA((char *)(b + im->Name), dll)) continue;
        IMAGE_THUNK_DATA *on = (IMAGE_THUNK_DATA *)(b + (im->OriginalFirstThunk ? im->OriginalFirstThunk : im->FirstThunk));
        IMAGE_THUNK_DATA *ft = (IMAGE_THUNK_DATA *)(b + im->FirstThunk);
        for (; on->u1.AddressOfData; on++, ft++) {
            if (IMAGE_SNAP_BY_ORDINAL(on->u1.Ordinal)) continue;
            IMAGE_IMPORT_BY_NAME *nm = (IMAGE_IMPORT_BY_NAME *)(b + on->u1.AddressOfData);
            if (strcmp((char *)nm->Name, fn)) continue;
            DWORD old; VirtualProtect(&ft->u1.Function, 4, PAGE_READWRITE, &old);
            *orig = (void *)(uintptr_t)ft->u1.Function; ft->u1.Function = (uintptr_t)hook;
            VirtualProtect(&ft->u1.Function, 4, old, &old);
            return;
        }
    }
}

/* ---------- install ---------- */
static int bytes_ok(uintptr_t a, const uint8_t *want, int n) {
    uint8_t got[16]; return n <= 16 && rd(a, got, (size_t)n) && !memcmp(got, want, (size_t)n);
}

void skin_init_level_watch(void (*logf)(const char *, ...)) {
    static int done; if (done) return; done = 1;
    g_log = logf; InitializeCriticalSection(&g_lcs);
    HMODULE exe = GetModuleHandleW(NULL);
    iat_patch(exe, "KERNEL32.dll", "CreateFileA", (void *)cfa_hook, (void **)&g_cfa);
    iat_patch(exe, "KERNEL32.dll", "CreateFileW", (void *)cfw_hook, (void **)&g_cfw);
    if (!g_cfa || !g_cfw) logf("skin: could not watch level loads (CreateFile import not found)");
}

int skin_install(void (*logf)(const char *, ...), int bodies) {
    g_bodies = bodies;
    skin_init_level_watch(logf);
    {   /* skins copied into levels by tools/skinprep */
        char path[MAX_PATH]; GetModuleFileNameA(NULL, path, sizeof path);
        char *sl = strrchr(path, 92 /* backslash */); if (sl) strcpy(sl + 1, "coop_skins.txt");
        FILE *f = fopen(path, "r"); int n = 0;
        if (f) { char line[512]; while (fgets(line, sizeof line, f)) { skin_extra_add_line(line); n++; } fclose(f); }
        logf("skin: %d levels listed in coop_skins.txt", n);
    }
    g_base = (uintptr_t)GetModuleHandleW(NULL);
    static const uint8_t sig_mgr[7] = {0x6A, 0xFF, 0x68, 0x28, 0xDE, 0x12, 0x01};
    static const uint8_t sig_spawn[10] = {0x55, 0x8B, 0xEC, 0x56, 0x57, 0xB8, 0x40, 0x01, 0x00, 0x00};
    static const uint8_t sig_tp[16] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x83, 0xEC, 0x44, 0x53, 0x56, 0x8B, 0xF1, 0x8B, 0x0E, 0x57};
    uint8_t *mgr = (uint8_t *)(g_base + RVA_MGR_UPDATE);
    /* the push imm32 in the prologue is relocated by the loader: compare only the opcode bytes */
    uint8_t m7[7];
    if (!rd((uintptr_t)mgr, m7, 7) || m7[0] != sig_mgr[0] || m7[1] != sig_mgr[1] || m7[2] != sig_mgr[2] ||
        !bytes_ok(g_base + RVA_SPAWN130, sig_spawn, 10) || !bytes_ok(g_base + RVA_TELEPORT, sig_tp, 16)) {
        logf("skin: engine functions don't match this build, skins off"); return 0;
    }
    g_spawn = (spawn_fn)(g_base + RVA_SPAWN130);
    g_tp = (tp_fn)(g_base + RVA_TELEPORT);
    uint8_t *tr = VirtualAlloc(NULL, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tr) return 0;
    memcpy(tr, mgr, 7);
    tr[7] = 0xE9; *(int32_t *)(tr + 8) = (int32_t)((mgr + 7) - (tr + 12));
    g_mgr_orig = (mgr_fn)(void *)tr;
    DWORD old; VirtualProtect(mgr, 7, PAGE_EXECUTE_READWRITE, &old);
    mgr[0] = 0xE9; *(int32_t *)(mgr + 1) = (int32_t)((uint8_t *)mgr_update_hook - (mgr + 5)); mgr[5] = 0x90; mgr[6] = 0x90;
    VirtualProtect(mgr, 7, old, &old);
    FlushInstructionCache(GetCurrentProcess(), mgr, 7);
    g_on = 1;
    status(bodies ? "on, waiting for a level and the partner" : "bodies off");
    return 1;
}

void skin_set_partner(const char *name) {
    if (!name || !name[0] || !strcmp(name, g_want)) return;
    lstrcpynA(g_want, name, sizeof g_want);
    if (g_log) g_log("skin: partner picked %s", g_want);
}
void skin_set_target(int ok, const float feet[3]) {
    if (ok) { memcpy(g_target, feet, 12); g_target_t = GetTickCount(); }
    InterlockedExchange(&g_target_ok, ok);
}
int skin_active(void) { return g_active != 0; }

/* (Superseded by engine.c, which moves the player the way OpenCAGE does; kept for reference, unused.)
   Teleport the local player (cutscene.h), on the game thread. The player is the active registry entry with tag 0
   that is near the camera (`cam`), so a stray entry can't be moved by mistake. Needs no hooks: only the
   TeleportCharacter address, checked against its bytes first. */
int skin_teleport_player(const float pos[3], const float cam[3]) {
    static int checked, ok;
    if (!checked) {
        checked = 1;
        if (!g_base) g_base = (uintptr_t)GetModuleHandleW(NULL);
        static const uint8_t sig_tp[16] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x83, 0xEC, 0x44, 0x53, 0x56, 0x8B, 0xF1, 0x8B, 0x0E, 0x57};
        ok = bytes_ok(g_base + RVA_TELEPORT, sig_tp, 16);
        if (ok && !g_tp) g_tp = (tp_fn)(g_base + RVA_TELEPORT);
    }
    if (!ok || !g_tp) return -1;
    for (int i = 0; i < REG_COUNT; i++) {
        uint32_t tag = 1; float cur[3];
        if (i == g_bound_slot || !reg_chr(i, 1)) continue;
        if (!rd(reg_entry(i) + 0x8C, &tag, 4) || tag != 0 || !rd(reg_entry(i) + 0x80, cur, 12)) continue;
        float dx = cur[0] - cam[0], dz = cur[2] - cam[2];
        if (dx * dx + dz * dz > 2.5f * 2.5f) continue;
        float p[8] __attribute__((aligned(16))) = {pos[0], pos[1], pos[2], 0};
        g_tp((void *)reg_entry(i), p);
        return 1;
    }
    return 0;
}
void skin_share_alien(int on, int tag) { g_alien_tag = tag; InterlockedExchange(&g_alien_on, on); }
void skin_set_alien(int ok, const float pos[3]) {
    if (!g_alien_on || !g_lcs.DebugInfo) return;
    if (ok) { DWORD now = GetTickCount(); EnterCriticalSection(&g_lcs); alien_track_push(&g_atrack, now, pos); LeaveCriticalSection(&g_lcs); g_alien_t = now; }
    InterlockedExchange(&g_alien_ok, ok);
}
int skin_level_has_skin(void) {
    char level[48]; if (!g_lcs.DebugInfo) return 0;
    EnterCriticalSection(&g_lcs); memcpy(level, g_level, sizeof level); LeaveCriticalSection(&g_lcs);
    return skin_choose(g_want, level) != NULL;
}
const char *skin_status(void) { return g_status; }
