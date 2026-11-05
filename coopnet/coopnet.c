/*
 * coopnet: presence layer for Alien: Isolation co-op (Steam build 4940658 only).
 * Loaded by steamwrap (steam_api.dll) from the game folder.
 *
 * - Reads the local camera record (pointer chain from coop-anything issue #85):
 *     mgr = *(u32*)(exe + 0x12F0C88); blk = *(u32*)(mgr + 0x58); rec = blk + 0x78C
 *   rec floats: [0..3] rotation quaternion (x,y,z,w) - hypothesis, checked by |q|=1;
 *               [4..6] position; [7]=47 (vfov deg), [8]=0.05 (near), [9]=1000 (far) - signature.
 * - Sends {pos, quat} over UDP at 20 Hz to the peer, receives the peer's.
 * - Draws the peer on a click-through overlay window over the game's client area.
 * - Logs telemetry (CSV) for checking axis conventions against real movement.
 *
 * Env: AIC_PROFILE (p1/p2...), AIC_COOP_MODE (host|join|steam-host|steam-join), AIC_COOP_PORT (local UDP port),
 *      AIC_COOP_PEER (host:port), AIC_COOP_PEER_STEAM (host's SteamID64, or "self" for a loopback test),
 *      AIC_COOP_NAME (shown to the peer, <=15 chars; Steam modes default to the Steam name).
 * Defaults: p1 binds 45500 and talks to 127.0.0.1:45501; any other profile the reverse (two copies on one PC).
 * host: binds 45500 and answers whoever sends valid packets. join: AIC_COOP_PEER=<host ip>:45500.
 * steam-host / steam-join: same packets over Steam P2P (steamnet.c), no IP or port forwarding.
 * Couch / split screen (Couchlink recipe in dist/couchlink): AIC_COUCH_SEAT=<n>. Seat 0 hosts on UDP 45500, every
 *   other seat joins 127.0.0.1:45500; the overlay draws whenever the window is visible, not only when focused.
 *
 * Co-op horror layer:
 * - Death (life.c): when you die you watch your partner until they die too, then both go back to the checkpoint.
 *   Spectating shows the partner's own window (couch, DWM thumbnail) or a bio-monitor built from their camera and
 *   their alien (online). Hold F9 for 2 s while spectating to go back at once.
 * - Motion tracker (hud.h): moving characters within 25 m and your partner, with pings that speed up as a contact
 *   closes in (sfx.c), a heartbeat when it is very close, a sting and a red flash when your partner dies.
 * - Alien: each game streams where its alien is (character table, tag AIC_ALIEN_TAG, default 3 = guess), so a
 *   spectator sees the partner's alien. AIC_ALIEN_PIN=1 (joiner, experimental) writes the host's alien position
 *   into the joiner's record.
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
#include <dwmapi.h>

int probe_install(const char *dir, const char *profile, void (*logf)(const char *, ...));
void probe_set_gameover_cb(int (*cb)(uint32_t hb, const uint32_t *msg));
void puppet_tick(int in_level, int peer_live, const float peer_cam_pos[3], void (*logf)(const char *, ...));

static FILE *g_log, *g_csv;
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

/* couch / roles */
static int g_couch, g_seat = -1, g_is_host;

/* co-op death (life.c); all under g_cs */
#define T_GAMEOVER 0x2f97adeeu
static Life g_life;
static int g_held; static uint32_t g_held_hb, g_held_msg[10];
static volatile LONG g_life_dirty;
static uint32_t g_peer_pid; static int g_peer_couch;
static DWORD g_flash_t, g_both_t;      /* partner-died flash, "both down" banner */
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
static void on_world(const WorldPkt *k) {
    if (k->session != g_wpeer_session) { memset(g_wseen, 0, sizeof g_wseen); g_wpeer_session = k->session; }   /* partner restarted */
    for (int i = 0; i < 64; i++) if (g_wseen[i] == k->seq) return;
    static int wi; g_wseen[wi++ & 63] = k->seq;
    WorldEvt ev; memset(&ev, 0, sizeof ev);
    ev.method = k->method; ev.eid = k->eid; ev.type = k->type; ev.npath = k->npath; memcpy(ev.path, k->path, sizeof k->path);
    g_wrx++;
    logf_("world: got #%u method %08x entity %08x path %08x/%08x (%d up)", k->seq, ev.method, ev.eid, ev.path[0], ev.path[1], ev.npath);
    ws_apply(&ev);
}

