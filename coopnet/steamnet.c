/*
 * steamnet: coopnet's Steam P2P transport, through the game's own steam_api (renamed steam_api_real.dll).
 *
 * ISteamNetworking005 slots (Steamworks isteamnetworking.h; slots 0..7 match 005):
 *   0 SendP2PPacket(CSteamID, const void*, uint32, EP2PSend, int channel)
 *   1 IsP2PPacketAvailable(uint32*, int channel)
 *   2 ReadP2PPacket(void*, uint32, uint32*, CSteamID*, int channel)
 *   3 AcceptP2PSessionWithUser(CSteamID)      7 AllowP2PPacketRelay(bool)
 * ISteamUser017 slot 2 GetSteamID(); ISteamFriends014 slot 0 GetPersonaName().
 *
 * MSVC x86 ABI the game's steam_api uses: thiscall, callee pops; CSteamID by value is 8 bytes on the
 * stack; a member function returning CSteamID takes a hidden result pointer as its first stack arg.
 * CCallbackBase is { vtbl, uint8 flags, int iCallback }. MSVC puts overloaded virtuals in reverse
 * order, so the vtable is Run(void*, bool, SteamAPICall_t), Run(void*), GetCallbackSizeBytes()
 * (the same order Steamworks.NET uses for its CCallbackBase on Windows).
 */
#include <windows.h>
#include <stdint.h>
#include <string.h>
#include "steamnet.h"

#define THISCALL __attribute__((thiscall))
#define AIC_CHANNEL 7          /* our own P2P channel, away from 0 */
#define K_P2P_REQUEST 1202     /* P2PSessionRequest_t { CSteamID } */
#define K_P2P_FAIL 1203        /* P2PSessionConnectFail_t { CSteamID; uint8 error } */

typedef struct { void **vtbl; } Iface;
typedef int (THISCALL *SendFn)(Iface *, uint64_t, const void *, uint32_t, int, int);
typedef int (THISCALL *AvailFn)(Iface *, uint32_t *, int);
typedef int (THISCALL *ReadFn)(Iface *, void *, uint32_t, uint32_t *, uint64_t *, int);
typedef int (THISCALL *IdFn)(Iface *, uint64_t);
typedef int (THISCALL *BoolFn)(Iface *, int);
typedef uint64_t *(THISCALL *GetIdFn)(Iface *, uint64_t *);
typedef const char *(THISCALL *NameFn)(Iface *);

static Iface *g_net, *g_user, *g_friends;
static void (__cdecl *g_reg)(void *, int);
static steamnet_log_fn g_logf;
static uint64_t g_self, g_peer, g_want;
static char g_persona[64];
static int g_ready;

/* Callbacks run on the game thread inside SteamAPI_RunCallbacks; hand them to the net thread. */
static CRITICAL_SECTION g_cs;
static volatile uint64_t g_req_id, g_fail_id;
static volatile int g_req_n, g_fail_err = -1;

typedef struct { void **vtbl; uint8_t flags; uint8_t pad[3]; int32_t icb; } Callback;
static void THISCALL cb_run(Callback *c, void *param) {
    EnterCriticalSection(&g_cs);
    if (c->icb == K_P2P_REQUEST) { memcpy((void *)&g_req_id, param, 8); g_req_n++; }
    else if (c->icb == K_P2P_FAIL) { memcpy((void *)&g_fail_id, param, 8); g_fail_err = ((uint8_t *)param)[8]; }
    LeaveCriticalSection(&g_cs);
}
static void THISCALL cb_run_cr(Callback *c, void *param, int io_failure, uint64_t call) {
    (void)io_failure; (void)call; cb_run(c, param);
}
static int THISCALL cb_size(Callback *c) { return c->icb == K_P2P_FAIL ? 16 : 8; }
static void *g_cb_vtbl[3] = { (void *)cb_run_cr, (void *)cb_run, (void *)cb_size };
static Callback g_cb_req = { g_cb_vtbl, 0, {0}, K_P2P_REQUEST }, g_cb_fail = { g_cb_vtbl, 0, {0}, K_P2P_FAIL };

static int is_individual(uint64_t id) { return (id >> 32) == 0x01100001u && (uint32_t)id != 0; }

