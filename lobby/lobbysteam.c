/* lobbysteam: Steam lobbies for the lobby window.
 *
 * The lobby window loads the game's own steam_api (steam_api_real.dll once the mod is installed) and starts it with
 * the game's app ID (steam_appid.txt, written by the installer), so to Steam you're playing Alien: Isolation while
 * the lobby is open. That's what makes the rest work:
 *   - Host: a friends-only Steam lobby for two (ISteamMatchmaking::CreateLobby), tagged "aic".
 *   - Your friends see it: a friend in one of the game's lobbies shows it in their game info (GetFriendGamePlayed),
 *     so the Internet tab lists friends who are hosting, with Join.
 *   - Invite: InviteUserToLobby sends a Steam invite. Accepting it while their lobby window is open joins
 *     (GameLobbyJoinRequested_t).
 *   - Once both are in the Steam lobby, the lobby's own packets (lobbyproto.h: HELLO, STATE, ready, start) and the
 *     save copy run over Steam P2P, relayed by Steam: no addresses, no port forwarding.
 *   - On Start, the window leaves the Steam lobby and lets go of Steam, then starts the game connected over Steam
 *     P2P to the other player (the game's own coopnet/steamnet.c), as play-coop.bat steam-host / steam-join do.
 *
 * Interfaces (the game's steam_api: SteamMatchMaking009, SteamFriends014, SteamNetworking005, SteamUser017), method
 * slots from the Steamworks headers (isteammatchmaking.h, isteamfriends014.h, isteamnetworking005.h); MSVC x86
 * thiscall, CSteamID by value is 8 bytes on the stack, a CSteamID return takes a hidden result pointer first.
 * Callback structs are packed to 8 on Windows.
 */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "lobbysteam.h"

#define THISCALL __attribute__((thiscall))
#define CH_LOBBY 6               /* lobby packets */
#define CH_SAVES 5               /* save copy stream (reliable) */
#define APP_ID 214490u
#define CB_GAME_LOBBY_JOIN 333   /* GameLobbyJoinRequested_t { CSteamID lobby, friend } */
#define CB_LOBBY_ENTER 504       /* LobbyEnter_t { uint64 lobby; uint32 perms; bool locked; uint32 response } */
#define CB_LOBBY_CREATED 513     /* LobbyCreated_t { EResult; uint64 lobby } */
#define CB_P2P_REQUEST 1202      /* P2PSessionRequest_t { CSteamID } */

typedef struct { void **vtbl; } Iface;
typedef uint64_t (THISCALL *CreateFn)(Iface *, int, int);
typedef uint64_t (THISCALL *JoinFn)(Iface *, uint64_t);
typedef void (THISCALL *LeaveFn)(Iface *, uint64_t);
typedef int (THISCALL *InviteFn)(Iface *, uint64_t, uint64_t);
typedef int (THISCALL *SetDataFn)(Iface *, uint64_t, const char *, const char *);
typedef uint64_t *(THISCALL *OwnerFn)(Iface *, uint64_t *, uint64_t);
typedef int (THISCALL *CountFn)(Iface *, int);
typedef uint64_t *(THISCALL *ByIndexFn)(Iface *, uint64_t *, int, int);
typedef int (THISCALL *StateFn)(Iface *, uint64_t);
typedef const char *(THISCALL *NameOfFn)(Iface *, uint64_t);
typedef int (THISCALL *GamePlayedFn)(Iface *, uint64_t, void *);
typedef const char *(THISCALL *NameFn)(Iface *);
typedef uint64_t *(THISCALL *GetIdFn)(Iface *, uint64_t *);
typedef int (THISCALL *SendFn)(Iface *, uint64_t, const void *, uint32_t, int, int);
typedef int (THISCALL *AvailFn)(Iface *, uint32_t *, int);
typedef int (THISCALL *ReadFn)(Iface *, void *, uint32_t, uint32_t *, uint64_t *, int);
typedef int (THISCALL *IdFn)(Iface *, uint64_t);
typedef int (THISCALL *BoolFn)(Iface *, int);

