/* engine: moving the local player, the way the game moves a character back that fell out of the world.
 *
 * Method, addresses and layouts from OpenCAGE's RuntimeUtils (PLAYER_CHEATS.cpp, PLAYER_DEBUG.cpp) by Matt Filer,
 * https://github.com/MattFiler/OpenCAGE, MIT license (third_party/licenses/OpenCAGE-LICENSE.txt). Same Steam build as
 * ours (OpenCAGE checks AI.exe's timestamp 0x54E3199D and image size 0x01F81000, as the lobby does).
 *
 *   post_process_characters  RVA 0x4031B0  cdecl()   the physics' pass over the characters at the end of each frame.
 *                                                    Hooked: a teleport asked for runs right after it, where the game
 *                                                    does its own.
 *   reposition_into_world    RVA 0x3ECCA0  thiscall(controller, float pos[4] aligned 16)
 *   change_to_idle           RVA 0x25BDD0  thiscall(action)
 *   globals                  [exe+0x12F0C88]; the byte after the pointer is set while characters can be read;
 *                            +0x08 the character manager: +0x80 the players (a pointer to their pointers, count at +0x84)
 *   character                +0x33C health (u16), +0x60 physics controller index, +0x36C current action (pointer)
 *   controllers              exe+0x1307A48, 18 of 0xB0: +0 the character, +0xA8 ready
 */
#include <windows.h>
#include <stdint.h>
#include <string.h>
#include "engine.h"

#define RVA_POST_PROCESS 0x4031B0u
#define RVA_REPOSITION   0x3ECCA0u
#define RVA_TO_IDLE      0x25BDD0u
#define RVA_GLOBALS      0x12F0C88u
#define RVA_CONTROLLERS  0x1307A48u

typedef void(__cdecl *pp_fn)(void);
typedef void(__attribute__((thiscall)) *repos_fn)(void *, const float *);
typedef void(__attribute__((thiscall)) *idle_fn)(void *);

static void (*g_log)(const char *, ...);
static uintptr_t g_base;
static pp_fn g_pp_orig;
static repos_fn g_repos;
static idle_fn g_idle;
static int g_on;

static CRITICAL_SECTION g_cs;
static int g_pending; static float g_to[3]; static uint32_t g_ticket;
static volatile LONG g_done_ticket, g_result;

static int rd(uintptr_t a, void *o, size_t n) {
    SIZE_T got = 0;
    return a > 0x10000 && a < 0x7FFF0000 && ReadProcessMemory(GetCurrentProcess(), (LPCVOID)a, o, n, &got) && got == n;
}

static uint32_t player(void) {
    uint32_t globals = 0, list = 0, count = 0, p = 0; uint8_t valid = 0;
    if (!rd(g_base + RVA_GLOBALS, &globals, 4) || !rd(g_base + RVA_GLOBALS + 4, &valid, 1) || !globals || !valid) return 0;
    uint32_t mgr = 0;
    if (!rd(globals + 0x08, &mgr, 4) || !mgr) return 0;
    if (!rd(mgr + 0x80, &list, 4) || !rd(mgr + 0x84, &count, 4) || !list || !count || count > 18) return 0;
    return rd(list, &p, 4) ? p : 0;
}

static int teleport_now(const float to[3]) {
    uint32_t pl = player();
    if (!pl) return ENG_NO_PLAYER;
    uint16_t health = 0;
    if (!rd(pl + 0x33C, &health, 2) || !health) return ENG_DEAD;
    int32_t handle = -1;
    if (!rd(pl + 0x60, &handle, 4) || handle < 0 || handle >= 18) return ENG_NO_BODY;
    uintptr_t ctl = g_base + RVA_CONTROLLERS + (uintptr_t)handle * 0xB0u;
    uint32_t owner = 0; uint8_t ready = 0;
    if (!rd(ctl, &owner, 4) || owner != pl || !rd(ctl + 0xA8, &ready, 1) || !ready) return ENG_NO_BODY;
    float pos[4] __attribute__((aligned(16))) = {to[0], to[1], to[2], 0.f};
    g_repos((void *)ctl, pos);
    uint32_t action = 0;
    if (rd(pl + 0x36C, &action, 4) && action) g_idle((void *)(uintptr_t)action);
    return ENG_DONE;
}