int steamnet_try_init(uint64_t peer, steamnet_log_fn logf) {
    if (g_ready) return 1;
    g_logf = logf;
    HMODULE real = GetModuleHandleA("steam_api_real.dll");
    if (!real) return 0;
    void *(__cdecl *acc_user)(void) = (void *)GetProcAddress(real, "SteamUser");
    void *(__cdecl *acc_net)(void) = (void *)GetProcAddress(real, "SteamNetworking");
    void *(__cdecl *acc_friends)(void) = (void *)GetProcAddress(real, "SteamFriends");
    g_reg = (void *)GetProcAddress(real, "SteamAPI_RegisterCallback");
    if (!acc_user || !acc_net || !g_reg) return 0;
    /* The accessors return NULL until the game's SteamAPI_Init has run. */
    g_user = acc_user(); g_net = acc_net(); g_friends = acc_friends ? acc_friends() : NULL;
    if (!g_user || !g_net) return 0;
    uint64_t id = 0; ((GetIdFn)g_user->vtbl[2])(g_user, &id);
    if (!is_individual(id)) { logf("steam: GetSteamID gave %llx, not a user id; not ready", (unsigned long long)id); return 0; }
    g_self = id;
    if (g_friends) { const char *n = ((NameFn)g_friends->vtbl[0])(g_friends); if (n) lstrcpynA(g_persona, n, sizeof g_persona); }
    InitializeCriticalSection(&g_cs);
    g_reg(&g_cb_req, K_P2P_REQUEST);
    g_reg(&g_cb_fail, K_P2P_FAIL);
    int relay = ((BoolFn)g_net->vtbl[7])(g_net, 1) & 0xFF;
    g_want = peer; g_peer = peer;
    if (peer) ((IdFn)g_net->vtbl[3])(g_net, peer);   /* replies from the host are welcome */
    logf("steam: ready, self=%llu relay=%d peer=%llu%s", (unsigned long long)g_self, relay,
         (unsigned long long)peer, peer ? "" : " (host: accepting the first player who connects)");
    g_ready = 1;
    return 1;
}

void steamnet_poll(void) {
    if (!g_ready) return;
    uint64_t req = 0, fail = 0; int nreq, ferr;
    EnterCriticalSection(&g_cs);
    nreq = g_req_n; g_req_n = 0; req = g_req_id; ferr = g_fail_err; g_fail_err = -1; fail = g_fail_id;
    LeaveCriticalSection(&g_cs);
    if (ferr >= 0) g_logf("steam: P2P connect to %llu failed (EP2PSessionError %d)", (unsigned long long)fail, ferr);
    if (!nreq) return;
    /* Host with no partner yet: take the first player who asks. Otherwise only the expected partner. */
    if ((!g_want && (!g_peer || req == g_peer)) || req == g_want) {
        int ok = ((IdFn)g_net->vtbl[3])(g_net, req) & 0xFF;
        if (req != g_peer) g_logf("steam: partner %llu connected (accept=%d)", (unsigned long long)req, ok);
        g_peer = req;
    } else {
        g_logf("steam: ignored a session request from %llu (not your partner)", (unsigned long long)req);
    }
}

int steamnet_send(const void *data, uint32_t n) {
    if (!g_ready || !g_peer) return 0;
    /* 1 = k_EP2PSendUnreliableNoDelay would drop until connected; 0 = unreliable lets Steam set up the session. */
    return ((SendFn)g_net->vtbl[0])(g_net, g_peer, data, n, 0, AIC_CHANNEL) & 0xFF;
}

int steamnet_recv(void *buf, uint32_t cap) {
    if (!g_ready) return 0;
    for (;;) {
        uint32_t sz = 0;
        if (!(((AvailFn)g_net->vtbl[1])(g_net, &sz, AIC_CHANNEL) & 0xFF)) return 0;
        uint8_t tmp[1200]; uint32_t got = 0; uint64_t from = 0;
        if (!(((ReadFn)g_net->vtbl[2])(g_net, tmp, sizeof tmp, &got, &from, AIC_CHANNEL) & 0xFF)) return 0;
        if (!g_peer || from != g_peer || got > cap) continue;
        memcpy(buf, tmp, got);
        return (int)got;
    }
}

void steamnet_set_peer(uint64_t id) {
    if (!g_ready) return;
    g_want = g_peer = id;
    ((IdFn)g_net->vtbl[3])(g_net, id);
    g_logf("steam: peer set to %llu", (unsigned long long)id);
}

uint64_t steamnet_self(void) { return g_self; }
uint64_t steamnet_peer(void) { return g_peer; }
const char *steamnet_persona(void) { return g_persona; }