static void (*g_log)(const char *, ...);
static HMODULE g_dll;
static int (__cdecl *g_init)(void);
static void (__cdecl *g_run)(void);
static void (__cdecl *g_shut)(void);
static void (__cdecl *g_reg)(void *, int);
static void (__cdecl *g_unreg)(void *);
static Iface *g_mm, *g_fr, *g_net, *g_user;
static int g_up;
static uint64_t g_self, g_lobby, g_owner;
static int g_state;
static volatile uint64_t g_invite;

/* ---- callbacks (run inside SteamAPI_RunCallbacks, on our main thread) ---- */
typedef struct { void **vtbl; uint8_t flags; uint8_t pad[3]; int32_t icb; } Callback;
static void THISCALL cb_run(Callback *c, void *param) {
    uint8_t *p = (uint8_t *)param;
    switch (c->icb) {
    case CB_LOBBY_CREATED: {
        int32_t r; uint64_t id; memcpy(&r, p, 4); memcpy(&id, p + 8, 8);
        if (r == 1 && id) {
            g_lobby = id; g_owner = g_self; g_state = LBS_IN;
            ((SetDataFn)g_mm->vtbl[20])(g_mm, id, "aic", "1");
            if (g_log) g_log("steam: lobby %llu created", (unsigned long long)id);
        } else { g_state = LBS_FAILED; if (g_log) g_log("steam: couldn't create a lobby (EResult %d)", r); }
        break;
    }
    case CB_LOBBY_ENTER: {
        uint64_t id; uint32_t resp; memcpy(&id, p, 8); memcpy(&resp, p + 16, 4);
        if (g_state == LBS_JOINING) {
            if (resp == 1) {
                uint64_t o = 0; ((OwnerFn)g_mm->vtbl[35])(g_mm, &o, id);
                g_lobby = id; g_owner = o; g_state = LBS_IN;
                ((IdFn)g_net->vtbl[3])(g_net, o);
                if (g_log) g_log("steam: joined lobby %llu, host %llu", (unsigned long long)id, (unsigned long long)o);
            } else { g_state = LBS_FAILED; if (g_log) g_log("steam: couldn't join lobby %llu (response %u)", (unsigned long long)id, resp); }
        }
        break;
    }
    case CB_GAME_LOBBY_JOIN: { uint64_t id; memcpy(&id, p, 8); g_invite = id; break; }
    case CB_P2P_REQUEST: {
        uint64_t who; memcpy(&who, p, 8);
        /* in a lobby: anyone (the lobby is friends-only and two at most; lobbyproto turns away a third) */
        if (g_state == LBS_IN) ((IdFn)g_net->vtbl[3])(g_net, who);
        break;
    }
    }
}
static void THISCALL cb_run_cr(Callback *c, void *param, int io_failure, uint64_t call) { (void)io_failure; (void)call; cb_run(c, param); }
static int THISCALL cb_size(Callback *c) {
    return c->icb == CB_LOBBY_ENTER ? 24 : c->icb == CB_LOBBY_CREATED || c->icb == CB_GAME_LOBBY_JOIN ? 16 : 8;
}
static void *g_vt[3] = {(void *)cb_run_cr, (void *)cb_run, (void *)cb_size};
static Callback g_cbs[4] = {{g_vt, 0, {0}, CB_LOBBY_CREATED}, {g_vt, 0, {0}, CB_LOBBY_ENTER}, {g_vt, 0, {0}, CB_GAME_LOBBY_JOIN}, {g_vt, 0, {0}, CB_P2P_REQUEST}};

/* ---- save stream state (main thread moves bytes between Steam and these; the copy threads block on them) ---- */
static CRITICAL_SECTION g_scs;
static uint8_t *g_rx; static int g_rx_len, g_rx_cap;          /* bytes received on CH_SAVES */
static uint8_t *g_tx; static int g_tx_len, g_tx_cap;          /* bytes to send on CH_SAVES */
static uint64_t g_stream_peer;
static char g_serve_folder[MAX_PATH]; static uint32_t g_serve_lobby; static volatile LONG g_serving, g_fetching;

static void grow(uint8_t **b, int *cap, int need) {
    if (need <= *cap) return;
    int n = *cap ? *cap : 65536; while (n < need) n *= 2;
    *b = (uint8_t *)realloc(*b, (size_t)n); *cap = n;
}