/* ---------- co-op death ---------- */
/* Game thread (call_method hook): a GameOver trigger is about to run. Return 1 to hold it back. */
static int on_gameover(uint32_t hb, const uint32_t *msg) {
    EnterCriticalSection(&g_cs);
    int hold = life_on_gameover(&g_life, GetTickCount());
    if (hold && !g_held) { g_held = 1; g_held_hb = hb; memcpy(g_held_msg, msg, sizeof g_held_msg); }
    int st = g_life.state;
    LeaveCriticalSection(&g_cs);
    InterlockedExchange(&g_life_dirty, 1);
    logf_("life: game over (hb %08x) -> %s", hb, hold ? "held, spectating the partner" : st == LIFE_RESTART ? "runs: back to the checkpoint" : "runs");
    return hold;
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

/* Net thread: run the life rules; release a held game over; turn one-shot events into sound and HUD. */
static void life_step(int give_up) {
    DWORD now = GetTickCount();
    EnterCriticalSection(&g_cs);
    int before = g_life.state;
    int rel = life_tick(&g_life, now, give_up);
    int died = g_life.ev_peer_died, both = g_life.ev_both_down;
    g_life.ev_peer_died = g_life.ev_both_down = 0;
    int post = rel && g_held; uint32_t hb = g_held_hb, msg[10]; memcpy(msg, g_held_msg, sizeof msg);
    if (rel) g_held = 0;
    if (died) g_flash_t = now;
    if (both) g_both_t = now;
    int after = g_life.state;
    LeaveCriticalSection(&g_cs);
    if (before != after) { InterlockedExchange(&g_life_dirty, 1); logf_("life: %d -> %d", before, after); }
    if (post && !hb) { logf_("life: held game over released (QA death, nothing to run)"); post = 0; }
    if (post) {
        int ok = replay_post_raw(hb, msg, T_GAMEOVER);
        logf_("life: running the held game over (%s)%s", ok ? "queued" : "QUEUE FULL", give_up ? ", player gave up" : "");
    }
    if (died) { sfx_play(SFX_STING); logf_("life: partner died"); }
    if (both) logf_("life: both down, back to the checkpoint");
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
}

static void on_alien(const AlienPkt *k) {
    EnterCriticalSection(&g_cs);
    if ((int32_t)(k->seq - g_peer_alien_seq) > 0 || GetTickCount() - g_peer_alien_t > 3000) {
        g_peer_alien_seq = k->seq; g_peer_alien_ok = k->flags & 1; memcpy(g_peer_alien, k->pos, 12); g_peer_alien_t = GetTickCount();
    }
    LeaveCriticalSection(&g_cs);
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
        union { PresencePkt p; WorldPkt w; LifePkt l; AlienPkt a; uint8_t raw[128]; } u;
        struct sockaddr_in from; int fl = sizeof from, n;
        if (g_steam) n = steamnet_recv(&u, sizeof u);
        else n = recvfrom(g_sock, (char *)&u, sizeof u, 0, (struct sockaddr *)&from, &fl);
        if (n <= 0) return;
        int kind = proto_check_world(&u.w, n) ? 2 : proto_check_life(&u.l, n) ? 3 : proto_check_alien(&u.a, n) ? 4 :
                   proto_check(&u.p, n) ? 1 : 0;
        if (!kind) continue;
        if (!g_steam && g_learn_peer && (!g_peer_fixed || from.sin_addr.s_addr != g_peer_addr.sin_addr.s_addr || from.sin_port != g_peer_addr.sin_port)) {
            g_peer_addr = from; g_peer_fixed = 1;   /* host: talk back to the joiner (latest address wins, handles NAT rebinding) */
            char ip[32]; inet_ntop(AF_INET, &from.sin_addr, ip, sizeof ip);
            logf_("net: peer joined from %s:%u", ip, ntohs(from.sin_port));
        }
        if (kind == 2) { on_world(&u.w); continue; }
        if (kind == 3) { on_life(&u.l); continue; }
        if (kind == 4) { on_alien(&u.a); continue; }
        PresencePkt k = u.p;
        EnterCriticalSection(&g_cs);
        if (!g_peer.valid || (int32_t)(k.seq - g_peer.seq) > 0) {
            memcpy(g_peer.p, k.pos, 12); memcpy(g_peer.q, k.quat, 16);
            memcpy(g_peer.name, k.name, 16); g_peer.name[15] = 0;
            g_peer.seq = k.seq; g_peer.t_ms = GetTickCount(); g_peer.valid = (k.flags & 1) ? 2 : 1;
            g_rx++;
        }
        LeaveCriticalSection(&g_cs);
    }
}

