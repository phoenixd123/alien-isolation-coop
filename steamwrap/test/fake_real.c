/* Fake "steam_api_real.dll": records which forwarded slot was hit and with which args. */
#include <windows.h>
#include <stdint.h>
#define THISCALL __attribute__((thiscall))
__declspec(dllexport) volatile int g_last_slot = -1;
__declspec(dllexport) volatile uint32_t g_last_arg;
typedef struct { void **vtbl; } Obj;
/* slot k: (this, uint32 a, uint32 b) -> returns 1000+k, pops 8 bytes */
#define S(k) static int THISCALL s##k(Obj *o, uint32_t a, uint32_t b) { (void)o; (void)b; g_last_slot = k; g_last_arg = a; return 1000 + k; }
S(0) S(1) S(2) S(3) S(4) S(5) S(6) S(7) S(8) S(9) S(10) S(11) S(12) S(13) S(14) S(15) S(16) S(17) S(18) S(19)
S(20) S(21) S(22) S(23) S(24) S(25) S(26) S(27) S(28) S(29) S(30) S(31) S(32) S(33) S(34) S(35) S(36) S(37) S(38) S(39)
static void *vt[40] = { s0,s1,s2,s3,s4,s5,s6,s7,s8,s9,s10,s11,s12,s13,s14,s15,s16,s17,s18,s19,
  s20,s21,s22,s23,s24,s25,s26,s27,s28,s29,s30,s31,s32,s33,s34,s35,s36,s37,s38,s39 };
static Obj rs = { vt }, us = { vt };
__declspec(dllexport) int __cdecl SteamAPI_Init(void) { return 1; }
__declspec(dllexport) void __cdecl SteamAPI_Shutdown(void) {}
__declspec(dllexport) void __cdecl SteamAPI_RunCallbacks(void) { g_last_slot = 900; }
__declspec(dllexport) void __cdecl SteamAPI_RegisterCallback(void *p, int i) { (void)p; g_last_slot = 901; g_last_arg = (uint32_t)i; }
__declspec(dllexport) void __cdecl SteamAPI_UnregisterCallback(void *p) { (void)p; }
__declspec(dllexport) void __cdecl SteamAPI_RegisterCallResult(void *p, uint64_t h) { (void)p; g_last_slot = 902; g_last_arg = (uint32_t)h; }
__declspec(dllexport) void __cdecl SteamAPI_UnregisterCallResult(void *p, uint64_t h) { (void)p; (void)h; }
__declspec(dllexport) void *__cdecl SteamUtils(void) { return (void *)0x11; }
__declspec(dllexport) void *__cdecl SteamFriends(void) { return (void *)0x12; }
__declspec(dllexport) void *__cdecl SteamUser(void) { return (void *)0x13; }
__declspec(dllexport) void *__cdecl SteamApps(void) { return (void *)0x14; }
__declspec(dllexport) void *__cdecl SteamRemoteStorage(void) { return &rs; }
__declspec(dllexport) void *__cdecl SteamUserStats(void) { return &us; }
