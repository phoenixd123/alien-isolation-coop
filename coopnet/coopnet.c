/*
 * coopnet - the co-op side of the mod, loaded by steamwrap. Steam build 4940658 only.
 *
 * Camera record:
 *   mgr = [exe+0x12F0C88], blk = [mgr+0x58], rec = blk+0x78C
 *   rec: quat x,y,z,w | pos x,y,z | fov 47 | near 0.05 | far 1000
 *
 * Modes (AIC_COOP_MODE): host / join over UDP 45500, steam-host / steam-join over Steam P2P.
 * With no mode, p1 and p2 talk to each other on localhost (45500/45501).
 * AIC_COUCH_SEAT=<n>: Couchlink seat, 0 hosts and the rest join 127.0.0.1.
 */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include "proto.h"
#include "view.h"
#include "steamnet.h"
#include "worldsync.h"
#include "replay.h"
#include "life.h"
#include "hud.h"
#include "sfx.h"
#include "ui.h"
#include "skin.h"
#include "skins.h"
#include "version.h"
#include "cutscene.h"
#include "engine.h"
#include "alien.h"
#include <stdlib.h>
#include <dwmapi.h>

int probe_install(const char *dir, const char *profile, void (*logf)(const char *, ...));
void probe_set_gameover_cb(int (*cb)(uint32_t hb, const uint32_t *msg));
void probe_set_tick_cb(void (*cb)(void));
void probe_set_swallow(int (*cb)(uint32_t method, uint32_t eid, uint32_t type));
void puppet_tick(int in_level, int peer_live, const float peer_cam_pos[3], void (*logf)(const char *, ...));

static FILE *g_log, *g_csv;
static int g_skins;              /* AIC_SKINS=1: partner body as a story character (skin.c) */
static int g_share_alien;        /* AIC_SHARED_ALIEN=1: one alien for both (alien.c; skin.c's position snapping if that won't hook) */
static int g_alien_full;         /* alien.c is on */
static char g_my_skin[24] = "TAYLOR";   /* AIC_SKIN: how the partner sees you */
static char g_profile[32] = "default";
static char g_name[16] = "partner";
static DWORD g_pid;
static volatile LONG g_quit;
static char g_dir[MAX_PATH];

static void logf_(const char *fmt, ...) {
    if (!g_log) return;
    SYSTEMTIME t; GetLocalTime(&t);
    fprintf(g_log, "%02d:%02d:%02d.%03d ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list a; va_start(a, fmt); vfprintf(g_log, fmt, a); va_end(a);
    fputc('\n', g_log); fflush(g_log);
}

/* ---------- reading the camera record ---------- */
#define RVA_CAM_MGR 0x12F0C88u
static int safe_read(uintptr_t addr, void *out, size_t n) {
    SIZE_T got = 0;
    return addr > 0x10000 && ReadProcessMemory(GetCurrentProcess(), (LPCVOID)addr, out, n, &got) && got == n;
}

typedef struct { float q[4], p[3]; int ok; float raw[16]; } Cam;
static int read_cam(Cam *c) {
    uintptr_t base = (uintptr_t)GetModuleHandleW(NULL);
    uint32_t mgr = 0, blk = 0;
    memset(c, 0, sizeof *c);
    if (!safe_read(base + RVA_CAM_MGR, &mgr, 4) || !mgr) return 0;
    if (!safe_read(mgr + 0x58, &blk, 4) || !blk) return 0;
    if (!safe_read(blk + 0x78C, c->raw, sizeof c->raw)) return 0;
    if (fabsf(c->raw[7] - 47.0f) > 0.01f || fabsf(c->raw[8] - 0.05f) > 1e-4f || fabsf(c->raw[9] - 1000.0f) > 0.1f) return 0;
    memcpy(c->q, c->raw, 16); memcpy(c->p, c->raw + 4, 12);
    float n = c->q[0]*c->q[0] + c->q[1]*c->q[1] + c->q[2]*c->q[2] + c->q[3]*c->q[3];
    c->ok = (n > 0.9f && n < 1.1f) ? 2 : 1;   /* 2 = quaternion looks valid */
    return 1;
}

/* ---------- shared state ---------- */
typedef struct { float p[3], q[4]; DWORD t_ms; int valid; char name[16]; uint32_t seq; } Peer;
static CRITICAL_SECTION g_cs;
static Cam g_me;
static Peer g_peer;
static uint32_t g_rx, g_tx;

static HWND g_game, g_ov;   /* the game's window and our overlay (overlay thread) */

/* couch / roles */
static int g_couch, g_seat = -1, g_is_host;

/* co-op death (life.c); all under g_cs */
static Life g_life;
static volatile LONG g_go_held, g_go_release;
static volatile LONG g_follow_on;   /* spectate camera follows the partner (see follow_step) */
static DWORD g_dead_t, g_load_until;  /* when we died; press "Load Current Save" until then (0 = no) */
static float g_dead_pos[3]; static int g_dead_pos_ok, g_qa_dead;
static volatile LONG g_life_dirty;
static uint32_t g_peer_pid; static int g_peer_couch;
static DWORD g_flash_t, g_both_t;      /* partner-died flash, "both down" banner */
/* cutscene teleport (cutscene.h): AIC_CUTSCENE_TP=0 turns it off */
static int g_cut_on = 1;
static CutTrail g_trail;                 /* our recent positions (g_cs) */
static volatile LONG g_cut_local;       /* a cutscene started here; the net thread sends */
static DWORD g_cut_sent_t, g_cut_recv_t, g_ui_cut_t; static uint32_t g_cut_seen_id;
static float g_tp_pos[3]; static volatile LONG g_tp_pending; static int g_tp_kind;   /* 0 cutscene, 1 regroup */
static volatile DWORD g_own_tp_t;          /* when the mod last moved us (carry-along ignores that jump) */
static volatile DWORD g_level_t0;          /* when we last entered a level */
static uint32_t g_carry_seen_id;
static DWORD g_tp_at, g_fade_t0;         /* teleport after the fade-out (cutscene.h) */
static volatile LONG g_regroup_req; static volatile float g_regroup_prog;   /* F7 */
static uint32_t g_regroup_id; static DWORD g_regroup_ask_t, g_ui_regroup_t, g_coming_t;
static uint32_t g_mod_hash, g_game_build; static volatile LONG g_build_bad;   /* BuildPkt */
static int g_spec_test;                /* AIC_SPECTATE_TEST=1: show the spectate view without dying (QA) */
static DWORD g_life_test_ms, g_t0;     /* AIC_LIFE_TEST=<s>: run the death rules <s> seconds after start (QA) */

/* alien and motion contacts (character table) */
#define RVA_CHAR_POS0 0x1307AC8u
static int g_alien_tag = 3, g_alien_pin;
static float g_my_alien[3]; static int g_my_alien_ok;
static float g_peer_alien[3]; static int g_peer_alien_ok; static DWORD g_peer_alien_t; static uint32_t g_peer_alien_seq;
static TrackSlot g_track[TRACK_SLOTS];
static float g_track_pos[TRACK_SLOTS][3]; static uint32_t g_track_tag[TRACK_SLOTS];

/* ---------- networking ---------- */
static SOCKET g_sock = INVALID_SOCKET;
static struct sockaddr_in g_peer_addr;
static int g_peer_fixed, g_learn_peer;
static int g_steam;                   /* 1 = Steam P2P transport */
static uint64_t g_steam_peer;
static int g_name_set;

static int resolve(const char *hostport, struct sockaddr_in *out) {
    char host[128]; const char *c = strrchr(hostport, ':'); if (!c) return 0;
    size_t hl = (size_t)(c - hostport); if (hl >= sizeof host) return 0;
    memcpy(host, hostport, hl); host[hl] = 0;
    struct addrinfo hints = {0}, *res = NULL; hints.ai_family = AF_INET; hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(host, c + 1, &hints, &res) || !res) return 0;
    memcpy(out, res->ai_addr, sizeof *out); freeaddrinfo(res); return 1;
}

static void net_init(void) {
    WSADATA w; WSAStartup(MAKEWORD(2, 2), &w);
    char buf[160], mode[16] = ""; int is_p1 = !strcmp(g_profile, "p1") || !strcmp(g_profile, "default");
    GetEnvironmentVariableA("AIC_COOP_MODE", mode, sizeof mode);
    if (g_couch && !mode[0]) {   /* Couchlink seat: 0 hosts, the rest join on this PC */
        lstrcpynA(mode, g_seat == 0 ? "host" : "join", sizeof mode);
        if (g_seat != 0 && !GetEnvironmentVariableA("AIC_COOP_PEER", buf, sizeof buf)) SetEnvironmentVariableA("AIC_COOP_PEER", "127.0.0.1:45500");
        logf_("couch: seat %d -> %s", g_seat, mode);
    }
    g_is_host = !strcmp(mode, "host") || !strcmp(mode, "steam-host") || (!mode[0] && is_p1);
    ws_set_host(g_is_host);
    if (g_alien_full) alien_set_host(g_is_host);
    logf_("role: %s", g_is_host ? "host (owns the alien director)" : "joiner");
    if (!strncmp(mode, "steam-", 6)) {
        g_steam = 1;
        if (!strcmp(mode, "steam-join")) {
            if (!GetEnvironmentVariableA("AIC_COOP_PEER_STEAM", buf, sizeof buf)) buf[0] = 0;
            g_steam_peer = !strcmp(buf, "self") ? 1 : _strtoui64(buf, NULL, 10);
            if (!g_steam_peer) logf_("net: steam-join needs AIC_COOP_PEER_STEAM=<SteamID64>");
        }
        logf_("net: Steam P2P transport (%s), waiting for Steam", mode);
        return;
    }
    int port = is_p1 ? 45500 : 45501;
    const char *peer = is_p1 ? "127.0.0.1:45501" : "127.0.0.1:45500";
    if (!strcmp(mode, "host")) { port = 45500; peer = NULL; g_learn_peer = 1; }   /* reply to whoever joins */
    else if (!strcmp(mode, "join")) port = 0;                                      /* any local port; peer = host */
    if (GetEnvironmentVariableA("AIC_COOP_PORT", buf, sizeof buf)) port = atoi(buf);
    if (GetEnvironmentVariableA("AIC_COOP_PEER", buf, sizeof buf)) { peer = buf; g_learn_peer = 0; }
    g_peer_fixed = peer ? resolve(peer, &g_peer_addr) : 0;
    if (!peer) peer = "(learned from first packet)";
    g_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in a = {0}; a.sin_family = AF_INET; a.sin_port = htons((u_short)port); a.sin_addr.s_addr = htonl(INADDR_ANY);
    int b = bind(g_sock, (struct sockaddr *)&a, sizeof a);
    u_long nb = 1; ioctlsocket(g_sock, FIONBIO, &nb);
    logf_("net: bind %d -> %s, peer %s -> %s", port, b == 0 ? "ok" : "FAIL", peer, g_peer_fixed ? "resolved" : "UNRESOLVED");
}