/* ---------- overlay ---------- */
static HWND g_game, g_ov;
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

static void text(HDC dc, int x, int y, COLORREF c, const char *s) { SetTextColor(dc, c); TextOutA(dc, x, y, s, (int)strlen(s)); }
static void text_c(HDC dc, int cx, int y, COLORREF c, const char *s) {
    SIZE z; GetTextExtentPoint32A(dc, s, (int)strlen(s), &z); text(dc, cx - z.cx / 2, y, c, s);
}
static void dot(HDC dc, int x, int y, int r, COLORREF c) {
    HBRUSH b = CreateSolidBrush(c); HGDIOBJ ob = SelectObject(dc, b);
    HPEN p = CreatePen(PS_SOLID, 1, c); HGDIOBJ op = SelectObject(dc, p);
    Ellipse(dc, x - r, y - r, x + r + 1, y + r + 1);
    SelectObject(dc, ob); SelectObject(dc, op); DeleteObject(b); DeleteObject(p);
}

typedef struct { float p[3]; COLORREF c; int r; const char *label; } Blip;

/* A motion-tracker disc at (cx, cy), radius R, centred on `at` facing `f`. Black is the overlay's
   transparent colour, so the disc is filled very dark green instead. Labelled blips out of range stick to the rim. */
static void draw_tracker(HDC dc, int cx, int cy, int R, const float at[3], const float f[2], const Blip *bl, int nb) {
    HBRUSH bg = CreateSolidBrush(RGB(4, 22, 12)); HGDIOBJ ob = SelectObject(dc, bg);
    HPEN rim = CreatePen(PS_SOLID, 2, RGB(60, 170, 110)); HGDIOBJ op = SelectObject(dc, rim);
    Ellipse(dc, cx - R, cy - R, cx + R, cy + R);
    SelectObject(dc, GetStockObject(NULL_BRUSH));
    HPEN ring = CreatePen(PS_SOLID, 1, RGB(30, 90, 60)); SelectObject(dc, ring);
    for (int i = 1; i < 3; i++) { int r = R * i / 3; Ellipse(dc, cx - r, cy - r, cx + r, cy + r); }
    MoveToEx(dc, cx - R, cy, NULL); LineTo(dc, cx + R, cy); MoveToEx(dc, cx, cy - R, NULL); LineTo(dc, cx, cy + R);
    float a = (float)(GetTickCount() % 2000) / 2000.f * 6.2831853f;   /* sweep line, one turn every 2 s */
    HPEN sw = CreatePen(PS_SOLID, 2, RGB(70, 210, 130)); SelectObject(dc, sw);
    MoveToEx(dc, cx, cy, NULL); LineTo(dc, cx + (int)(sinf(a) * R), cy - (int)(cosf(a) * R));
    SelectObject(dc, op); SelectObject(dc, ob); DeleteObject(bg); DeleteObject(rim); DeleteObject(ring); DeleteObject(sw);
    dot(dc, cx, cy, 3, RGB(70, 210, 130));
    for (int i = 0; i < nb; i++) {
        float x, y, d = hud_track(at, f, bl[i].p, &x, &y);
        int edge = 0;
        if (d > TRACK_RANGE) {
            if (!bl[i].label) continue;
            float n = sqrtf(x * x + y * y); x /= n; y /= n; edge = 1;
        }
        int px = cx + (int)(x * (float)(R - 6)), py = cy - (int)(y * (float)(R - 6));
        dot(dc, px, py, edge ? bl[i].r - 1 : bl[i].r, bl[i].c);
        if (bl[i].label) { char l[48]; snprintf(l, sizeof l, "%s %.0fm", bl[i].label, d); text(dc, px + 8, py - 9, bl[i].c, l); }
    }
}