static void __cdecl post_process_hook(void) {
    g_pp_orig();
    if (!g_pending) return;
    float to[3]; uint32_t t;
    EnterCriticalSection(&g_cs);
    memcpy(to, g_to, 12); t = g_ticket; g_pending = 0;
    LeaveCriticalSection(&g_cs);
    LONG r = teleport_now(to);
    InterlockedExchange(&g_result, r);
    InterlockedExchange(&g_done_ticket, (LONG)t);
}

static int bytes_ok(uintptr_t a, const uint8_t *want, int n) { uint8_t got[16]; return n <= 16 && rd(a, got, (size_t)n) && !memcmp(got, want, (size_t)n); }

int engine_install(void (*logf)(const char *, ...)) {
    if (g_on) return 1;
    g_log = logf;
    g_base = (uintptr_t)GetModuleHandleW(NULL);
    static const uint8_t sig_pp[12] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x81, 0xEC, 0xD4, 0x00, 0x00, 0x00};
    static const uint8_t sig_rp[16] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x83, 0xEC, 0x44, 0x53, 0x56, 0x8B, 0xF1, 0x8B, 0x0E, 0x57};
    static const uint8_t sig_id[7] = {0x6A, 0xFF, 0x68, 0x32, 0xD9, 0x12, 0x01};
    uint8_t id7[7];
    /* change_to_idle's push imm32 is relocated by the loader: compare the opcodes only */
    int idle_ok = rd(g_base + RVA_TO_IDLE, id7, 7) && id7[0] == sig_id[0] && id7[1] == sig_id[1] && id7[2] == sig_id[2];
    if (!bytes_ok(g_base + RVA_POST_PROCESS, sig_pp, 12) || !bytes_ok(g_base + RVA_REPOSITION, sig_rp, 16) || !idle_ok) {
        logf("engine: functions don't match this build, no teleports");
        return 0;
    }
    InitializeCriticalSection(&g_cs);
    g_repos = (repos_fn)(g_base + RVA_REPOSITION);
    g_idle = (idle_fn)(g_base + RVA_TO_IDLE);
    /* detour: the first 6 bytes (push ebp / mov ebp,esp / and esp,-16) hold no relative addresses */
    uint8_t *fn = (uint8_t *)(g_base + RVA_POST_PROCESS);
    uint8_t *tr = VirtualAlloc(NULL, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tr) return 0;
    memcpy(tr, fn, 6);
    tr[6] = 0xE9; *(int32_t *)(tr + 7) = (int32_t)((fn + 6) - (tr + 11));
    g_pp_orig = (pp_fn)(void *)tr;
    DWORD old;
    VirtualProtect(fn, 6, PAGE_EXECUTE_READWRITE, &old);
    fn[0] = 0xE9; *(int32_t *)(fn + 1) = (int32_t)((uint8_t *)post_process_hook - (fn + 5)); fn[5] = 0x90;
    VirtualProtect(fn, 6, old, &old);
    FlushInstructionCache(GetCurrentProcess(), fn, 6);
    g_on = 1;
    logf("engine: player teleport ready (end-of-frame physics pass)");
    return 1;
}

uint32_t engine_teleport(const float to[3]) {
    if (!g_on) return 0;
    EnterCriticalSection(&g_cs);
    memcpy(g_to, to, 12); g_pending = 1;
    if (++g_ticket == 0) g_ticket = 1;
    uint32_t t = g_ticket;
    LeaveCriticalSection(&g_cs);
    return t;
}

int engine_result(uint32_t ticket) {
    if (!ticket || (uint32_t)g_done_ticket != ticket) return ENG_PENDING;
    return (int)g_result;
}