static void steam_up(void) {
    if (steamnet_self()) return;
    if (!steamnet_try_init(g_steam_peer == 1 ? 0 : g_steam_peer, logf_)) return;
    if (g_steam_peer == 1) steamnet_set_peer(steamnet_self());   /* "self": loopback test on one account */
    if (!g_name_set && steamnet_persona()[0]) lstrcpynA(g_name, steamnet_persona(), sizeof g_name);
    char p[MAX_PATH + 64]; snprintf(p, sizeof p, "%s\\coop_logs\\my_steam_id.txt", g_dir);
    FILE *f = fopen(p, "w"); if (f) { fprintf(f, "%llu\n", (unsigned long long)steamnet_self()); fclose(f); }
}

static void net_raw(const void *d, int n) {
    if (g_steam) { steamnet_send(d, (uint32_t)n); return; }
    if (g_sock != INVALID_SOCKET && g_peer_fixed) sendto(g_sock, (const char *)d, n, 0, (struct sockaddr *)&g_peer_addr, sizeof g_peer_addr);
}

/* World events from worldsync.c: each one goes out three times (unreliable transport); the receiver drops repeats. */
static uint32_t g_wseq, g_wtx, g_wrx, g_wsession;
static void net_send_world(void) {
    WorldEvt ev;
    while (ws_pop(&ev)) {
        if (!g_wsession) g_wsession = (GetTickCount() * 2654435761u) ^ (g_pid << 16) ^ 1u;
        WorldPkt k; proto_fill_world(&k, g_wsession, ++g_wseq, ev.method, ev.eid, ev.type, ev.path, ev.npath);
        for (int i = 0; i < 3; i++) net_raw(&k, sizeof k);
        g_wtx++;
        logf_("world: sent #%u method %08x entity %08x path %08x/%08x (%d up)", k.seq, ev.method, ev.eid, ev.path[0], ev.path[1], ev.npath);
    }
}

static uint32_t g_wseen[64], g_wpeer_session;
/* Partner's world events wait until our own level has been running a few seconds: replayed while it is still
   starting up, they reach script entities that aren't finished yet and the game crashes (found in game: the joiner
   loading into a level the host was already playing). Held in order, applied once the level is ready. */
#define LEVEL_READY_MS 8000
static volatile LONG g_lvl_ready;
static WorldEvt g_whold[256]; static int g_whold_n;   /* g_cs */
static void world_flush(void) {
    WorldEvt evs[256]; int n;
    EnterCriticalSection(&g_cs); n = g_whold_n; memcpy(evs, g_whold, (size_t)n * sizeof *evs); g_whold_n = 0; LeaveCriticalSection(&g_cs);
    if (n) logf_("world: level ready, applying %d held partner event%s", n, n == 1 ? "" : "s");
    for (int i = 0; i < n; i++) ws_apply(&evs[i]);
}
static void on_world(const WorldPkt *k) {
    if (k->session != g_wpeer_session) { memset(g_wseen, 0, sizeof g_wseen); g_wpeer_session = k->session; }   /* partner restarted */
    for (int i = 0; i < 64; i++) if (g_wseen[i] == k->seq) return;
    static int wi; g_wseen[wi++ & 63] = k->seq;
    WorldEvt ev; memset(&ev, 0, sizeof ev);
    ev.method = k->method; ev.eid = k->eid; ev.type = k->type; ev.npath = k->npath; memcpy(ev.path, k->path, sizeof k->path);
    g_wrx++;
    logf_("world: got #%u method %08x entity %08x path %08x/%08x (%d up)%s", k->seq, ev.method, ev.eid, ev.path[0], ev.path[1], ev.npath,
          g_lvl_ready ? "" : ", held until our level is ready");
    if (!g_lvl_ready) {
        EnterCriticalSection(&g_cs);
        if (g_whold_n < 256) g_whold[g_whold_n++] = ev;
        LeaveCriticalSection(&g_cs);
        return;
    }
    ws_apply(&ev);
}

/* ---------- co-op death ---------- */
/* The player died (call_method hook, game thread; see probe.c). The game now waits on its own game-over menu. */
static int on_gameover(uint32_t hb, const uint32_t *msg) {
    (void)hb;
    DWORD now = GetTickCount();
    EnterCriticalSection(&g_cs);
    int was = g_life.state;
    if (g_dead_t && now - g_dead_t < 8000) { LeaveCriticalSection(&g_cs); return 0; }   /* the engine reports a death twice */
    int wait = life_on_gameover(&g_life, now);
    int st = g_life.state;
    if (was != LIFE_DEAD) { g_dead_t = now ? now : 1; g_dead_pos_ok = 0; }
    g_qa_dead = !msg || !msg[0];
    /* Partner already down: pick "Load Current Save" as soon as the menu is up. Solo (no partner): the menu is the
       player's, as in the normal game. */
    if (!wait && g_life.peer_known && life_peer_live(&g_life, now) && !g_qa_dead) g_load_until = now + 45000;
    if (!wait) InterlockedExchange(&g_go_release, 1);
    LeaveCriticalSection(&g_cs);
    if (was == LIFE_DEAD) return 0;
    InterlockedExchange(&g_life_dirty, 1);
    logf_("life: player died (%s) -> %s", msg && msg[0] == 0xae498037u ? "TriggerDeath" : "GameOver",
          wait ? "waiting on the game-over menu, spectating the partner" : st == LIFE_RESTART ? "back to the checkpoint" : "?");
    return 0;
}

static void send_life(int force) {
    static DWORD last;
    DWORD now = GetTickCount();
    int dirty = InterlockedExchange(&g_life_dirty, 0);
    if (!force && !dirty && now - last < 500) return;
    last = now;
    EnterCriticalSection(&g_cs); int st = g_life.state; uint32_t ep = g_life.epoch; LeaveCriticalSection(&g_cs);
    LifePkt k; proto_fill_life(&k, st, ep, g_pid, (g_couch ? 1 : 0) | (g_is_host ? 2 : 0));
    for (int i = 0; i < (dirty ? 3 : 1); i++) net_raw(&k, sizeof k);
}

static void on_life(const LifePkt *k) {
    EnterCriticalSection(&g_cs);
    life_on_peer(&g_life, k->state, k->epoch, GetTickCount());
    g_peer_pid = k->pid; g_peer_couch = k->flags & 1;
    LeaveCriticalSection(&g_cs);
}

/* "Load Current Save" is the default item on the game-over menu, so a posted Enter picks it (no focus needed).
   Repeated every 2.5 s until the camera leaves the death spot. */
static void press_load(void) {
    if (!g_game) return;
    PostMessageA(g_game, WM_KEYDOWN, VK_RETURN, 1 | (0x1C << 16));
    Sleep(60);
    PostMessageA(g_game, WM_KEYUP, VK_RETURN, (LPARAM)(1u | (0x1Cu << 16) | (3u << 30)));
}

/* Net thread: run the life rules; go back to the checkpoint when they say so; spot a reload; sound and HUD events. */
static void life_step(int give_up, const Cam *c) {
    DWORD now = GetTickCount();
    EnterCriticalSection(&g_cs);
    int before = g_life.state;
    /* where the body lies: the camera drops to the floor in the first seconds after death */
    /* While following, our camera record holds the partner's pose: no respawn detection then. Once following
       stops, the record shows our death camera again; take the death spot from it 0.7 s later. */
    static DWORD capture_t;
    if (g_follow_on) { g_dead_pos_ok = 0; capture_t = now + 700; }
    else if ((before == LIFE_DEAD || g_load_until) && !g_dead_pos_ok && now - g_dead_t > 3000 && (int32_t)(now - capture_t) >= 0 && c->ok) {
        memcpy(g_dead_pos, c->p, 12); g_dead_pos_ok = 1;
    }
    int moved = 0;
    if (g_dead_pos_ok && c->ok && !g_follow_on) {
        float dx = c->p[0] - g_dead_pos[0], dy = c->p[1] - g_dead_pos[1], dz = c->p[2] - g_dead_pos[2];
        moved = dx * dx + dy * dy + dz * dz > 0.25f;
    }
    int self_respawn = moved && before == LIFE_DEAD;
    if (self_respawn) { life_on_respawn(&g_life, now); InterlockedExchange(&g_go_release, 1); }
    if (moved) { g_dead_pos_ok = 0; g_load_until = 0; }
    int rel = life_tick(&g_life, now, give_up);
    if (rel && !g_qa_dead) g_load_until = now + 45000;
    if (rel) InterlockedExchange(&g_go_release, 1);
    int died = g_life.ev_peer_died, both = g_life.ev_both_down;
    g_life.ev_peer_died = g_life.ev_both_down = 0;
    if (died) g_flash_t = now;
    if (both) g_both_t = now;
    int after = g_life.state;
    DWORD load_until = g_load_until;
    LeaveCriticalSection(&g_cs);
    if (before != after) { InterlockedExchange(&g_life_dirty, 1); logf_("life: %d -> %d", before, after); }
    if (self_respawn) logf_("life: the player picked a save on the game-over menu");
    if (moved && !self_respawn) logf_("life: back at the checkpoint (camera left the death spot)");
    if (rel) logf_("life: going back to the checkpoint%s", give_up ? " (player gave up)" : "");
    static DWORD next_press;
    if (load_until && (int32_t)(load_until - now) > 0 && now - g_dead_t > 4000 && (int32_t)(now - next_press) >= 0) {
        press_load(); next_press = now + 2500;
        logf_("life: pressed Load Current Save");
    }
    if (died) { sfx_play(SFX_STING); logf_("life: partner died"); }
    if (both) logf_("life: both down, back to the checkpoint");
}

/* ---------- spectate camera ----------
 * Writing the player camera does nothing (it's rebuilt from the character every frame), but while dead the
 * KillCamera script camera is active and renders from its camera records. So we find those records and keep
 * writing the partner's pose into them. Menu is held (show_go_hook), fade and blur are swallowed (swallow_cb).
 */
static float g_follow[7];            /* q[4], p[3]; written by the net thread, read by writers (tearing is harmless) */
static int g_follow_mode = 1;        /* AIC_SPECTATE_CAM=0 turns camera follow off (bio-monitor / thumbnail instead) */
static int g_nofade;                 /* swallow TriggerDeath while we spectate (no fade to black); AIC_DEATH_NOFADE=0 keeps it */