/* The partner marker in the 3D view (circle with name and distance, or an arrow at the edge). */
static void draw_marker(HDC dc, int W, int H, const Cam *mep, const Peer *pep) {
    Cam me = *mep; Peer pe = *pep; char line[160];
    ViewMark mk = view_project(me.p, me.q, pe.p, W, H, g_fwd_sign);
    float dist = mk.dist, *c = mk.cam, z = c[2];
    int sx = mk.sx, sy = mk.sy, on = mk.on;
    g_mark = mk; g_mark_t = GetTickCount();
    HPEN pen = CreatePen(PS_SOLID, 3, RGB(80, 255, 120)); HGDIOBJ op = SelectObject(dc, pen);
    HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
    SetTextColor(dc, RGB(80, 255, 120));
    if (on) {
        int r = (int)(600.f / (dist + 2.f)); if (r < 6) r = 6; if (r > 40) r = 40;
        Ellipse(dc, sx - r, sy - r, sx + r, sy + r);
        MoveToEx(dc, sx, sy + r, NULL); LineTo(dc, sx, sy + 3 * r);
        snprintf(line, sizeof line, "%s  %.1f m", pe.name, dist);
        TextOutA(dc, sx + r + 6, sy - 8, line, (int)strlen(line));
    } else {
        /* edge arrow: direction in camera x/y plane; behind -> flip */
        float ex = c[0], ey = c[1];
        if (z <= 0.05f && fabsf(ex) < 1e-3f && fabsf(ey) < 1e-3f) ex = 1.f;
        float len = sqrtf(ex * ex + ey * ey); ex /= len; ey /= len;
        float cx = W * 0.5f, cy = H * 0.5f, k = fminf((cx - 40) / fmaxf(fabsf(ex), 1e-3f), (cy - 40) / fmaxf(fabsf(ey), 1e-3f));
        int ax_ = (int)(cx + ex * k), ay_ = (int)(cy - ey * k);
        POINT tri[3] = {{ax_ + (int)(ex * 22), ay_ - (int)(ey * 22)},
                        {ax_ + (int)(-ey * 12), ay_ - (int)(ex * 12)},
                        {ax_ + (int)(ey * 12), ay_ + (int)(ex * 12)}};
        HBRUSH gb = CreateSolidBrush(RGB(80, 255, 120)); SelectObject(dc, gb); Polygon(dc, tri, 3);
        SelectObject(dc, GetStockObject(NULL_BRUSH)); DeleteObject(gb);
        snprintf(line, sizeof line, "%s %.1f m", pe.name, dist);
        TextOutA(dc, ax_ - (ex > 0 ? 120 : -30), ay_ + (ey > 0 ? 20 : -36), line, (int)strlen(line));
    }
    SelectObject(dc, op); SelectObject(dc, ob); DeleteObject(pen);
}

