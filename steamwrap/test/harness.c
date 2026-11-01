/* Offline test for steamwrap: calls the wrapper the way AI.exe does (MSVC __thiscall, callee pops)
 * and checks that each call pops exactly the bytes the Steamworks signature says. */
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

typedef struct { void **vtbl; } Obj;
static int fails, passes;
#define CHECK(c, ...) do { if (c) passes++; else { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* Call vtable[slot] as thiscall with n dword args (args[0] pushed last = first param).
 * Returns eax; *popped = bytes the callee removed from the stack. */
static uint32_t vcall(Obj *o, int slot, int n, const uint32_t *args, int *popped) {
    void *fn = o->vtbl[slot];
    uint32_t r, saved, after;
    __asm__ volatile(
        "movl %%esp, %[saved]\n\t"
        "movl %[n], %%ecx\n\t"
        "1: testl %%ecx, %%ecx\n\t"
        "jz 2f\n\t"
        "pushl -4(%[args],%%ecx,4)\n\t"
        "decl %%ecx\n\t"
        "jmp 1b\n\t"
        "2: movl %[obj], %%ecx\n\t"
        "call *%[fn]\n\t"
        "movl %%esp, %[after]\n\t"
        "movl %[saved], %%esp\n\t"
        : [saved] "=&m"(saved), [after] "=&m"(after), "=a"(r)
        : [n] "m"(n), [args] "r"(args), [obj] "m"(o), [fn] "m"(fn)
        : "ecx", "edx", "memory", "cc");
    *popped = (int)(after - (saved - 4u * (uint32_t)n)); /* after - esp_after_pushes */
    return r;
}

typedef int (__cdecl *pInit)(void);
typedef int (__cdecl *pRestart)(uint32_t);
typedef void *(__cdecl *pAcc)(void);
typedef void (__cdecl *pVoid)(void);
typedef void (__cdecl *pRegCb)(void *, int);
typedef void (__cdecl *pRegCr)(void *, uint64_t);