/* Script cameras keep several copies of their record (current, previous, blend); all of them need the pose. */
#define NCAMREC 16
static uint32_t g_camrec[NCAMREC]; static volatile LONG g_ncamrec;
static int camrec_ok(const float *r) {
    float n = r[0] * r[0] + r[1] * r[1] + r[2] * r[2] + r[3] * r[3];
    if (!(n > 0.98f && n < 1.02f)) return 0;
    for (int i = 4; i < 7; i++) if (!(r[i] == r[i]) || fabsf(r[i]) > 10000.f) return 0;
    return r[7] >= 5.f && r[7] <= 120.f && r[8] == 0.05f && r[9] == 1000.0f;
}
static int near3(const float *a, const float *b, float tol) {
    return fabsf(a[0] - b[0]) < tol && fabsf(a[1] - b[1]) < tol && fabsf(a[2] - b[2]) < tol;
}
/* Records at the death camera (within 1 m of `pose`) or already holding the pose we write; merged with the list. */
static void camrec_scan(const float pose[7]) {
    static float buf[1 << 18];
    uint32_t found[NCAMREC]; int n = 0;
    MEMORY_BASIC_INFORMATION m;
    for (uintptr_t a = 0x10000; a < 0x7FFF0000 && n < NCAMREC && VirtualQuery((LPCVOID)a, &m, sizeof m); a = (uintptr_t)m.BaseAddress + m.RegionSize) {
        if (m.State != MEM_COMMIT || m.Type != MEM_PRIVATE || (m.Protect & (PAGE_GUARD | PAGE_NOACCESS)) || !(m.Protect & PAGE_READWRITE)) continue;
        for (uintptr_t o = 0; o < m.RegionSize && n < NCAMREC; o += sizeof buf - 64) {
            size_t len = m.RegionSize - o < sizeof buf ? m.RegionSize - o : sizeof buf;
            SIZE_T got = 0;
            if (!ReadProcessMemory(GetCurrentProcess(), (LPCVOID)((uintptr_t)m.BaseAddress + o), buf, len, &got)) continue;
            size_t nf = got / 4;
            for (size_t i = 9; i < nf && n < NCAMREC; i++)
                if (buf[i] == 1000.0f && buf[i - 1] == 0.05f && camrec_ok(buf + i - 9) &&
                    (near3(buf + i - 5, pose + 4, 1.0f) || (g_follow_on && near3(buf + i - 5, g_follow + 4, 0.01f))))
                    found[n++] = (uint32_t)((uintptr_t)m.BaseAddress + o + (i - 9) * 4);
            if (len < sizeof buf) break;
        }
    }
    if (n > 12) { logf_("spectate: %d camera records near the death camera, too many: not writing", n); n = 0; InterlockedExchange(&g_ncamrec, 0); return; }
    int have = g_ncamrec;
    for (int i = 0; i < n && have < NCAMREC; i++) {
        int dup = 0;
        for (int j = 0; j < have; j++) if (g_camrec[j] == found[i]) dup = 1;
        if (!dup) g_camrec[have++] = found[i];
    }
    InterlockedExchange(&g_ncamrec, have);
}
static void follow_write(void) {
    if (!g_follow_on) return;
    float v[10]; memcpy(v, g_follow, 28); v[7] = 47.0f; v[8] = 0.05f; v[9] = 1000.0f;
    int n = g_ncamrec;
    for (int i = 0; i < n; i++) {
        float cur[10];
        if (!safe_read(g_camrec[i], cur, sizeof cur) || cur[8] != 0.05f || cur[9] != 1000.0f) continue;   /* gone or reused */
        memcpy((void *)(uintptr_t)g_camrec[i], v, 32);
    }
}
static void menu_tick(void);
static void menu_fallback(void);
static void start_tp(const float pos[3], int kind) {   /* any thread: fade out, then move (game thread) */
    DWORD now = GetTickCount();
    EnterCriticalSection(&g_cs); memcpy(g_tp_pos, pos, 12); g_tp_kind = kind; LeaveCriticalSection(&g_cs);
    g_fade_t0 = now | 1; g_tp_at = now + TP_FADE_OUT_MS; g_own_tp_t = now | 1;
    InterlockedExchange(&g_tp_pending, 1);
}
/* Build fingerprints (same as the lobby's): FNV-1a of a file, and AI.exe's PE timestamp and image size. */
static uint32_t file_fnv(const char *path) {
    FILE *f = fopen(path, "rb"); if (!f) return 0;
    uint32_t h = 2166136261u; unsigned char buf[16384]; size_t r;
    while ((r = fread(buf, 1, sizeof buf, f)) > 0) for (size_t i = 0; i < r; i++) { h ^= buf[i]; h *= 16777619u; }
    fclose(f); return h ? h : 1;
}
static uint32_t pe_build(const char *exe) {
    FILE *f = fopen(exe, "rb"); if (!f) return 0;
    uint8_t hd[0x400]; size_t r = fread(hd, 1, sizeof hd, f); fclose(f);
    if (r < 0x40) return 0;
    uint32_t pe; memcpy(&pe, hd + 0x3C, 4);
    if (pe + 24 + 60 > r || memcmp(hd + pe, "PE\0\0", 4)) return 0;
    uint32_t v[2]; memcpy(&v[0], hd + pe + 8, 4); memcpy(&v[1], hd + pe + 24 + 56, 4);
    uint32_t h = 2166136261u; const uint8_t *b = (const uint8_t *)v;
    for (int i = 0; i < 8; i++) { h ^= b[i]; h *= 16777619u; }
    return h ? h : 1;
}

/* At full dark, hand the move to engine.c (it runs at the end of the frame's physics pass), then report it. */
static void cut_teleport_tick(void) {
    static uint32_t ticket; static int kind; static float at[3];
    if (ticket) {
        int r = engine_result(ticket);
        if (r == ENG_PENDING) return;
        logf_("%s: %s (%.1f %.1f %.1f)", kind == 2 ? "carry" : kind ? "regroup" : "cutscene",
              r == ENG_DONE ? "moved to the partner" : r == ENG_DEAD ? "not moved (we're dead)" : r == ENG_NO_BODY ? "not moved (no physics body just now)" : "not moved (no player)",
              at[0], at[1], at[2]);
        if (r == ENG_DONE) { g_own_tp_t = GetTickCount() | 1; if (kind == 1) g_ui_regroup_t = GetTickCount(); else if (kind == 0) g_ui_cut_t = GetTickCount(); }
        ticket = 0;
    }
    if (!g_tp_pending || (int32_t)(GetTickCount() - g_tp_at) < 0) return;
    InterlockedExchange(&g_tp_pending, 0);
    EnterCriticalSection(&g_cs); memcpy(at, g_tp_pos, 12); kind = g_tp_kind; LeaveCriticalSection(&g_cs);
    ticket = engine_teleport(at);
    if (!ticket) logf_("%s: can't move the player on this build", kind ? "regroup" : "cutscene");
}
static void game_tick_cb(void) { follow_write(); menu_tick(); cut_teleport_tick(); if (g_alien_full) alien_entity_tick(); }
static void on_cutscene(void) { if (g_cut_on) InterlockedExchange(&g_cut_local, 1); }   /* game thread */
static DWORD WINAPI follow_thread(LPVOID p) {
    (void)p;
    timeBeginPeriod(1);
    while (!g_quit) { if (g_follow_on) { follow_write(); Sleep(1); } else Sleep(20); }
    return 0;
}

/* Net thread: follow the partner while we are dead and they are alive and visible. */
static void follow_step(void) {
    static int was;
    EnterCriticalSection(&g_cs);
    DWORD now = GetTickCount();
    int live = g_peer.valid && now - g_peer.t_ms < 1500 && (g_peer.p[0] != 0.f || g_peer.p[2] != 0.f);
    int on = g_follow_mode && g_life.state == LIFE_DEAD && g_life.peer_state == LIFE_ALIVE && live;
    if (on) { memcpy(g_follow, g_peer.q, 16); memcpy(g_follow + 4, g_peer.p, 12); }
    LeaveCriticalSection(&g_cs);
    static float death_pose[7]; static DWORD t_on; static int scans;
    if (on && !was) {
        Cam me; read_cam(&me);
        memcpy(death_pose, me.raw, 28);
        InterlockedExchange(&g_ncamrec, 0);
        camrec_scan(death_pose);
        t_on = GetTickCount(); scans = 1;
        logf_("spectate: %ld camera records at the death camera (%.2f %.2f %.2f)", (long)g_ncamrec, death_pose[4], death_pose[5], death_pose[6]);
    }
    if (on && was && scans < 4 && GetTickCount() - t_on > (scans == 1 ? 500u : scans == 2 ? 1500u : 3000u)) {
        long before = g_ncamrec;
        camrec_scan(death_pose); scans++;
        if (g_ncamrec != before) logf_("spectate: now %ld camera records", (long)g_ncamrec);
    }
    if (on != was) {
        InterlockedExchange(&g_follow_on, on);
        logf_("spectate: %s the partner's camera", on ? "following" : "stopped following");
        was = on;
    }
}

/* While spectating, keep the death effects off our screen: TriggerDeath on Health_Damage_Death (fade to black,
   vignette, colour), and the death depth-of-field in Global\systemic_post_processing ("Death DOF" .. "Death DOF_3",
   "Time to Death"), which otherwise blurs the partner's view. Game thread. */
static int swallow_cb(uint32_t method, uint32_t eid, uint32_t type) {
    if (!g_nofade) return 0;
    int effect = (method == 0xae498037u && type == 0x3f4defe1u) ||
                 (method == 0x830c2104u && (eid == 0xbcf22095u || eid == 0xaf7fd498u || eid == 0x97e51b56u || eid == 0xbcd40132u ||
                                            eid == 0x006bbdb7u));
    if (!effect) return 0;
    /* The death DOF starts ~0.3 s before TriggerDeath (from the death monitor), so "about to spectate" counts too:
       the partner is connected and alive. */
    EnterCriticalSection(&g_cs);
    DWORD now = GetTickCount();
    int s = g_life.state == LIFE_DEAD ||
            (g_follow_mode && g_life.state == LIFE_ALIVE && life_peer_live(&g_life, now) && g_life.peer_state == LIFE_ALIVE);
    LeaveCriticalSection(&g_cs);
    return s;
}

/* Game-over menu: UI state vtable at exe 0x1300200, "show game over" in slot 0x1300224 (FUN_00833af0), "hide" in
   0x1300228. Only ever called through the vtable. We hold the call while spectating and run it once both are down. */
typedef void(__attribute__((thiscall)) *show_go_fn)(void *self);
static show_go_fn g_show_go;
static void *volatile g_go_self;
static int g_hold_menu = 1;      /* AIC_HOLD_MENU=0: show the menu at once (old behaviour) */
static void __attribute__((thiscall)) show_go_hook(void *self) {
    int hold = 0;
    if (g_hold_menu) {
        EnterCriticalSection(&g_cs);
        hold = g_life.state == LIFE_DEAD;
        LeaveCriticalSection(&g_cs);
    }
    if (hold) {
        g_go_self = self; InterlockedExchange(&g_go_held, 1);
        logf_("life: game-over menu held while spectating");
        return;
    }
    g_show_go(self);
}
static void menu_show_now(const char *who) {
    if (g_go_release && InterlockedExchange(&g_go_held, 0)) {
        InterlockedExchange(&g_go_release, 0);
        g_show_go(g_go_self);
        logf_("life: game-over menu shown (%s)", who);
    } else if (g_go_release && !g_go_held) InterlockedExchange(&g_go_release, 0);
}
static void menu_tick(void) { menu_show_now("game thread"); }
/* Net thread: if no script dispatch ran the release within 1 s (the game may stop dispatching while dead), show the
   menu from here. */