static void draw(HDC dc, int W, int H) {
    RECT all = {0, 0, W, H};
    HBRUSH black = CreateSolidBrush(RGB(0, 0, 0)); FillRect(dc, &all, black); DeleteObject(black);
    DWORD now = GetTickCount();
    Cam me; Peer pe; Life L; DWORD flash, both; float palien[3]; int palien_ok;
    Blip bl[TRACK_SLOTS + 2]; int nb = 0;
    EnterCriticalSection(&g_cs);
    me = g_me; pe = g_peer; L = g_life; flash = g_flash_t; both = g_both_t;
    memcpy(palien, g_peer_alien, 12); palien_ok = g_peer_alien_ok && now - g_peer_alien_t < 1500;
    for (int k = 0; k < TRACK_SLOTS && nb < TRACK_SLOTS; k++)
        if (hud_motion_visible(&g_track[k], now) && g_track_tag[k] != 0) {
            memcpy(bl[nb].p, g_track_pos[k], 12); bl[nb].c = RGB(235, 245, 235); bl[nb].r = 5; bl[nb].label = NULL; nb++;
        }
    LeaveCriticalSection(&g_cs);
    SetBkMode(dc, TRANSPARENT);
    char line[200];
    int live = pe.valid && now - pe.t_ms < 1500;
    int linked = live || life_peer_live(&L, now);
    int spect = L.state == LIFE_DEAD || g_spec_test;

    if (spect) {   /* ---- you are dead: watch the partner ---- */
        RECT band = {0, 0, W, 64};
        HBRUSH bb = CreateSolidBrush(RGB(16, 4, 4)); FillRect(dc, &band, bb); DeleteObject(bb);
        snprintf(line, sizeof line, "YOU DIED  -  watching %s", pe.valid ? pe.name : "your partner");
        text_c(dc, W / 2, 8, RGB(230, 60, 50), line);
        if (!linked) text_c(dc, W / 2, 34, RGB(200, 200, 200), "Partner signal lost... going back to the checkpoint soon");
        else if (L.peer_state == LIFE_DEAD) text_c(dc, W / 2, 34, RGB(230, 60, 50), "Both down.");
        else text_c(dc, W / 2, 34, RGB(190, 170, 170), "When they die too, you both go back to the last checkpoint.  Hold F9 to go back now.");
        if (g_thumb_ok) return;   /* couch: the partner's own screen is shown under this band */
        if (live) {
            float f[2]; hud_forward(pe.q, f);
            Blip pb[1]; int np = 0;
            if (palien_ok) { memcpy(pb[0].p, palien, 12); pb[0].c = RGB(255, 70, 60); pb[0].r = 7; pb[0].label = "ALIEN"; np = 1; }
            int R = (H < W ? H : W) * 34 / 100;
            draw_tracker(dc, W / 2, H / 2 + 20, R, pe.p, f, pb, np);
            snprintf(line, sizeof line, "BIO-MONITOR  %s", pe.name);
            text_c(dc, W / 2, H / 2 + 20 - R - 30, RGB(80, 255, 140), line);
            if (palien_ok) {
                float dx = palien[0] - pe.p[0], dz = palien[2] - pe.p[2], d = sqrtf(dx * dx + dz * dz);
                if (d < 8) snprintf(line, sizeof line, "IT IS RIGHT ON THEM  (%.0f m)", d);
                else snprintf(line, sizeof line, "Alien %.0f m from them", d);
                text_c(dc, W / 2, H / 2 + 20 + R + 12, d < 8 ? RGB(255, 70, 60) : RGB(220, 160, 150), line);
            }
        }
        return;
    }
    if (!linked) {
        snprintf(line, sizeof line, "CO-OP: waiting for %s...", pe.valid ? pe.name : "partner");
        text(dc, 12, 10, RGB(200, 200, 200), line);
        if (g_steam) {
            if (steamnet_self()) snprintf(line, sizeof line, "Your Steam ID: %llu  (your friend joins with it)", (unsigned long long)steamnet_self());
            else snprintf(line, sizeof line, "Connecting to Steam...");
            text(dc, 12, 34, RGB(200, 200, 200), line);
        }
        return;
    }
    /* The menu camera sits at the origin: that is "not in a level" for both of us. */
    int me_in = me.ok && (me.p[0] != 0.f || me.p[1] != 0.f || me.p[2] != 0.f);
    if (live && pe.p[0] == 0.f && pe.p[1] == 0.f && pe.p[2] == 0.f) live = 0;
    if (!me_in) {
        snprintf(line, sizeof line, "CO-OP: %s connected (you are not in a level)", pe.valid ? pe.name : "partner");
        text(dc, 12, 10, RGB(120, 255, 120), line);
        return;
    }
    if (live && L.peer_state != LIFE_DEAD) draw_marker(dc, W, H, &me, &pe);
    {   /* motion tracker, bottom left */
        float f[2]; hud_forward(me.q, f);
        if (live) {
            memcpy(bl[nb].p, pe.p, 12); bl[nb].c = L.peer_state == LIFE_DEAD ? RGB(230, 60, 50) : RGB(80, 255, 120);
            bl[nb].r = 6; bl[nb].label = pe.name; nb++;
        }
        int R = H / 9; if (R < 60) R = 60;
        draw_tracker(dc, 24 + R, H - 24 - R, R, me.p, f, bl, nb);
        if (live) {
            float dx = pe.p[0] - me.p[0], dz = pe.p[2] - me.p[2];
            if (dx * dx + dz * dz > 40.f * 40.f && (now / 400) % 2) text(dc, 24, H - 48 - 2 * R, RGB(220, 200, 90), "PARTNER SIGNAL WEAK");
        }
    }
    if (L.peer_state == LIFE_DEAD && life_peer_live(&L, now)) {
        snprintf(line, sizeof line, "%s IS DOWN  -  they are watching you. Stay alive.", pe.valid ? pe.name : "YOUR PARTNER");
        text_c(dc, W / 2, 12, RGB(230, 60, 50), line);
    }
    if (now - flash < 2000 && (now / 150) % 2) {   /* red edge flash when the partner dies */
        HBRUSH r = CreateSolidBrush(RGB(170, 0, 0));
        RECT e[4] = {{0, 0, W, 14}, {0, H - 14, W, H}, {0, 0, 14, H}, {W - 14, 0, W, H}};
        for (int i = 0; i < 4; i++) FillRect(dc, &e[i], r);
        DeleteObject(r);
    }
    if (now - both < 5000) text_c(dc, W / 2, H / 2 - 10, RGB(230, 60, 50), "Both down. Back to the last checkpoint...");
}