int lbs_init(const char *game_dir, void (*logf)(const char *, ...)) {
    if (g_up) return 1;
    g_log = logf;
    InitializeCriticalSection(&g_scs);
    char p[MAX_PATH];
    snprintf(p, sizeof p, "%s\\steam_api_real.dll", game_dir);
    g_dll = LoadLibraryA(p);
    if (!g_dll) {   /* mod not installed: steam_api.dll is still the game's own (ours is never there without the real one) */
        char ap[MAX_PATH]; snprintf(ap, sizeof ap, "%s\\steam_api.dll", game_dir);
        g_dll = LoadLibraryA(ap);
    }
    if (!g_dll) { if (logf) logf("steam: no steam_api in the game folder"); return 0; }
    g_init = (void *)GetProcAddress(g_dll, "SteamAPI_Init");
    g_run = (void *)GetProcAddress(g_dll, "SteamAPI_RunCallbacks");
    g_shut = (void *)GetProcAddress(g_dll, "SteamAPI_Shutdown");
    g_reg = (void *)GetProcAddress(g_dll, "SteamAPI_RegisterCallback");
    g_unreg = (void *)GetProcAddress(g_dll, "SteamAPI_UnregisterCallback");
    void *(__cdecl *a_mm)(void) = (void *)GetProcAddress(g_dll, "SteamMatchmaking");
    void *(__cdecl *a_fr)(void) = (void *)GetProcAddress(g_dll, "SteamFriends");
    void *(__cdecl *a_net)(void) = (void *)GetProcAddress(g_dll, "SteamNetworking");
    void *(__cdecl *a_user)(void) = (void *)GetProcAddress(g_dll, "SteamUser");
    if (!g_init || !g_run || !g_shut || !g_reg || !a_mm || !a_fr || !a_net || !a_user) { if (logf) logf("steam: steam_api is missing functions"); return 0; }
    /* SteamAPI_Init reads steam_appid.txt from the working folder (the game folder; the installer writes it) */
    char was[MAX_PATH]; GetCurrentDirectoryA(sizeof was, was); SetCurrentDirectoryA(game_dir);
    SetEnvironmentVariableA("SteamAppId", "214490"); SetEnvironmentVariableA("SteamGameId", "214490");
    int ok = g_init() & 0xFF;
    SetCurrentDirectoryA(was);
    if (!ok) { if (logf) logf("steam: SteamAPI_Init failed (is Steam running and logged in?)"); return 0; }
    g_mm = a_mm(); g_fr = a_fr(); g_net = a_net(); g_user = a_user();
    if (!g_mm || !g_fr || !g_net || !g_user) { g_shut(); return 0; }
    ((GetIdFn)g_user->vtbl[2])(g_user, &g_self);
    for (int i = 0; i < 4; i++) g_reg(&g_cbs[i], g_cbs[i].icb);
    ((BoolFn)g_net->vtbl[7])(g_net, 1);   /* relay through Steam when a direct path fails */
    g_up = 1; g_state = LBS_IDLE;
    if (logf) logf("steam: up, you are %llu", (unsigned long long)g_self);
    return 1;
}

void lbs_shutdown(void) {
    if (!g_up) return;
    lbs_leave();
    for (int i = 0; i < 4; i++) if (g_unreg) g_unreg(&g_cbs[i]);
    g_shut();
    g_up = 0; g_state = LBS_OFF;
}

int lbs_ready(void) { return g_up; }
uint64_t lbs_self(void) { return g_up ? g_self : 0; }
int lbs_state(void) { return g_up ? g_state : LBS_OFF; }
uint64_t lbs_lobby(void) { return g_state == LBS_IN ? g_lobby : 0; }
uint64_t lbs_owner(void) { return g_state == LBS_IN ? g_owner : 0; }
uint64_t lbs_take_invite(void) { uint64_t v = g_invite; g_invite = 0; return v; }

int lbs_host(void) {
    if (!g_up || g_state == LBS_CREATING || g_state == LBS_JOINING) return 0;
    lbs_leave();
    ((CreateFn)g_mm->vtbl[13])(g_mm, 1 /* friends only */, 2);
    g_state = LBS_CREATING;
    return 1;
}
int lbs_join(uint64_t lobby) {
    if (!g_up || !lobby) return 0;
    lbs_leave();
    ((JoinFn)g_mm->vtbl[14])(g_mm, lobby);
    g_state = LBS_JOINING;
    return 1;
}
void lbs_leave(void) {
    if (!g_up) return;
    if (g_lobby) ((LeaveFn)g_mm->vtbl[15])(g_mm, g_lobby);
    g_lobby = g_owner = 0;
    if (g_state != LBS_OFF) g_state = LBS_IDLE;
}
int lbs_invite(uint64_t f) { return g_up && g_state == LBS_IN && (((InviteFn)g_mm->vtbl[16])(g_mm, g_lobby, f) & 0xFF); }