static void menu_fallback(void) {
    static DWORD since;
    if (!g_go_release || !g_go_held) { since = 0; return; }
    if (!since) since = GetTickCount();
    else if (GetTickCount() - since > 1000) { menu_show_now("net thread"); since = 0; }
}
static void hook_gameover_ui(void) {
    uintptr_t base = (uintptr_t)GetModuleHandleW(NULL);
    uint32_t *slot = (uint32_t *)(base + (0x01300224u - 0x400000u));
    uint32_t want = (uint32_t)(base + (0x00833af0u - 0x400000u));
    if (*slot != want) { logf_("life: game-over vtable slot mismatch (%08x), menu not held", *slot); return; }
    DWORD old; VirtualProtect(slot, 4, PAGE_READWRITE, &old);
    g_show_go = (show_go_fn)(uintptr_t)*slot; *slot = (uint32_t)(uintptr_t)show_go_hook;
    VirtualProtect(slot, 4, old, &old);
    logf_("life: game-over menu hook installed");
}

/* ---------- alien and motion contacts ---------- */
/* Character table (see log_characters): every 100 ms, track motion per slot and find our alien. */
static void read_chars(void) {
    uintptr_t base = (uintptr_t)GetModuleHandleW(NULL);
    DWORD now = GetTickCount();
    int found = 0; float ap[3] = {0};
    for (int k = 0; k < TRACK_SLOTS; k++) {
        float r[3]; uint32_t tag;
        int ok = safe_read(base + RVA_CHAR_POS0 + (uintptr_t)k * 0xB0, r, 12) &&
                 safe_read(base + RVA_CHAR_POS0 + (uintptr_t)k * 0xB0 + 12, &tag, 4);
        if (!ok) break;
        if (!(r[0] == r[0]) || fabsf(r[0]) > 5000.f || fabsf(r[1]) > 5000.f || fabsf(r[2]) > 5000.f ||
            (fabsf(r[0]) + fabsf(r[2])) < 0.01f || fabsf(r[1]) < 0.01f || tag > 64) { g_track[k].used = 0; continue; }
        EnterCriticalSection(&g_cs);
        hud_motion_update(&g_track[k], r, now);
        memcpy(g_track_pos[k], r, 12); g_track_tag[k] = tag;
        LeaveCriticalSection(&g_cs);
        if (tag == 0) {
            EnterCriticalSection(&g_cs); cut_trail_push(&g_trail, now, r); int alive = g_life.state == LIFE_ALIVE; LeaveCriticalSection(&g_cs);
            /* carry-along: a script moved us in one go (a lift between floors): tell the partner where from and to */
            static float prev[3]; static DWORD prev_t; static CarryWatch cw;
            DWORD tp = g_own_tp_t, l0 = g_level_t0; float cf[3], ct[3];
            if (prev_t && g_lvl_ready && carry_watch(&cw, prev, r, now, (int32_t)(now - prev_t), tp ? (int32_t)(now - tp) : -1, l0 ? (int32_t)(now - l0) : 0, alive, cf, ct)) {
                CarryPkt ck; proto_fill_carry(&ck, (uint32_t)now ^ (g_pid << 16) ^ 0xCA77u, cf, ct);
                for (int i = 0; i < 3; i++) net_raw(&ck, sizeof ck);
                logf_("carry: a script moved us %.1f m (%.1f %.1f %.1f -> %.1f %.1f %.1f), telling the partner",
                      sqrtf((ct[0] - cf[0]) * (ct[0] - cf[0]) + (ct[1] - cf[1]) * (ct[1] - cf[1]) + (ct[2] - cf[2]) * (ct[2] - cf[2])),
                      cf[0], cf[1], cf[2], ct[0], ct[1], ct[2]);
            }
            memcpy(prev, r, 12); prev_t = now;
        }
        if (!found && (int)tag == g_alien_tag) { found = 1; memcpy(ap, r, 12); }
        if (found == 1 && (int)tag == g_alien_tag && g_alien_pin && !g_is_host) {
            EnterCriticalSection(&g_cs);
            int pin = g_peer_alien_ok && now - g_peer_alien_t < 500; float hp[3]; memcpy(hp, g_peer_alien, 12);
            LeaveCriticalSection(&g_cs);
            if (pin) {   /* experimental: unknown whether the engine reads this record back */
                SIZE_T w = 0;
                WriteProcessMemory(GetCurrentProcess(), (LPVOID)(base + RVA_CHAR_POS0 + (uintptr_t)k * 0xB0), hp, 12, &w);
            }
            found = 2;
        }
    }
    EnterCriticalSection(&g_cs); g_my_alien_ok = found != 0; memcpy(g_my_alien, ap, 12); LeaveCriticalSection(&g_cs);
    static int was = -1;
    if ((found != 0) != was) { was = found != 0; logf_("alien: %s (tag %d)", was ? "record found" : "no record", g_alien_tag); }
    static uint32_t seq;
    AlienPkt k; proto_fill_alien(&k, ++seq, ap, found != 0);
    net_raw(&k, sizeof k);
    if (g_alien_full && g_is_host) {   /* the full alien (alien.c), read on the game's AI thread */
        static uint32_t xseq;
        AlienExPkt x; alien_read(&x);
        x.seq = ++xseq; proto_fill_alienx(&x);
        net_raw(&x, sizeof x);
    }
}

static void on_alien(const AlienPkt *k) {
    EnterCriticalSection(&g_cs);
    if ((int32_t)(k->seq - g_peer_alien_seq) > 0 || GetTickCount() - g_peer_alien_t > 3000) {
        g_peer_alien_seq = k->seq; g_peer_alien_ok = k->flags & 1; memcpy(g_peer_alien, k->pos, 12); g_peer_alien_t = GetTickCount();
        if (g_share_alien && !g_is_host) skin_set_alien(k->flags & 1, k->pos);
    }
    LeaveCriticalSection(&g_cs);
}

/* Partner was moved by a script (carry-along, cutscene.h): if we were right there, we go with them. */
static void on_carry(const CarryPkt *k) {
    if (k->id == g_carry_seen_id) return;
    g_carry_seen_id = k->id;
    static DWORD last; DWORD now = GetTickCount();
    if (!g_lvl_ready || (last && now - last < CARRY_EVERY_MS)) return;
    float me[3], to[3]; int have, alive;
    EnterCriticalSection(&g_cs);
    have = g_trail.n > 0; if (have) memcpy(me, g_trail.p[(g_trail.head + CUT_TRAIL - 1) % CUT_TRAIL], 12);
    alive = g_life.state == LIFE_ALIVE;
    LeaveCriticalSection(&g_cs);
    if (!carry_follow(alive, have, me, k->from, k->to, to)) { logf_("carry: partner was moved by a script; we weren't with them"); return; }
    last = now;
    start_tp(to, 2);
    logf_("carry: partner was moved by a script and we were with them, going too (%.1f %.1f %.1f)", to[0], to[1], to[2]);
}

/* Partner's cutscene started: move there now (cutscene.h decides; the teleport runs on the game thread). */
static void on_cut(const CutPkt *k) {
    if (k->id == g_cut_seen_id) return;
    g_cut_seen_id = k->id;
    DWORD now = GetTickCount();
    float me[3]; int have; Life L;
    EnterCriticalSection(&g_cs);
    have = g_trail.n > 0; if (have) memcpy(me, g_trail.p[(g_trail.head + CUT_TRAIL - 1) % CUT_TRAIL], 12);
    L = g_life;
    LeaveCriticalSection(&g_cs);
    float dx = have ? k->pos[0] - me[0] : 1e9f, dy = have ? k->pos[1] - me[1] : 0, dz = have ? k->pos[2] - me[2] : 0;
    float dist = sqrtf(dx * dx + dy * dy + dz * dz);
    if (k->flags == CUT_REGROUP_ASK) {   /* the partner wants to come to us: answer with a spot behind us */
        float spot[3]; int ok;
        EnterCriticalSection(&g_cs); ok = cut_safe_spot(&g_trail, now, spot); LeaveCriticalSection(&g_cs);
        if (!ok || L.state != LIFE_ALIVE) { logf_("regroup: partner asked, but we're not somewhere they can come to"); return; }
        CutPkt r; proto_fill_cut(&r, k->id, spot, CUT_REGROUP_SPOT);
        for (int i = 0; i < 3; i++) net_raw(&r, sizeof r);
        g_coming_t = now;
        logf_("regroup: partner is coming to us (%.1f %.1f %.1f)", spot[0], spot[1], spot[2]);
        return;
    }
    if (k->flags == CUT_REGROUP_SPOT) {
        if (k->id != g_regroup_id) return;   /* not an answer to our ask */
        g_regroup_id = 0;
        if (!regroup_ok(L.state == LIFE_ALIVE, have, dist)) { logf_("regroup: not moving (%.1f m away, alive %d)", dist, L.state == LIFE_ALIVE); return; }
        start_tp(k->pos, 1);
        logf_("regroup: moving to the partner (%.1f m)", dist);
        return;
    }
    if (!g_cut_on) return;
    int32_t since = g_cut_sent_t ? (int32_t)(now - g_cut_sent_t) : -1;
    if (!cut_should_follow(L.state == LIFE_ALIVE, have, dist, since, g_is_host)) {
        logf_("cutscene: partner's cutscene started, staying (%.1f m away, alive %d, mine %ld ms ago)", dist, L.state == LIFE_ALIVE, (long)since);
        return;
    }
    g_cut_recv_t = now;
    start_tp(k->pos, 0);
    logf_("cutscene: partner's cutscene started, moving there (%.1f m)", dist);
}

/* Tracker sounds: ping for the nearest moving contact, heartbeat when one is within 6 m or while spectating. */
static void tracker_sound(int in_level, int active) {
    static DWORD next_ping, next_heart;
    DWORD now = GetTickCount();
    EnterCriticalSection(&g_cs);
    int spect = g_life.state == LIFE_DEAD || g_spec_test;
    Cam me = g_me; float best = -1;
    if (in_level && !spect)
        for (int k = 0; k < TRACK_SLOTS; k++) {
            if (!hud_motion_visible(&g_track[k], now) || g_track_tag[k] == 0) continue;   /* tag 0: the player */
            float dx = g_track_pos[k][0] - me.p[0], dz = g_track_pos[k][2] - me.p[2], d = sqrtf(dx * dx + dz * dz);
            if (d < 1.0f) continue;   /* our own body */
            if (best < 0 || d < best) best = d;
        }
    LeaveCriticalSection(&g_cs);
    if (!active) return;
    if (spect) {
        if (now >= next_heart) { sfx_play(SFX_HEART); next_heart = now + 1600; }
        return;
    }
    unsigned iv = hud_ping_ms(best);
    if (best >= 0 && best < 6.f && now >= next_heart) { sfx_play(SFX_HEART); next_heart = now + 800; next_ping = now + 400; return; }
    if (iv && now >= next_ping) { sfx_play(best < 8 ? SFX_PING_NEAR : best < 16 ? SFX_PING_MID : SFX_PING_FAR); next_ping = now + iv; }
}