static LRESULT CALLBACK ov_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_PAINT) {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
        RECT r; GetClientRect(h, &r);
        HDC mem = CreateCompatibleDC(dc); HBITMAP bm = CreateCompatibleBitmap(dc, r.right, r.bottom);
        HGDIOBJ old = SelectObject(mem, bm);
        HFONT f = CreateFontA(-18, 0, 0, 0, FW_BOLD, 0, 0, 0, ANSI_CHARSET, 0, 0, NONANTIALIASED_QUALITY, 0, "Segoe UI");
        HGDIOBJ of = SelectObject(mem, f);
        draw(mem, r.right, r.bottom);
        BitBlt(dc, 0, 0, r.right, r.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, of); DeleteObject(f); SelectObject(mem, old); DeleteObject(bm); DeleteDC(mem);
        EndPaint(h, &ps); return 0;
    }
    if (m == WM_ERASEBKGND) return 1;
    return DefWindowProcA(h, m, w, l);
}

/* QA hook: if coop_logs\ovdump_<profile>.req exists, render the overlay offscreen to ov_<profile>.bmp.
   Lets a bot check the drawing without ever copying screen pixels. */
static void maybe_dump(int W, int H) {
    char req[MAX_PATH + 64], out[MAX_PATH + 64];
    snprintf(req, sizeof req, "%s\\coop_logs\\ovdump_%s.req", g_dir, g_profile);
    if (GetFileAttributesA(req) == INVALID_FILE_ATTRIBUTES || W <= 0 || H <= 0) return;
    DeleteFileA(req);
    BITMAPINFO bi = {0}; bi.bmiHeader.biSize = sizeof bi.bmiHeader; bi.bmiHeader.biWidth = W; bi.bmiHeader.biHeight = -H;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    void *bits = NULL; HDC mem = CreateCompatibleDC(NULL);
    HBITMAP bm = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!bm) { DeleteDC(mem); return; }
    HGDIOBJ old = SelectObject(mem, bm);
    HFONT f = CreateFontA(-18, 0, 0, 0, FW_BOLD, 0, 0, 0, ANSI_CHARSET, 0, 0, NONANTIALIASED_QUALITY, 0, "Segoe UI");
    HGDIOBJ of = SelectObject(mem, f);
    draw(mem, W, H); GdiFlush();
    snprintf(out, sizeof out, "%s\\coop_logs\\ov_%s.bmp", g_dir, g_profile);
    FILE *fp = fopen(out, "wb");
    if (fp) {
        DWORD img = (DWORD)W * (DWORD)H * 4;
        BITMAPFILEHEADER fh = {0}; fh.bfType = 0x4D42; fh.bfOffBits = sizeof fh + sizeof bi.bmiHeader; fh.bfSize = fh.bfOffBits + img;
        BITMAPINFOHEADER ih = bi.bmiHeader; ih.biHeight = -H;
        fwrite(&fh, sizeof fh, 1, fp); fwrite(&ih, sizeof ih, 1, fp); fwrite(bits, img, 1, fp); fclose(fp);
        logf_("overlay dump: %s (%dx%d)", out, W, H);
    }
    SelectObject(mem, of); DeleteObject(f); SelectObject(mem, old); DeleteObject(bm); DeleteDC(mem);
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
    SetLayeredWindowAttributes(g_ov, RGB(0, 0, 0), 0, LWA_COLORKEY);
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

        int want = !spect ? 0 : ppid ? 2 : 1;
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
        SetWindowPos(g_ov, HWND_TOPMOST, tl.x, tl.y, cr.right, cr.bottom, SWP_NOACTIVATE | (show ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
        InvalidateRect(g_ov, NULL, FALSE);
        Sleep(16);
    }
    if (thumb) DwmUnregisterThumbnail(thumb);
    return 0;
}