int lbs_friends(LbsFriend *out, int max) {
    if (!g_up) return 0;
    int n = ((CountFn)g_fr->vtbl[3])(g_fr, 4 /* immediate: real friends */), k = 0;
    for (int pass = 0; pass < 2; pass++)   /* the ones hosting first */
        for (int i = 0; i < n && k < max; i++) {
            uint64_t id = 0; ((ByIndexFn)g_fr->vtbl[4])(g_fr, &id, i, 4);
            if (!id || !((StateFn)g_fr->vtbl[6])(g_fr, id)) continue;   /* offline */
            struct { uint64_t game; uint32_t ip; uint16_t port, qport; uint64_t lobby; } gi; memset(&gi, 0, sizeof gi);
            int playing = ((GamePlayedFn)g_fr->vtbl[8])(g_fr, id, &gi) & 0xFF;
            int hosting = playing && (uint32_t)(gi.game & 0xFFFFFFu) == APP_ID && gi.lobby;
            if (hosting != (pass == 0)) continue;
            LbsFriend *f = &out[k++];
            memset(f, 0, sizeof *f);
            f->id = id; f->online = 1; f->hosting = hosting; f->lobby = hosting ? gi.lobby : 0;
            const char *nm = ((NameOfFn)g_fr->vtbl[7])(g_fr, id);
            lstrcpynA(f->name, nm ? nm : "?", sizeof f->name);
        }
    return k;
}

/* ---- P2P ---- */
void lbs_send(void *ctx, LbAddr dest, const void *buf, int len) {
    (void)ctx;
    if (!g_up || !lbs_is_steam(dest)) return;   /* broadcasts are the LAN's */
    ((SendFn)g_net->vtbl[0])(g_net, lbs_id(dest), buf, (uint32_t)len, 0 /* unreliable: the protocol resends */, CH_LOBBY);
}
int lbs_recv(LbAddr *from, void *buf, int cap) {
    if (!g_up) return 0;
    uint32_t sz = 0;
    while (((AvailFn)g_net->vtbl[1])(g_net, &sz, CH_LOBBY) & 0xFF) {
        uint8_t tmp[1200]; uint32_t got = 0; uint64_t who = 0;
        if (!(((ReadFn)g_net->vtbl[2])(g_net, tmp, sizeof tmp, &got, &who, CH_LOBBY) & 0xFF)) return 0;
        if (got > (uint32_t)cap || (who >> 32) != 0x01100001u) continue;
        memcpy(buf, tmp, got); *from = lbs_addr(who);
        return (int)got;
    }
    return 0;
}

static DWORD WINAPI serve_thread(LPVOID u);

void lbs_tick(void) {
    if (!g_up) return;
    g_run();
    /* save stream: Steam <-> the buffers */
    uint32_t sz = 0;
    while (((AvailFn)g_net->vtbl[1])(g_net, &sz, CH_SAVES) & 0xFF) {
        static uint8_t tmp[1 << 17]; uint32_t got = 0; uint64_t who = 0;
        if (sz > sizeof tmp || !(((ReadFn)g_net->vtbl[2])(g_net, tmp, sizeof tmp, &got, &who, CH_SAVES) & 0xFF)) break;
        EnterCriticalSection(&g_scs);
        if (!g_stream_peer || who == g_stream_peer) {
            g_stream_peer = who;
            grow(&g_rx, &g_rx_cap, g_rx_len + (int)got); memcpy(g_rx + g_rx_len, tmp, got); g_rx_len += (int)got;
        }
        LeaveCriticalSection(&g_scs);
        /* host: a request coming in starts the serving thread */
        if (g_state == LBS_IN && g_owner == g_self && g_serve_folder[0] && !g_serving && !g_fetching) {
            InterlockedExchange(&g_serving, 1);
            HANDLE h = CreateThread(NULL, 0, serve_thread, NULL, 0, NULL);
            if (h) CloseHandle(h); else InterlockedExchange(&g_serving, 0);
        }
    }
    EnterCriticalSection(&g_scs);
    int off = 0;
    while (g_stream_peer && off < g_tx_len) {
        int n = g_tx_len - off > 60000 ? 60000 : g_tx_len - off;
        if (!(((SendFn)g_net->vtbl[0])(g_net, g_stream_peer, g_tx + off, (uint32_t)n, 2 /* reliable */, CH_SAVES) & 0xFF)) break;
        off += n;
    }
    if (off) { memmove(g_tx, g_tx + off, (size_t)(g_tx_len - off)); g_tx_len -= off; }
    LeaveCriticalSection(&g_scs);
}