static void net_send(const Cam *c) {
    if (g_steam) {
        PresencePkt k; proto_fill(&k, ++g_tx, c->p, c->q, c->ok == 2, g_name);
        if (!steamnet_send(&k, sizeof k)) g_tx--;
        return;
    }
    if (g_sock == INVALID_SOCKET || !g_peer_fixed) return;
    PresencePkt k; proto_fill(&k, ++g_tx, c->p, c->q, c->ok == 2, g_name);
    sendto(g_sock, (const char *)&k, sizeof k, 0, (struct sockaddr *)&g_peer_addr, sizeof g_peer_addr);
}

static void net_recv(void) {
    for (;;) {
        union { PresencePkt p; WorldPkt w; LifePkt l; AlienPkt a; SkinPkt k; CutPkt c; BuildPkt b; AlienExPkt x; CarryPkt y; uint8_t raw[128]; } u;
        struct sockaddr_in from; int fl = sizeof from, n;
        if (g_steam) n = steamnet_recv(&u, sizeof u);
        else n = recvfrom(g_sock, (char *)&u, sizeof u, 0, (struct sockaddr *)&from, &fl);
        if (n <= 0) return;
        int kind = proto_check_world(&u.w, n) ? 2 : proto_check_life(&u.l, n) ? 3 : proto_check_alien(&u.a, n) ? 4 :
                   proto_check_skin(&u.k, n) ? 5 : proto_check_cut(&u.c, n) ? 6 : proto_check_build(&u.b, n) ? 8 : proto_check_alienx(&u.x, n) ? 9 : proto_check_carry(&u.y, n) ? 10 : proto_check(&u.p, n) ? 1 : 0;
        if (!kind) continue;
        if (!g_steam && g_learn_peer && (!g_peer_fixed || from.sin_addr.s_addr != g_peer_addr.sin_addr.s_addr || from.sin_port != g_peer_addr.sin_port)) {
            g_peer_addr = from; g_peer_fixed = 1;   /* host: talk back to the joiner (latest address wins, handles NAT rebinding) */
            char ip[32]; inet_ntop(AF_INET, &from.sin_addr, ip, sizeof ip);
            logf_("net: peer joined from %s:%u", ip, ntohs(from.sin_port));
        }
        if (kind == 2) { on_world(&u.w); continue; }
        if (kind == 3) { on_life(&u.l); continue; }
        if (kind == 4) { on_alien(&u.a); continue; }
        if (kind == 5) { if (g_skins) skin_set_partner(u.k.skin); continue; }
        if (kind == 6) { on_cut(&u.c); continue; }
        if (kind == 10) { on_carry(&u.y); continue; }
        if (kind == 9) { if (g_alien_full && !g_is_host) alien_from_host(&u.x); continue; }
        if (kind == 8) {
            LONG bad = proto_build_diff(g_mod_hash, g_game_build, &u.b);
            if (InterlockedExchange(&g_build_bad, bad) != bad) {
                ws_pause(bad != 0);
                logf_("build: partner runs %s; world sync %s", !bad ? "the same builds" : bad & 1 ? "a different mod build" : "a different game build",
                      bad ? "off" : "on");
            }
            continue;
        }
        PresencePkt k = u.p;
        EnterCriticalSection(&g_cs);
        /* newer than the last, or the partner went quiet (a restarted game counts from 0 again: found in game) */
        if (!g_peer.valid || (int32_t)(k.seq - g_peer.seq) > 0 || GetTickCount() - g_peer.t_ms > 2000) {
            memcpy(g_peer.p, k.pos, 12); memcpy(g_peer.q, k.quat, 16);
            memcpy(g_peer.name, k.name, 16); g_peer.name[15] = 0;
            g_peer.seq = k.seq; g_peer.t_ms = GetTickCount(); g_peer.valid = (k.flags & 1) ? 2 : 1;
            g_rx++;
        }
        LeaveCriticalSection(&g_cs);
    }
}

/* ---------- overlay ---------- */
static BOOL CALLBACK find_game(HWND h, LPARAM lp) {
    DWORD pid; GetWindowThreadProcessId(h, &pid);
    if (pid != g_pid || !IsWindowVisible(h) || h == g_ov) return TRUE;
    RECT r; GetClientRect(h, &r);
    if (r.right - r.left >= 320) { *(HWND *)lp = h; return FALSE; }
    return TRUE;
}

/* Camera convention (measured, see view.h): camera looks along +Z, +Y up, +X right. */
static int g_fwd_sign = 1;
static ViewMark g_mark;               /* last drawn marker, for the log (benign race: log only) */
static volatile DWORD g_mark_t;

static volatile LONG g_thumb_ok;   /* couch spectating: the partner's window is shown live */
static volatile LONG g_give_up;

static volatile float g_f9_prog;   /* F9 hold progress while spectating, 0..1 */

/* Snapshot our state into a UiFrame for ui.c. */
static void build_frame(UiFrame *u, int W, int H) {
    memset(u, 0, sizeof *u);
    DWORD now = GetTickCount();
    u->W = W; u->H = H; u->now = now;
    Cam me; Peer pe; Life L; DWORD flash, both;
    EnterCriticalSection(&g_cs);
    me = g_me; pe = g_peer; L = g_life; flash = g_flash_t; both = g_both_t;
    u->alien_ok = g_peer_alien_ok && now - g_peer_alien_t < 1500; memcpy(u->alien_p, g_peer_alien, 12);
    for (int k = 0; k < TRACK_SLOTS && u->nblips < UI_MAX_BLIPS - 1; k++)
        if (hud_motion_visible(&g_track[k], now) && g_track_tag[k] != 0) {
            UiBlip *b = &u->blips[u->nblips++]; memcpy(b->p, g_track_pos[k], 12); b->kind = 0; b->label = NULL;
        }
    LeaveCriticalSection(&g_cs);
    static char name[16];
    memcpy(name, pe.valid ? pe.name : "partner", 16); name[15] = 0;
    u->peer_name = name;
    int live = pe.valid && now - pe.t_ms < 1500;
    u->linked = live || life_peer_live(&L, now);
    u->spect = L.state == LIFE_DEAD || g_spec_test;
    u->follow_on = g_follow_on; u->thumb_ok = g_thumb_ok;
    u->peer_dead = L.peer_state == LIFE_DEAD && life_peer_live(&L, now);
    u->flash_t = flash; u->both_t = both; u->give_up = g_f9_prog; u->cut_t = g_ui_cut_t;
    u->regroup_t = g_ui_regroup_t; u->coming_t = g_coming_t; u->regroup = g_regroup_prog; u->build_bad = (int)g_build_bad;
    u->fade = g_fade_t0 ? tp_fade((int32_t)(now - g_fade_t0)) : 0.f;
    u->steam = g_steam; u->steam_id = g_steam ? steamnet_self() : 0;
    if (live && pe.p[0] == 0.f && pe.p[1] == 0.f && pe.p[2] == 0.f) live = 0;   /* menu camera: not in a level */
    u->peer_live = live;
    u->me_in_level = me.ok && (me.p[0] != 0.f || me.p[1] != 0.f || me.p[2] != 0.f);
    if (live) {
        memcpy(u->peer_p, pe.p, 12); hud_forward(pe.q, u->peer_f);
        if (u->me_in_level) {
            ViewMark mk = view_project(me.p, me.q, pe.p, W, H, g_fwd_sign);
            g_mark = mk; g_mark_t = now;
            u->mk_show = 1; u->mk_on = mk.on; u->mk_sx = mk.sx; u->mk_sy = mk.sy; u->mk_dist = mk.dist;
            u->mk_dir[0] = mk.cam[0]; u->mk_dir[1] = mk.cam[1];
            if (mk.cam[2] <= 0.05f && fabsf(mk.cam[0]) < 1e-3f && fabsf(mk.cam[1]) < 1e-3f) u->mk_dir[0] = 1.f;
            UiBlip *b = &u->blips[u->nblips++]; memcpy(b->p, pe.p, 12); b->kind = u->peer_dead ? 2 : 1; b->label = name;
            float dx = pe.p[0] - me.p[0], dz = pe.p[2] - me.p[2];
            u->far_apart = dx * dx + dz * dz > 40.f * 40.f;
        }
    }
    memcpy(u->me_p, me.p, 12); hud_forward(me.q, u->me_f);
}

static LRESULT CALLBACK ov_proc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcA(h, m, w, l); }

/* Debug: coop_logs\ovdump_<profile>.req renders the overlay offscreen to ov_<profile>.bmp (over black). */
static void maybe_dump(int W, int H) {
    char req[MAX_PATH + 64], out[MAX_PATH + 64];
    snprintf(req, sizeof req, "%s\\coop_logs\\ovdump_%s.req", g_dir, g_profile);
    if (GetFileAttributesA(req) == INVALID_FILE_ATTRIBUTES || W <= 0 || H <= 0) return;
    DeleteFileA(req);
    uint32_t *px = malloc((size_t)W * H * 4);
    if (!px) return;
    UiFrame u; build_frame(&u, W, H); ui_render(&u, px, W, H);
    snprintf(out, sizeof out, "%s\\coop_logs\\ov_%s.bmp", g_dir, g_profile);
    FILE *fp = fopen(out, "wb");
    if (fp) {
        DWORD img = (DWORD)W * (DWORD)H * 4;
        BITMAPFILEHEADER fh = {0}; BITMAPINFOHEADER ih = {0};
        ih.biSize = sizeof ih; ih.biWidth = W; ih.biHeight = -H; ih.biPlanes = 1; ih.biBitCount = 32;
        fh.bfType = 0x4D42; fh.bfOffBits = sizeof fh + sizeof ih; fh.bfSize = fh.bfOffBits + img;
        fwrite(&fh, sizeof fh, 1, fp); fwrite(&ih, sizeof ih, 1, fp); fwrite(px, img, 1, fp); fclose(fp);
        logf_("overlay dump: %s (%dx%d)", out, W, H);
    }
    free(px);
}

/* The overlay's pixels: a 32-bit DIB pushed with UpdateLayeredWindow (per-pixel alpha). */
typedef struct { HDC dc; HBITMAP bm; HGDIOBJ old; uint32_t *px; int W, H; } OvSurf;
static int ov_surf(OvSurf *s, int W, int H) {
    if (s->bm && s->W == W && s->H == H) return 1;
    if (s->bm) { SelectObject(s->dc, s->old); DeleteObject(s->bm); s->bm = NULL; }
    if (!s->dc) s->dc = CreateCompatibleDC(NULL);
    BITMAPINFO bi = {0}; bi.bmiHeader.biSize = sizeof bi.bmiHeader; bi.bmiHeader.biWidth = W; bi.bmiHeader.biHeight = -H;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    void *bits = NULL;
    s->bm = CreateDIBSection(s->dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!s->bm) return 0;
    s->old = SelectObject(s->dc, s->bm); s->px = bits; s->W = W; s->H = H;
    return 1;
}
static void ov_present(HWND h, OvSurf *s, POINT at) {
    UiFrame u; build_frame(&u, s->W, s->H);
    ui_render(&u, s->px, s->W, s->H);
    GdiFlush();
    SIZE sz = {s->W, s->H}; POINT zero = {0, 0};
    BLENDFUNCTION bf = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    static int logged;
    if (!UpdateLayeredWindow(h, NULL, &at, &sz, s->dc, &zero, 0, &bf, ULW_ALPHA)) {
        if (!logged++) logf_("overlay: UpdateLayeredWindow failed (%lu)", (unsigned long)GetLastError());
    } else if (logged == 0) { logged = -1; logf_("overlay: drawing %dx%d with per-pixel alpha", s->W, s->H); }
}