/* ---------- character registry (research) ----------
 * Found 2026-11-03 by scanning for the player's feet below the camera: a static array in AI.exe .data,
 * record stride 0xB0, position (x,y,z floats) at exe+0x1307AC8 + k*0xB0, a tag dword right after it
 * (player 0; meaning of the others unknown). Records follow the characters as they move. Logged every
 * 5 s so a save with the alien shows which record and tag it gets. */
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
        life_step(InterlockedExchange(&g_give_up, 0) != 0);
        if (g_life_test_ms && now - g_t0 >= g_life_test_ms) {   /* QA: a death without dying (AIC_LIFE_TEST=<s>) */
            uint32_t m[10] = {0};
            g_life_test_ms = 0;
            logf_("life: QA death");
            on_gameover(0, m);
        }
        {
            static DWORD last_chars;
            int in_lvl = c.ok && (c.p[0] != 0.f || c.p[1] != 0.f || c.p[2] != 0.f);
            if (in_lvl && now - last_chars >= 100) { read_chars(); last_chars = now; }
            tracker_sound(in_lvl, g_couch || (g_game && GetForegroundWindow() == g_game));
        }
        {
            int in_level = c.ok && (c.p[0] != 0.f || c.p[1] != 0.f || c.p[2] != 0.f);
            float pp[3]; int live;
            EnterCriticalSection(&g_cs);
            memcpy(pp, g_peer.p, 12); live = g_peer.valid && (GetTickCount() - g_peer.t_ms) < 1500;
            LeaveCriticalSection(&g_cs);
            puppet_tick(in_level, live, pp, logf_);
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
    logf_("coopnet up: profile=%s name=%s fwd_sign=%d couch_seat=%d alien_tag=%d pin=%d", g_profile, g_name, g_fwd_sign,
          g_couch ? g_seat : -1, g_alien_tag, g_alien_pin);
    probe_install(g_dir, g_profile, logf_);
    CloseHandle(CreateThread(NULL, 0, net_thread, NULL, 0, NULL));
    CloseHandle(CreateThread(NULL, 0, overlay_thread, NULL, 0, NULL));
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD r, LPVOID x) {
    (void)x;
    if (r == DLL_PROCESS_ATTACH) { DisableThreadLibraryCalls(h); init(h); }
    else if (r == DLL_PROCESS_DETACH) { g_quit = 1; }
    return TRUE;
}