static int st_send(LbStream *st, const void *p, int n) {
    (void)st;
    EnterCriticalSection(&g_scs);
    grow(&g_tx, &g_tx_cap, g_tx_len + n); memcpy(g_tx + g_tx_len, p, (size_t)n); g_tx_len += n;
    LeaveCriticalSection(&g_scs);
    return 1;
}
static int st_recv(LbStream *st, void *p, int n) {
    (void)st;
    DWORD t0 = GetTickCount();
    for (;;) {
        EnterCriticalSection(&g_scs);
        if (g_rx_len >= n) {
            memcpy(p, g_rx, (size_t)n); memmove(g_rx, g_rx + n, (size_t)(g_rx_len - n)); g_rx_len -= n;
            LeaveCriticalSection(&g_scs);
            return 1;
        }
        LeaveCriticalSection(&g_scs);
        if (GetTickCount() - t0 > 20000) return 0;
        Sleep(5);
    }
}
static void stream_reset(uint64_t peer) {
    EnterCriticalSection(&g_scs); g_rx_len = g_tx_len = 0; g_stream_peer = peer; LeaveCriticalSection(&g_scs);
}

static DWORD WINAPI serve_thread(LPVOID u) {
    (void)u;
    LbStream st = {st_send, st_recv, NULL};
    lbnet_serve_stream(&st, g_serve_folder, g_serve_lobby);
    /* let the reliable sends drain before taking another request */
    for (int i = 0; i < 400; i++) { EnterCriticalSection(&g_scs); int left = g_tx_len; LeaveCriticalSection(&g_scs); if (!left) break; Sleep(25); }
    stream_reset(0);
    InterlockedExchange(&g_serving, 0);
    return 0;
}
void lbs_serve_saves(const char *folder, uint32_t lobby) {
    lstrcpynA(g_serve_folder, folder ? folder : "", sizeof g_serve_folder); g_serve_lobby = lobby;
    stream_reset(0);
}

typedef struct { uint32_t lobby; char folder[MAX_PATH], backup[MAX_PATH]; LbCopy *c; } Fetch;
static DWORD WINAPI fetch_thread(LPVOID u) {
    Fetch *F = (Fetch *)u;
    LbStream st = {st_send, st_recv, NULL};
    lbnet_fetch_stream(&st, F->lobby, F->folder, F->backup, F->c);
    stream_reset(0);
    InterlockedExchange(&g_fetching, 0);
    free(F);
    return 0;
}
int lbs_fetch_saves(uint64_t host, uint32_t lobby, const char *folder, const char *backup_root, LbCopy *c) {
    if (!g_up || c->state == LBC_RUNNING || g_fetching) return 0;
    memset(c, 0, sizeof *c); c->state = LBC_RUNNING;
    Fetch *F = (Fetch *)calloc(1, sizeof *F);
    F->lobby = lobby; F->c = c;
    lstrcpynA(F->folder, folder, MAX_PATH); lstrcpynA(F->backup, backup_root, MAX_PATH);
    stream_reset(host);
    InterlockedExchange(&g_fetching, 1);
    HANDLE h = CreateThread(NULL, 0, fetch_thread, F, 0, NULL);
    if (!h) { free(F); InterlockedExchange(&g_fetching, 0); lstrcpynA(c->msg, "COULD NOT START THE COPY", sizeof c->msg); c->state = LBC_FAILED; return 0; }
    CloseHandle(h);
    return 1;
}