/* Partner's presence (camera) packets are arriving: the camera can follow them. */
static int now_live_peer(void) {
    EnterCriticalSection(&g_cs);
    int live = g_peer.valid && GetTickCount() - g_peer.t_ms < 1500;
    LeaveCriticalSection(&g_cs);
    return live;
}

/* Couch spectating: the partner's game window on this PC (largest visible non-tool window of its process). */
typedef struct { DWORD pid; HWND best; LONG area; } FindPid;
static BOOL CALLBACK find_pid_win(HWND h, LPARAM lp) {
    FindPid *f = (FindPid *)lp; DWORD pid; GetWindowThreadProcessId(h, &pid);
    if (pid != f->pid || !IsWindowVisible(h) || (GetWindowLongA(h, GWL_EXSTYLE) & WS_EX_TOOLWINDOW)) return TRUE;
    RECT r; GetClientRect(h, &r); LONG a = (r.right - r.left) * (r.bottom - r.top);
    if (a > f->area) { f->area = a; f->best = h; }
    return TRUE;
}

static LRESULT CALLBACK spec_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_ERASEBKGND) { RECT r; GetClientRect(h, &r); FillRect((HDC)w, &r, (HBRUSH)GetStockObject(BLACK_BRUSH)); return 1; }
    if (m == WM_NCHITTEST) return HTTRANSPARENT;
    return DefWindowProcA(h, m, w, l);
}