int main(void) {
    SetEnvironmentVariableA("AIC_PROFILE", "p2");
    HMODULE w = LoadLibraryA("steam_api.dll");
    CHECK(w != NULL, "load wrapper err %lu", GetLastError());
    if (!w) return 1;
    HMODULE real = GetModuleHandleA("steam_api_real.dll");
    CHECK(real != NULL, "real not loaded");
    volatile int *last = (volatile int *)GetProcAddress(real, "g_last_slot");
    volatile uint32_t *larg = (volatile uint32_t *)GetProcAddress(real, "g_last_arg");

    CHECK(((pRestart)GetProcAddress(w, "SteamAPI_RestartAppIfNecessary"))(214490) == 0, "restart must be 0");
    CHECK(((pInit)GetProcAddress(w, "SteamAPI_Init"))() == 1, "init");
    CHECK(((pAcc)GetProcAddress(w, "SteamUtils"))() == (void *)0x11, "utils fwd");
    CHECK(((pAcc)GetProcAddress(w, "SteamFriends"))() == (void *)0x12, "friends fwd");
    CHECK(((pAcc)GetProcAddress(w, "SteamUser"))() == (void *)0x13, "user fwd");
    CHECK(((pAcc)GetProcAddress(w, "SteamApps"))() == (void *)0x14, "apps fwd");
    ((pVoid)GetProcAddress(w, "SteamAPI_RunCallbacks"))();
    CHECK(*last == 900, "runcallbacks fwd");
    ((pRegCb)GetProcAddress(w, "SteamAPI_RegisterCallback"))((void *)1, 1307);
    CHECK(*last == 901 && *larg == 1307, "regcb fwd");
    ((pRegCr)GetProcAddress(w, "SteamAPI_RegisterCallResult"))((void *)1, 0x1234ull);
    CHECK(*last == 902 && *larg == 0x1234, "regcr fwd");

    Obj *rs = ((pAcc)GetProcAddress(w, "SteamRemoteStorage"))();
    Obj *us = ((pAcc)GetProcAddress(w, "SteamUserStats"))();
    int p; uint32_t r;
    const char *data = "SAVEDATA-123";
    char buf[64] = {0};
    int32_t sz = 0;

    { uint32_t a[] = {(uint32_t)"slot_0.sav", (uint32_t)data, 12}; r = vcall(rs, 0, 3, a, &p);
      CHECK((r & 0xFF) == 1 && p == 12, "FileWrite r=%u popped=%d", r, p); }
    { uint32_t a[] = {(uint32_t)"slot_0.sav", (uint32_t)buf, 64}; r = vcall(rs, 1, 3, a, &p);
      CHECK(r == 12 && p == 12 && !memcmp(buf, data, 12), "FileRead r=%u popped=%d", r, p); }
    { uint32_t a[] = {(uint32_t)"slot_0.sav"}; r = vcall(rs, 10, 1, a, &p);
      CHECK((r & 0xFF) == 1 && p == 4, "FileExists r=%u popped=%d", r, p); }
    { uint32_t a[] = {(uint32_t)"nope.sav"}; r = vcall(rs, 10, 1, a, &p);
      CHECK((r & 0xFF) == 0 && p == 4, "FileExists(missing) r=%u popped=%d", r, p); }
    { uint32_t a[] = {(uint32_t)"slot_0.sav"}; r = vcall(rs, 12, 1, a, &p);
      CHECK(r == 12 && p == 4, "GetFileSize r=%u popped=%d", r, p); }
    { r = vcall(rs, 15, 0, NULL, &p); CHECK(r == 1 && p == 0, "GetFileCount r=%u popped=%d", r, p); }
    { uint32_t a[] = {0, (uint32_t)&sz}; r = vcall(rs, 16, 2, a, &p);
      CHECK(r && !strcmp((const char *)r, "slot_0.sav") && sz == 12 && p == 8, "GetFileNameAndSize popped=%d", p); }
    { uint32_t a[] = {0, 0}; r = vcall(rs, 17, 2, a, &p); CHECK((r & 0xFF) == 1 && p == 8, "GetQuota popped=%d", p); }
    { r = vcall(rs, 19, 0, NULL, &p); CHECK((r & 0xFF) == 0 && p == 0, "IsCloudEnabledForApp false popped=%d", p); }
    { uint32_t a[] = {1}; r = vcall(rs, 20, 1, a, &p); CHECK(p == 4, "SetCloudEnabledForApp popped=%d", p); }
    { *last = -1; r = vcall(rs, 31, 0, NULL, &p); CHECK((r & 0xFF) == 1 && p == 0 && *last == -1, "SynchronizeToServer local popped=%d", p); }
    { uint32_t a[] = {(uint32_t)"slot_0.sav"}; r = vcall(rs, 13, 1, a, &p); CHECK(r > 1700000000u && p == 4, "GetFileTimestamp r=%u popped=%d", r, p); }

    /* write stream: Open(name)->u64 pops 4; WriteChunk(u64 h, data, n) pops 16; Close(u64 h) pops 8 */
    uint32_t h;
    { uint32_t a[] = {(uint32_t)"stream.sav"}; h = vcall(rs, 6, 1, a, &p); CHECK(h >= 1 && h <= 16 && p == 4, "StreamOpen h=%u popped=%d", h, p); }
    { uint32_t a[] = {h, 0, (uint32_t)data, 5}; r = vcall(rs, 7, 4, a, &p); CHECK((r & 0xFF) == 1 && p == 16, "StreamWrite popped=%d", p); }
    { uint32_t a[] = {h, 0}; r = vcall(rs, 8, 2, a, &p); CHECK((r & 0xFF) == 1 && p == 8, "StreamClose popped=%d", p); }
    CHECK(GetFileAttributesA("coop_saves\\p2\\stream.sav") != INVALID_FILE_ATTRIBUTES, "stream file in coop_saves\\p2");
    CHECK(GetFileAttributesA("coop_saves\\p2\\slot_0.sav") != INVALID_FILE_ATTRIBUTES, "save in coop_saves\\p2");
    { uint32_t a[] = {(uint32_t)"slot_0.sav"}; r = vcall(rs, 3, 1, a, &p); CHECK((r & 0xFF) == 1 && p == 4, "FileDelete popped=%d", p); }
    CHECK(GetFileAttributesA("coop_saves\\p2\\slot_0.sav") == INVALID_FILE_ATTRIBUTES, "deleted");

    /* forwarded slots reach the real object with this=real; fake pops 8 */
    { uint32_t a[] = {0xABCD, 0}; r = vcall(rs, 24, 2, a, &p); CHECK(r == 1024 && *last == 24 && *larg == 0xABCD && p == 8, "RS fwd 24 r=%u popped=%d", r, p); }
    { uint32_t a[] = {0x55, 0}; r = vcall(rs, 39, 2, a, &p); CHECK(r == 1039 && *last == 39 && p == 8, "RS fwd 39 r=%u popped=%d", r, p); }
    { uint32_t a[] = {0x77, 0}; r = vcall(us, 6, 2, a, &p); CHECK(r == 1006 && *last == 6 && *larg == 0x77 && p == 8, "US fwd 6 r=%u popped=%d", r, p); }
    { uint32_t a[] = {0x78, 0}; r = vcall(us, 0, 2, a, &p); CHECK(r == 1000 && *last == 0 && p == 8, "US fwd 0 popped=%d", p); }
    /* blocked writes */
    { *last = -1; uint32_t a[] = {(uint32_t)"ACH_TEST"}; r = vcall(us, 7, 1, a, &p); CHECK((r & 0xFF) == 1 && *last == -1 && p == 4, "SetAchievement blocked popped=%d", p); }
    { r = vcall(us, 10, 0, NULL, &p); CHECK((r & 0xFF) == 1 && *last == -1 && p == 0, "StoreStats blocked popped=%d", p); }
    { uint32_t a[] = {(uint32_t)"STAT", 5}; r = vcall(us, 3, 2, a, &p); CHECK(*last == -1 && p == 8, "SetStat(int) blocked popped=%d", p); }
    { float f = 1.5f; uint32_t fb; memcpy(&fb, &f, 4); uint32_t a[] = {(uint32_t)"STAT", fb}; r = vcall(us, 4, 2, a, &p); CHECK(*last == -1 && p == 8, "SetStat(float) popped=%d", p); }
    { uint32_t a[] = {(uint32_t)"STAT", 0, 0, 0}; r = vcall(us, 5, 4, a, &p); CHECK(*last == -1 && p == 16, "UpdateAvgRateStat popped=%d", p); }
    { uint32_t a[] = {(uint32_t)"ACH", 1, 2}; r = vcall(us, 13, 3, a, &p); CHECK(*last == -1 && p == 12, "IndicateAchievementProgress popped=%d", p); }

    printf("%d passed, %d failed\n", passes, fails);
    return fails != 0;
}