static DWORD WINAPI overlay_thread(LPVOID p) {
    (void)p;
    WNDCLASSA wc = {0}; wc.lpfnWndProc = ov_proc; wc.hInstance = GetModuleHandleA(NULL); wc.lpszClassName = "AICoopOverlay";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW); RegisterClassA(&wc);
    WNDCLASSA ws = wc; ws.lpfnWndProc = spec_proc; ws.lpszClassName = "AICoopSpectate"; RegisterClassA(&ws);
    while (!g_quit && !g_game) { EnumWindows(find_game, (LPARAM)&g_game); Sleep(500); }
    if (g_quit) return 0;
    logf_("overlay: game window %p", (void *)g_game);
    g_ov = CreateWindowExA(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                           "AICoopOverlay", "", WS_POPUP, 0, 0, 10, 10, NULL, NULL, wc.hInstance, NULL);
    if (!ui_init()) logf_("overlay: GDI+ failed to start, no overlay");
    OvSurf surf = {0};
    /* Spectate backdrop under the overlay: the partner's live window (couch) or a dark veil (online). */
    HWND spec = NULL, src = NULL; HTHUMBNAIL thumb = NULL; int spec_mode = 0;   /* 1 veil, 2 thumbnail */
    DWORD f9_since = 0;
    while (!g_quit) {
        MSG msg; while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageA(&msg); }
        if (!IsWindow(g_game)) break;
        RECT cr; POINT tl = {0, 0}; GetClientRect(g_game, &cr); ClientToScreen(g_game, &tl);
        maybe_dump(cr.right, cr.bottom);
        HWND fg = GetForegroundWindow();
        /* Only draw while the game is the active window (never paint over other apps); couch: whenever visible,
           since every player's window is on screen at once and only one can be focused. */
        int show = !IsIconic(g_game) && cr.right > 0 && (g_couch || fg == g_game);

        EnterCriticalSection(&g_cs);
        int spect = g_life.state == LIFE_DEAD || g_spec_test;
        DWORD ppid = g_peer_couch ? g_peer_pid : 0;
        LeaveCriticalSection(&g_cs);

        /* F9 held 2 s while spectating (own window focused): go back to the checkpoint now */
        if (spect && fg == g_game && (GetAsyncKeyState(VK_F9) & 0x8000)) {
            if (!f9_since) f9_since = GetTickCount();
            else if (GetTickCount() - f9_since > 2000) { InterlockedExchange(&g_give_up, 1); f9_since = 0; }
        } else f9_since = 0;
        g_f9_prog = f9_since ? (float)(GetTickCount() - f9_since) / 2000.f : 0.f;
        /* F7 held 1.5 s while alive (own window focused): go to the partner */
        {
            static DWORD f7_since;
            if (!spect && fg == g_game && (GetAsyncKeyState(VK_F7) & 0x8000)) {
                if (!f7_since) f7_since = GetTickCount();
                else if (GetTickCount() - f7_since > REGROUP_HOLD_MS) { InterlockedExchange(&g_regroup_req, 1); f7_since = 0; }
            } else f7_since = 0;
            g_regroup_prog = f7_since ? (float)(GetTickCount() - f7_since) / REGROUP_HOLD_MS : 0.f;
        }

        int want = !spect || g_follow_on || (g_follow_mode && now_live_peer()) ? 0 : ppid ? 2 : 1;
        if (want == 2 && src && !IsWindow(src)) want = 1;
        if (want != spec_mode) {
            if (thumb) { DwmUnregisterThumbnail(thumb); thumb = NULL; }
            if (spec) { DestroyWindow(spec); spec = NULL; }
            InterlockedExchange(&g_thumb_ok, 0); src = NULL;
            if (want) {
                DWORD ex = WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT | (want == 1 ? WS_EX_LAYERED : 0);
                spec = CreateWindowExA(ex, "AICoopSpectate", "", WS_POPUP, tl.x, tl.y, cr.right, cr.bottom, NULL, NULL, wc.hInstance, NULL);
                if (want == 1) SetLayeredWindowAttributes(spec, 0, 200, LWA_ALPHA);
                if (want == 2) {
                    FindPid f = {ppid, NULL, 0}; EnumWindows(find_pid_win, (LPARAM)&f);
                    HRESULT hr = f.best ? DwmRegisterThumbnail(spec, f.best, &thumb) : E_FAIL;
                    if (SUCCEEDED(hr)) { src = f.best; InterlockedExchange(&g_thumb_ok, 1); }
                    logf_("spectate: partner window %p (pid %lu) -> thumbnail hr=%08lx", (void *)f.best, ppid, (unsigned long)hr);
                    if (FAILED(hr)) want = 1, SetWindowLongA(spec, GWL_EXSTYLE, ex | WS_EX_LAYERED), SetLayeredWindowAttributes(spec, 0, 200, LWA_ALPHA);
                } else logf_("spectate: bio-monitor view (partner on another PC)");
            }
            spec_mode = want;
        }
        if (spec) {
            SetWindowPos(spec, HWND_TOPMOST, tl.x, tl.y, cr.right, cr.bottom, SWP_NOACTIVATE | (show ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
            if (thumb) {
                DWM_THUMBNAIL_PROPERTIES tp = {0};
                tp.dwFlags = DWM_TNP_RECTDESTINATION | DWM_TNP_VISIBLE | DWM_TNP_OPACITY | DWM_TNP_SOURCECLIENTAREAONLY;
                RECT d = {0, 0, cr.right, cr.bottom}; tp.rcDestination = d; tp.fVisible = TRUE; tp.opacity = 255; tp.fSourceClientAreaOnly = TRUE;
                DwmUpdateThumbnailProperties(thumb, &tp);
            }
        }
        if (show && cr.right > 0 && cr.bottom > 0 && ov_surf(&surf, cr.right, cr.bottom)) ov_present(g_ov, &surf, tl);
        SetWindowPos(g_ov, HWND_TOPMOST, tl.x, tl.y, cr.right, cr.bottom, SWP_NOACTIVATE | SWP_NOSIZE | SWP_NOMOVE | (show ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
        Sleep(33);
    }
    if (thumb) DwmUnregisterThumbnail(thumb);
    return 0;
}

/* ---------- character table ----------
 * Static array in AI.exe .data: stride 0xB0, position at exe+0x1307AC8 + k*0xB0, tag dword right after
 * (0 = player, others not fully known yet). Logged every 5 s. */
#define RVA_CHAR_POS0 0x1307AC8u
static void log_characters(void) {
    uintptr_t base = (uintptr_t)GetModuleHandleW(NULL);
    char line[1024]; int n = 0, len = 0;
    for (int k = 0; k < 64 && len < (int)sizeof line - 64; k++) {
        float r[3]; uint32_t tag;
        if (!safe_read(base + RVA_CHAR_POS0 + (uintptr_t)k * 0xB0, r, 12) ||
            !safe_read(base + RVA_CHAR_POS0 + (uintptr_t)k * 0xB0 + 12, &tag, 4)) break;
        if (!(r[0] == r[0]) || fabsf(r[0]) > 5000.f || fabsf(r[1]) > 5000.f || fabsf(r[2]) > 5000.f ||
            (fabsf(r[0]) + fabsf(r[2])) < 0.01f || fabsf(r[1]) < 0.01f || tag > 64) continue;   /* past the array: y = 0 */
        len += snprintf(line + len, sizeof line - (size_t)len, " [%d t%u %.1f %.1f %.1f]", k, tag, r[0], r[1], r[2]);
        n++;
    }
    if (n) logf_("chars: %d%s", n, line);
}

/* ---------- main loop ---------- */
static DWORD WINAPI net_thread(LPVOID p) {
    (void)p;
    net_init();
    DWORD next = GetTickCount(), last_csv = 0, last_stat = GetTickCount();
    int was_ok = -1;
    while (!g_quit) {
        Cam c; read_cam(&c);
        if (c.ok != was_ok) { logf_("camera record: %s", c.ok == 2 ? "valid (quat ok)" : c.ok == 1 ? "signature ok, quat INVALID" : "not available"); was_ok = c.ok; }
        EnterCriticalSection(&g_cs); g_me = c; LeaveCriticalSection(&g_cs);
        DWORD now = GetTickCount();
        if (g_steam) { steam_up(); steamnet_poll(); }
        if (c.ok) net_send(&c);
        net_send_world();
        send_life(0);
        net_recv();
        life_step(InterlockedExchange(&g_give_up, 0) != 0, &c);
        follow_step();
        menu_fallback();
        if (g_life_test_ms && now - g_t0 >= g_life_test_ms) {   /* QA: a death without dying (AIC_LIFE_TEST=<s>) */
            uint32_t m[10] = {0};
            g_life_test_ms = 0;
            logf_("life: QA death");
            on_gameover(0, m);
        }
        {
            static DWORD last_chars;
            int in_lvl = c.ok && (c.p[0] != 0.f || c.p[1] != 0.f || c.p[2] != 0.f);
            if (in_lvl && now - last_chars >= 33) { read_chars(); last_chars = now; }   /* 30 Hz: the alien position drives the joiner's alien */
            {   /* a level (or a checkpoint) just started: its start-up scripts run in both games, don't send them */
                static int was_in;
                if (!in_lvl) ws_settle(15000);   /* menus and loading: the level's start-up scripts run before the camera exists */
                else if (!was_in) { logf_("worldsync: level started, not sending for 15 s"); g_level_t0 = now | 1; }
                {   /* partner events wait for our level (on_world) */
                    LONG ready = in_lvl && g_level_t0 && now - g_level_t0 > LEVEL_READY_MS;
                    if (ready != g_lvl_ready) { InterlockedExchange(&g_lvl_ready, ready); if (ready) world_flush(); }
                }
                {   /* tests: AIC_DEBUG_SPAWN, 25 s into a level */
                    static DWORD armed_for;
                    if (in_lvl && g_alien_full && g_level_t0 && now - g_level_t0 > 25000 && armed_for != g_level_t0) { armed_for = g_level_t0; alien_debug_arm(); }
                }
                was_in = in_lvl;
            }
            {   /* builds, every 3 s */
                static DWORD last_build;
                if (now - last_build >= 3000) { BuildPkt k; proto_fill_build(&k, g_mod_hash, g_game_build); net_raw(&k, sizeof k); last_build = now; }
            }
            if (g_regroup_req) {   /* F7 held: ask the partner for a spot next to them */
                InterlockedExchange(&g_regroup_req, 0);
                if (!in_lvl) logf_("regroup: not in a level");
                else if (g_regroup_ask_t && now - g_regroup_ask_t < REGROUP_GAP_MS) logf_("regroup: asked a moment ago, wait a few seconds");
                else {
                    float me[3] = {0, 0, 0};
                    EnterCriticalSection(&g_cs); if (g_trail.n) memcpy(me, g_trail.p[(g_trail.head + CUT_TRAIL - 1) % CUT_TRAIL], 12); LeaveCriticalSection(&g_cs);
                    g_regroup_id = (uint32_t)now ^ (g_pid << 12) ^ 0xF7F7u; if (!g_regroup_id) g_regroup_id = 1;
                    CutPkt k; proto_fill_cut(&k, g_regroup_id, me, CUT_REGROUP_ASK);
                    for (int i = 0; i < 3; i++) net_raw(&k, sizeof k);
                    g_regroup_ask_t = now;
                    logf_("regroup: asked the partner for a spot");
                }
            }
            if (g_cut_local) {   /* a cutscene started here: bring the partner (not if we were just moved to theirs) */
                InterlockedExchange(&g_cut_local, 0);
                float spot[3]; int have;
                EnterCriticalSection(&g_cs); have = cut_safe_spot(&g_trail, now, spot); LeaveCriticalSection(&g_cs);
                if (in_lvl && have && (!g_cut_recv_t || now - g_cut_recv_t > 5000)) {
                    CutPkt k; proto_fill_cut(&k, (uint32_t)now ^ (g_pid << 16) ^ 0x5EEDu, spot, CUT_SCENE);
                    for (int i = 0; i < 3; i++) net_raw(&k, sizeof k);
                    g_cut_sent_t = now;
                    logf_("cutscene: started here, asking the partner to come (%.1f %.1f %.1f)", spot[0], spot[1], spot[2]);
                }
            }
            tracker_sound(in_lvl, g_couch || (g_game && GetForegroundWindow() == g_game));
        }
        {
            int in_level = c.ok && (c.p[0] != 0.f || c.p[1] != 0.f || c.p[2] != 0.f);
            float pp[3]; int live;
            EnterCriticalSection(&g_cs);
            memcpy(pp, g_peer.p, 12); live = g_peer.valid && (GetTickCount() - g_peer.t_ms) < 1500;
            LeaveCriticalSection(&g_cs);
            if (g_skins) {
                static DWORD last_skin;
                if (now - last_skin >= 2000) { SkinPkt k; proto_fill_skin(&k, g_my_skin); net_raw(&k, sizeof k); last_skin = now; }
                float feet[3] = {pp[0], pp[1] - 1.65f, pp[2]};
                skin_set_target(in_level && live && (pp[0] != 0.f || pp[2] != 0.f), feet);
                if (g_alien_full && in_level && live && (pp[0] != 0.f || pp[2] != 0.f)) alien_partner_at(feet);
            }
            /* a skin body replaces the borrowed-NPC body */
            if (!g_skins || (!skin_active() && !skin_level_has_skin())) puppet_tick(in_level, live, pp, logf_);
        }
        if (g_csv && c.ok && now - last_csv >= 100) {
            fprintf(g_csv, "%lu", now);
            for (int i = 0; i < 16; i++) fprintf(g_csv, ",%.5f", c.raw[i]);
            EnterCriticalSection(&g_cs);
            fprintf(g_csv, ",%d,%.3f,%.3f,%.3f\n", g_peer.valid, g_peer.p[0], g_peer.p[1], g_peer.p[2]);
            LeaveCriticalSection(&g_cs);
            fflush(g_csv); last_csv = now;
        }
        if (now - last_stat >= 5000) {
            if (g_steam) logf_("steam: self=%llu peer=%llu", (unsigned long long)steamnet_self(), (unsigned long long)steamnet_peer());
            if (g_wtx || g_wrx) logf_("world: sent %u, received %u", g_wtx, g_wrx);
            log_characters();
            logf_("stat: tx=%u rx=%u me=%d (%.2f %.2f %.2f) q=(%.3f %.3f %.3f %.3f) peer=%d age=%ld (%.2f %.2f %.2f)", g_tx, g_rx, c.ok,
                  c.p[0], c.p[1], c.p[2], c.q[0], c.q[1], c.q[2], c.q[3],
                  g_peer.valid, g_peer.valid ? (long)(now - g_peer.t_ms) : -1L, g_peer.p[0], g_peer.p[1], g_peer.p[2]);
            if (now - g_mark_t < 2000)
                logf_("mark: on=%d screen=(%d,%d) cam=(%.2f %.2f %.2f) dist=%.2f", g_mark.on, g_mark.sx, g_mark.sy,
                      g_mark.cam[0], g_mark.cam[1], g_mark.cam[2], g_mark.dist);
            last_stat = now;
        }
        next += 50; DWORD n2 = GetTickCount();
        if ((int32_t)(next - n2) > 0) Sleep(next - n2); else next = n2;
    }
    return 0;
}

/* The game checks some of its data files against a table of sizes and hashes (FILE_HASHES::verify_integrity, RVA
   0x374CA0, called from 0x3DBFA4 and 0x5E8BA5) and quits silently when one differs. The levels -Skins writes
   (tools/skinprep) fail it by design. OpenCAGE patches those two calls in AI.exe on disk; the mod leaves AI.exe alone
   and points them at this function in memory instead. It waives the check only for a file skinprep changed (one with
   a backup under coop_backup\skins\<LEVEL>\); every other file still gets the game's own check. */
typedef uint8_t(__cdecl *integrity_fn)(const char *, const uint32_t *, uint32_t);
static integrity_fn g_integrity_orig;
static int skinprep_changed(const char *name) {
    char up[MAX_PATH]; lstrcpynA(up, name, sizeof up);
    for (char *q = up; *q; q++) { if (*q >= 'a' && *q <= 'z') *q -= 32; if (*q == '/') *q = 92; }
    const char *pr = strstr(up, "PRODUCTION");
    if (!pr || pr[10] != 92) return 0;
    const char *rest = pr + 11;
    char key[64], sub[MAX_PATH];
    if (!strncmp(rest, "DLC", 3) && rest[3] == 92) {   /* PRODUCTION\DLC\<NAME>\...: the backup is DLC_<NAME> */
        const char *e = strchr(rest + 4, 92); if (!e) return 0;
        snprintf(key, sizeof key, "DLC_%.*s", (int)(e - rest - 4), rest + 4); lstrcpynA(sub, e + 1, sizeof sub);
    } else {
        const char *e = strchr(rest, 92); if (!e) return 0;
        snprintf(key, sizeof key, "%.*s", (int)(e - rest), rest); lstrcpynA(sub, e + 1, sizeof sub);
    }
    char bk[MAX_PATH * 2]; snprintf(bk, sizeof bk, "%s\\coop_backup\\skins\\%s\\%s", g_dir, key, sub);
    return GetFileAttributesA(bk) != INVALID_FILE_ATTRIBUTES;
}
static uint8_t __cdecl integrity_hook(const char *name, const uint32_t *table, uint32_t count) {
    if (name && skinprep_changed(name)) { logf_("integrity: %s was changed by -Skins; the game's check is skipped for it", name); return 1; }
    uint8_t ok = g_integrity_orig(name, table, count);
    if (!ok) logf_("integrity: the game's check failed for %s (the game will quit)", name ? name : "?");
    return ok;
}
static void integrity_install(void) {
    uintptr_t base = (uintptr_t)GetModuleHandleW(NULL), target = base + 0x374CA0u;
    static const uint32_t sites[2] = {0x3DBFA4u, 0x5E8BA5u};
    int n = 0;
    for (int i = 0; i < 2; i++) {
        uint8_t *c = (uint8_t *)(base + sites[i]);
        if (c[0] != 0xE8 || (uintptr_t)(c + 5) + (uintptr_t)(intptr_t)*(int32_t *)(c + 1) != target) continue;   /* not this build */
        DWORD old; VirtualProtect(c, 5, PAGE_EXECUTE_READWRITE, &old);
        *(int32_t *)(c + 1) = (int32_t)((uint8_t *)integrity_hook - (c + 5));
        VirtualProtect(c, 5, old, &old); FlushInstructionCache(GetCurrentProcess(), c, 5);
        n++;
    }
    g_integrity_orig = (integrity_fn)target;
    logf_("integrity: %d of 2 file checks routed through the mod (levels changed by -Skins are let through)", n);
}

/* AIC_DIAG=1: log what ends the game (diagnosis). Serious exceptions with the faulting address, and calls to
   ExitProcess / TerminateProcess from the game, each with the game-code return addresses found on the stack. */
static uintptr_t g_exe_lo, g_exe_hi;
static void diag_stack(const char *what, uintptr_t at, const uint32_t *sp) {
    char line[512]; int n = snprintf(line, sizeof line, "diag: %s at exe+%#x; stack:", what, (unsigned)(at - g_exe_lo));
    int k = 0;
    for (int i = 0; i < 256 && k < 16 && n < (int)sizeof line - 16; i++) {
        uint32_t v = 0; SIZE_T got = 0;
        if (!ReadProcessMemory(GetCurrentProcess(), sp + i, &v, 4, &got) || got != 4) break;
        if (v >= g_exe_lo && v < g_exe_hi) { n += snprintf(line + n, sizeof line - (size_t)n, " %#x", (unsigned)(v - g_exe_lo)); k++; }
    }
    logf_("%s", line);
}
static LONG CALLBACK diag_veh(EXCEPTION_POINTERS *e) {
    static volatile LONG left = 40;
    DWORD c = e->ExceptionRecord->ExceptionCode;
    if (c >= 0xC0000000u && c != 0xC0000096u && InterlockedDecrement(&left) >= 0) {
        char w[64]; snprintf(w, sizeof w, "exception %08lx", (unsigned long)c);
        diag_stack(w, (uintptr_t)e->ContextRecord->Eip, (const uint32_t *)(uintptr_t)e->ContextRecord->Esp);
        if (g_log) fflush(g_log);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
static void (WINAPI *g_exit_orig)(UINT);
static BOOL (WINAPI *g_term_orig)(HANDLE, UINT);
static void WINAPI diag_exit(UINT code) {
    uint32_t mark = 0; char w[48]; snprintf(w, sizeof w, "ExitProcess(%u)", code);
    diag_stack(w, (uintptr_t)__builtin_return_address(0), &mark);
    if (g_log) fflush(g_log);
    g_exit_orig(code);
}
static BOOL WINAPI diag_term(HANDLE h, UINT code) {
    if (GetProcessId(h) == GetCurrentProcessId()) {
        uint32_t mark = 0; char w[48]; snprintf(w, sizeof w, "TerminateProcess(self, %u)", code);
        diag_stack(w, (uintptr_t)__builtin_return_address(0), &mark);
        if (g_log) fflush(g_log);
    }
    return g_term_orig(h, code);
}
static void diag_iat(HMODULE mod, const char *fn, void *hook, void **orig) {
    uint8_t *base = (uint8_t *)mod;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY dd = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    for (IMAGE_IMPORT_DESCRIPTOR *d = (IMAGE_IMPORT_DESCRIPTOR *)(base + dd.VirtualAddress); d->Name; d++) {
        if (lstrcmpiA((char *)(base + d->Name), "KERNEL32.dll")) continue;
        IMAGE_THUNK_DATA *ot = (IMAGE_THUNK_DATA *)(base + d->OriginalFirstThunk), *ft = (IMAGE_THUNK_DATA *)(base + d->FirstThunk);
        for (; ot->u1.AddressOfData; ot++, ft++) {
            if (ot->u1.Ordinal & IMAGE_ORDINAL_FLAG) continue;
            if (strcmp((char *)((IMAGE_IMPORT_BY_NAME *)(base + ot->u1.AddressOfData))->Name, fn)) continue;
            DWORD old; VirtualProtect(&ft->u1.Function, 4, PAGE_READWRITE, &old);
            *orig = (void *)(uintptr_t)ft->u1.Function; ft->u1.Function = (uintptr_t)hook;
            VirtualProtect(&ft->u1.Function, 4, old, &old);
        }
    }
}
static void (WINAPI *g_ods_orig)(LPCSTR);
static void WINAPI diag_ods(LPCSTR t) {
    static volatile LONG left = 400;
    if (t && InterlockedDecrement(&left) >= 0) {
        char m[300]; lstrcpynA(m, t, sizeof m);
        for (char *q = m; *q; q++) if (*q == 10 || *q == 13) *q = 32;
        logf_("diag: debug: %s", m);
    }
    if (g_ods_orig) g_ods_orig(t);
}
/* the game's "request quit" (sets the quit flag at exe+0x17DA920): log who asks */
static void __cdecl diag_quit(void) {
    uint32_t mark = 0;
    diag_stack("quit requested", (uintptr_t)__builtin_return_address(0), &mark);
    if (g_log) fflush(g_log);
    *(volatile uint8_t *)(g_exe_lo + 0x17DA920u) = 1;
}
static void diag_install(void) {
    HMODULE exe = GetModuleHandleW(NULL);
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)((uint8_t *)exe + ((IMAGE_DOS_HEADER *)exe)->e_lfanew);
    g_exe_lo = (uintptr_t)exe; g_exe_hi = g_exe_lo + nt->OptionalHeader.SizeOfImage;
    AddVectoredExceptionHandler(1, diag_veh);
    diag_iat(exe, "ExitProcess", (void *)diag_exit, (void **)&g_exit_orig);
    diag_iat(exe, "TerminateProcess", (void *)diag_term, (void **)&g_term_orig);
    diag_iat(exe, "OutputDebugStringA", (void *)diag_ods, (void **)&g_ods_orig);
    uint8_t *q = (uint8_t *)(g_exe_lo + 0x614770u);
    if (q[0] == 0xC6 && q[1] == 0x05 && q[6] == 0x01 && q[7] == 0xC3) {   /* mov byte [flag], 1 ; ret */
        DWORD old; VirtualProtect(q, 8, PAGE_EXECUTE_READWRITE, &old);
        q[0] = 0xE9; *(int32_t *)(q + 1) = (int32_t)((uint8_t *)diag_quit - (q + 5));
        VirtualProtect(q, 8, old, &old); FlushInstructionCache(GetCurrentProcess(), q, 8);
        logf_("diag: quit request hooked");
    } else logf_("diag: quit request: unexpected bytes %02x %02x", q[0], q[1]);
    logf_("diag: watching exceptions and exits (exit hook %s, terminate hook %s)", g_exit_orig ? "on" : "off", g_term_orig ? "on" : "off");
}

static void init(HMODULE self) {
    InitializeCriticalSection(&g_cs);
    g_pid = GetCurrentProcessId();
    char b[64];
    if (GetEnvironmentVariableA("AIC_PROFILE", b, sizeof b) && b[0]) lstrcpynA(g_profile, b, sizeof g_profile);
    if (GetEnvironmentVariableA("AIC_COOP_NAME", b, sizeof b) && b[0]) { lstrcpynA(g_name, b, sizeof g_name); g_name_set = 1; }
    else lstrcpynA(g_name, g_profile, sizeof g_name);
    char dir[MAX_PATH]; GetModuleFileNameA(self, dir, MAX_PATH);
    char *s = strrchr(dir, '\\'); if (s) *s = 0;
    lstrcpynA(g_dir, dir, sizeof g_dir);
    char p[MAX_PATH + 64];
    snprintf(p, sizeof p, "%s\\coop_logs", dir); CreateDirectoryA(p, NULL);
    snprintf(p, sizeof p, "%s\\coop_logs\\coopnet_%s_%lu.log", dir, g_profile, g_pid); g_log = fopen(p, "w");
    snprintf(p, sizeof p, "%s\\coop_logs\\cam_%s_%lu.csv", dir, g_profile, g_pid); g_csv = fopen(p, "w");
    if (g_csv) { fprintf(g_csv, "ms"); for (int i = 0; i < 16; i++) fprintf(g_csv, ",r%d", i); fprintf(g_csv, ",pv,px,py,pz\n"); }
    integrity_install();
    if (GetEnvironmentVariableA("AIC_DIAG", b, sizeof b) && b[0] == '1') diag_install();
    if (GetEnvironmentVariableA("AIC_FWD_SIGN", b, sizeof b)) g_fwd_sign = atoi(b) < 0 ? -1 : 1;
    if (GetEnvironmentVariableA("AIC_COUCH_SEAT", b, sizeof b) && b[0]) {
        g_couch = 1; g_seat = atoi(b);
        if (!g_name_set) { snprintf(g_name, sizeof g_name, "Player %d", g_seat + 1); }
    }
    if (GetEnvironmentVariableA("AIC_ALIEN_TAG", b, sizeof b) && b[0]) g_alien_tag = atoi(b);
    g_alien_pin = GetEnvironmentVariableA("AIC_ALIEN_PIN", b, sizeof b) && b[0] == '1';
    g_spec_test = GetEnvironmentVariableA("AIC_SPECTATE_TEST", b, sizeof b) && b[0] == '1';
    g_t0 = GetTickCount();
    if (GetEnvironmentVariableA("AIC_LIFE_TEST", b, sizeof b) && b[0]) g_life_test_ms = (DWORD)atoi(b) * 1000u;
    life_init(&g_life, GetTickCount());
    sfx_init();
    probe_set_gameover_cb(on_gameover);
    if (GetEnvironmentVariableA("AIC_SPECTATE_CAM", b, sizeof b) && b[0] == '0') g_follow_mode = 0;
    g_nofade = !(GetEnvironmentVariableA("AIC_DEATH_NOFADE", b, sizeof b) && b[0] == '0');   /* no fade to black while spectating */
    g_hold_menu = !(GetEnvironmentVariableA("AIC_HOLD_MENU", b, sizeof b) && b[0] == '0');
    g_cut_on = !(GetEnvironmentVariableA("AIC_CUTSCENE_TP", b, sizeof b) && b[0] == '0');
    engine_install(logf_);   /* player teleport for cutscenes and F7 (method from OpenCAGE) */
    {   /* our builds, for BuildPkt */
        char pth[MAX_PATH];
        snprintf(pth, sizeof pth, "%s\\coopnet.dll", dir); g_mod_hash = file_fnv(pth);
        GetModuleFileNameA(NULL, pth, sizeof pth); g_game_build = pe_build(pth);
        logf_("build: mod %08x, game %08x", g_mod_hash, g_game_build);
    }
    ws_set_cutscene_cb(on_cutscene);
    probe_set_tick_cb(game_tick_cb);
    probe_set_swallow(swallow_cb);
    hook_gameover_ui();
    CloseHandle(CreateThread(NULL, 0, follow_thread, NULL, 0, NULL));
    logf_("coopnet %s up: profile=%s name=%s fwd_sign=%d couch_seat=%d alien_tag=%d pin=%d", AIC_RELEASE, g_profile, g_name, g_fwd_sign,
          g_couch ? g_seat : -1, g_alien_tag, g_alien_pin);
    probe_install(g_dir, g_profile, logf_);
    {   /* skins: AIC_SKINS=1 turns them on (not tested in game yet); AIC_SKIN=<name> is how your partner sees you */
        char sb[32];
        if (GetEnvironmentVariableA("AIC_SKIN", sb, sizeof sb) && sb[0]) {
            for (char *q = sb; *q; q++) if (*q >= 'a' && *q <= 'z') *q -= 32;
            if (skin_find(sb)) lstrcpynA(g_my_skin, sb, sizeof g_my_skin); else logf_("skin: unknown skin %s, using %s", sb, g_my_skin);
        }
        g_skins = GetEnvironmentVariableA("AIC_SKINS", sb, sizeof sb) && sb[0] == '1';
        g_share_alien = GetEnvironmentVariableA("AIC_SHARED_ALIEN", sb, sizeof sb) && sb[0] == '1';
        if (g_share_alien) g_alien_full = alien_install(logf_, g_is_host);
        if ((g_skins || (g_share_alien && !g_alien_full)) && !skin_install(logf_, g_skins)) { g_skins = 0; if (!g_alien_full) g_share_alien = 0; }
        if (g_share_alien && !g_alien_full) skin_share_alien(1, g_alien_tag);   /* older way: only the position */
        if (g_skins) alien_body_hook = skin_body;
        logf_("skin: %s (you are %s); shared alien: %s", g_skins ? "on" : "off", g_my_skin, g_share_alien ? "on (joiner follows the host's alien)" : "off");
    }
    CloseHandle(CreateThread(NULL, 0, net_thread, NULL, 0, NULL));
    CloseHandle(CreateThread(NULL, 0, overlay_thread, NULL, 0, NULL));
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD r, LPVOID x) {
    (void)x;
    if (r == DLL_PROCESS_ATTACH) { DisableThreadLibraryCalls(h); init(h); }
    else if (r == DLL_PROCESS_DETACH) { g_quit = 1; }
    return TRUE;
}
