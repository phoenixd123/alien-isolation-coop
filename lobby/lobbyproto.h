/* Lobby protocol and state machine. Pure code (no sockets, no clock): lobbynet.c moves the packets, lobby.c
 * drives it, test/lobby_test.c runs it with a fake clock and over real UDP.
 *
 * LAN today. The host's lobby announces itself with a beacon on UDP 45501 (broadcast), the joiner answers the
 * sender with HELLO every half second, the host answers with STATE. Either side gone for 6 s = left. When the
 * host presses Start, STATE carries a 3 s countdown and both lobbies launch the game at the same moment, the
 * host as `host` and the joiner as `join <host ip>`. For Steam later, only the transport changes: a Steam lobby
 * replaces the beacon and the same HELLO/STATE packets go over Steam P2P.
 *
 * All packets: LbHead, a body, then an FNV-1a checksum of everything before it.
 */
#pragma once
#include <stdint.h>
#include <string.h>

#define LB_MAGIC      0x424C4941u   /* "AILB" */
#define LB_PROTO      1
#define LB_PORT       45501         /* discovery (beacons) */
#define LB_GAME_PORT  45500         /* the game's own co-op port */
#define LB_TIMEOUT_MS 6000
#define LB_BEACON_MS  1000
#define LB_SEND_MS    500
#define LB_COUNTDOWN_MS 3000
#define LB_MAXFOUND   16
/* ms since t, signed, so a stamp a moment in the future (stamps are `now | 1`) reads as 0, not 4 billion */
#define LB_SINCE(now, t) ((int32_t)((uint32_t)(now) - (uint32_t)(t)))

enum { LB_BEACON = 1, LB_HELLO, LB_STATE, LB_LEAVE, LB_PROBE };   /* PROBE: "is there a lobby at this address?" */
enum { LB_IDLE, LB_HOSTING, LB_JOINING };
/* why a joiner isn't in: */
enum { LB_OK, LB_FULL, LB_KICKED, LB_CLOSED, LB_LOST, LB_NOREPLY };
/* compatibility (lb_compat) */
#define LB_DIFF_MOD   1   /* blocks Start: different coopnet.dll builds would desync */
#define LB_DIFF_GAME  2   /* blocks Start: different AI.exe builds */
#define LB_DIFF_SAVE  4   /* warning: different saves, offer to copy the host's */
#define LB_NO_SAVE    8   /* warning: one side has no co-op save yet */

typedef struct { uint32_t ip; uint16_t port; } LbAddr;   /* host byte order */

typedef struct {
    char name[24];
    char skin[24];
    uint32_t mod, game, save;   /* hashes: coopnet.dll, AI.exe build, co-op save folder (0 = none) */
    uint8_t ready, present, pad[2];
} LbPlayer;                     /* 64 bytes */

typedef struct { uint32_t magic; uint8_t proto, kind, pad[2]; uint32_t lobby, nonce, seq; } LbHead;   /* 20 bytes */

typedef struct { LbHead h; LbPlayer host; uint8_t players, max, open, pad; uint32_t check; } LbBeacon;
typedef struct { LbHead h; LbPlayer me; uint32_t check; } LbHello;
typedef struct {
    LbHead h;
    LbPlayer host, guest;
    uint32_t guest_nonce;       /* the joiner the host has let in (0: nobody) */
    uint32_t to_nonce;          /* who this copy is for */
    uint16_t start_in;          /* ms until launch, 0 = no countdown */
    uint8_t reject, pad;        /* for to_nonce: LB_FULL / LB_KICKED */
    uint32_t check;
} LbState;
typedef struct { LbHead h; uint32_t check; } LbLeave;

static inline uint32_t lb_fnv(const void *p, int n) {
    const uint8_t *b = (const uint8_t *)p; uint32_t h = 2166136261u;
    for (int i = 0; i < n; i++) { h ^= b[i]; h *= 16777619u; }
    return h;
}
static inline void lb_clean(char *s, int cap) {   /* printable ASCII, NUL-terminated */
    s[cap - 1] = 0;
    for (int i = 0; s[i]; i++) if ((unsigned char)s[i] < 32 || (unsigned char)s[i] > 126) s[i] = '?';
}

/* Checks the head and checksum of a received packet; returns its kind or 0. */
static inline int lb_check_packet(const void *buf, int len) {
    static const int sizes[] = {0, sizeof(LbBeacon), sizeof(LbHello), sizeof(LbState), sizeof(LbLeave), sizeof(LbLeave)};
    if (len < (int)sizeof(LbHead) + 4) return 0;
    const LbHead *h = (const LbHead *)buf;
    if (h->magic != LB_MAGIC || h->proto != LB_PROTO || h->kind < 1 || h->kind > 5 || len != sizes[h->kind]) return 0;
    uint32_t c; memcpy(&c, (const uint8_t *)buf + len - 4, 4);
    return c == lb_fnv(buf, len - 4) ? h->kind : 0;
}

typedef struct {
    LbAddr addr;
    uint32_t lobby;
    LbPlayer host;
    int players, open;
    uint32_t seen;
} LbFound;

typedef struct {
    int role;
    uint32_t lobby, nonce, seq;
    LbPlayer me, peer;
    int have_peer;              /* host: a joiner is in; joiner: we got STATE and are in */
    uint32_t peer_seen;
    LbAddr peer_addr;           /* host: the joiner; joiner: the host */
    uint32_t peer_nonce;        /* host: the joiner's nonce */
    uint32_t kicked_nonce;
    int reason;                 /* joiner: why we're out (LB_FULL ...), shown on screen */
    uint32_t join_t;            /* joiner: when we started knocking */
    uint32_t last_beacon, last_send;
    uint32_t start_at; int counting, launched;
    uint32_t probe_ip, probe_t;  /* join by address: waiting for that host's beacon */
    LbFound found[LB_MAXFOUND]; int nfound;
} Lobby;

/* The lobby asks the transport to send through this. dest.ip == 0xFFFFFFFF means "every broadcast address";
   dest.port == 0 means the discovery port. */
typedef void (*LbSend)(void *ctx, LbAddr dest, const void *buf, int len);

static inline void lb_head(Lobby *L, LbHead *h, int kind) {
    memset(h, 0, sizeof *h);
    h->magic = LB_MAGIC; h->proto = LB_PROTO; h->kind = (uint8_t)kind; h->lobby = L->lobby; h->nonce = L->nonce; h->seq = ++L->seq;
}
#define LB_SEAL(pk) ((pk).check = lb_fnv(&(pk), (int)sizeof(pk) - 4))

/* Start a lobby (host) / start looking (browse) / knock on a lobby (join). `nonce` is any random number. */
static inline void lb_init(Lobby *L, const LbPlayer *me, uint32_t nonce) {
    LbFound keep[LB_MAXFOUND]; int nk = L->nfound; memcpy(keep, L->found, sizeof keep);
    memset(L, 0, sizeof *L);
    L->me = *me; L->me.present = 1; L->nonce = nonce ? nonce : 0x5EED1234u;
    memcpy(L->found, keep, sizeof keep); L->nfound = nk;
}
static inline void lb_host(Lobby *L, const LbPlayer *me, uint32_t nonce) {
    lb_init(L, me, nonce); L->role = LB_HOSTING; L->lobby = nonce ^ 0xA5A5F00Du; L->me.ready = 1;
}
static inline void lb_browse(Lobby *L, const LbPlayer *me, uint32_t nonce) { lb_init(L, me, nonce); L->role = LB_IDLE; }
static inline void lb_join(Lobby *L, const LbPlayer *me, uint32_t nonce, LbAddr host, uint32_t lobby, uint32_t now) {
    lb_init(L, me, nonce); L->role = LB_JOINING; L->peer_addr = host; L->lobby = lobby; L->join_t = now; L->me.ready = 0;
}

static inline void lb_send_beacon(Lobby *L, LbSend send, void *ctx, LbAddr to) {
    LbBeacon b; memset(&b, 0, sizeof b);
    lb_head(L, &b.h, LB_BEACON);
    b.host = L->me; b.players = (uint8_t)(1 + L->have_peer); b.max = 2; b.open = !L->have_peer;
    LB_SEAL(b); send(ctx, to, &b, sizeof b);
}

/* Join by address: ask whoever is at `ip` for its beacon. lb_probe_answer() then says which lobby answered. */
static inline void lb_probe(Lobby *L, uint32_t ip, uint32_t now, LbSend send, void *ctx) {
    LbLeave k; lb_head(L, &k.h, LB_PROBE); k.h.lobby = 0; LB_SEAL(k);
    LbAddr to = {ip, 0};
    send(ctx, to, &k, sizeof k);
    L->probe_ip = ip; L->probe_t = now | 1;
}

static inline void lb_send_state(Lobby *L, LbSend send, void *ctx, LbAddr to, uint32_t to_nonce, int reject, uint32_t now) {
    LbState s; memset(&s, 0, sizeof s);
    lb_head(L, &s.h, LB_STATE);
    s.host = L->me;
    if (L->have_peer) { s.guest = L->peer; s.guest_nonce = L->peer_nonce; }
    s.to_nonce = to_nonce; s.reject = (uint8_t)reject;
    if (L->counting) { uint32_t left = L->start_at - now; s.start_in = (uint16_t)((int32_t)left > 0 ? left : 1); }
    LB_SEAL(s); send(ctx, to, &s, sizeof s);
}

static inline void lb_leave(Lobby *L, LbSend send, void *ctx) {
    if (L->role == LB_IDLE) return;
    LbLeave k; lb_head(L, &k.h, LB_LEAVE); LB_SEAL(k);
    if (L->role == LB_JOINING || L->have_peer) send(ctx, L->peer_addr, &k, sizeof k);
    L->role = LB_IDLE; L->have_peer = 0; L->counting = 0;
}

/* Packet from `from`. Returns 1 if the screen should be redrawn. */
static inline int lb_on_packet(Lobby *L, LbAddr from, const void *buf, int len, uint32_t now, LbSend send, void *ctx) {
    int kind = lb_check_packet(buf, len);
    if (!kind) return 0;
    const LbHead *h = (const LbHead *)buf;
    if (h->nonce == L->nonce) return 0;   /* our own broadcast */

    if (kind == LB_BEACON) {
        LbBeacon b; memcpy(&b, buf, sizeof b);
        lb_clean(b.host.name, sizeof b.host.name); lb_clean(b.host.skin, sizeof b.host.skin);
        int i = 0;
        for (; i < L->nfound; i++) if (L->found[i].lobby == h->lobby) break;
        if (i == L->nfound) {
            if (L->nfound == LB_MAXFOUND) return 0;
            L->nfound++;
        }
        LbFound *f = &L->found[i];
        f->addr = from; f->lobby = h->lobby; f->host = b.host; f->players = b.players; f->open = b.open; f->seen = now;
        return 1;
    }
    if (kind == LB_PROBE) {
        if (L->role == LB_HOSTING) lb_send_beacon(L, send, ctx, from);
        return 0;
    }
    if (h->lobby != L->lobby) return 0;

    if (L->role == LB_HOSTING) {
        if (kind == LB_HELLO) {
            LbHello k; memcpy(&k, buf, sizeof k);
            lb_clean(k.me.name, sizeof k.me.name); lb_clean(k.me.skin, sizeof k.me.skin);
            if (h->nonce == L->kicked_nonce) { lb_send_state(L, send, ctx, from, h->nonce, LB_KICKED, now); return 0; }
            if (L->have_peer && h->nonce != L->peer_nonce) { lb_send_state(L, send, ctx, from, h->nonce, LB_FULL, now); return 0; }
            int changed = !L->have_peer || memcmp(&L->peer, &k.me, sizeof k.me) != 0;
            L->have_peer = 1; L->peer_nonce = h->nonce; L->peer = k.me; L->peer.present = 1; L->peer_addr = from; L->peer_seen = now;
            if (L->counting && !L->peer.ready) L->counting = 0;   /* they backed out during the countdown */
            if (changed) lb_send_state(L, send, ctx, from, h->nonce, LB_OK, now);
            return changed;
        }
        if (kind == LB_LEAVE && L->have_peer && h->nonce == L->peer_nonce) { L->have_peer = 0; L->counting = 0; return 1; }
        return 0;
    }
    if (L->role == LB_JOINING) {
        if (kind == LB_STATE) {
            LbState s; memcpy(&s, buf, sizeof s);
            if (s.to_nonce != L->nonce) return 0;
            if (s.reject) { L->role = LB_IDLE; L->have_peer = 0; L->reason = s.reject; return 1; }
            if (s.guest_nonce != L->nonce) return 0;
            lb_clean(s.host.name, sizeof s.host.name); lb_clean(s.host.skin, sizeof s.host.skin);
            L->peer = s.host; L->peer_addr = from; L->peer_seen = now; L->have_peer = 1; L->reason = LB_OK;
            if (s.start_in && !L->counting) { L->counting = 1; L->start_at = now + s.start_in; }
            else if (!s.start_in) L->counting = 0;
            return 1;
        }
        if (kind == LB_LEAVE) { L->role = LB_IDLE; L->have_peer = 0; L->counting = 0; L->reason = LB_CLOSED; return 1; }
    }
    return 0;
}

/* Join by address: the lobby found at the probed address (index into found[]), -1 while waiting, -2 if nobody
   answered within LB_TIMEOUT_MS / 2. Clears the probe once it has an answer. */
static inline int lb_probe_answer(Lobby *L, uint32_t now) {
    if (!L->probe_t) return -1;
    for (int i = 0; i < L->nfound; i++) if (L->found[i].addr.ip == L->probe_ip) { L->probe_t = 0; return i; }
    if (LB_SINCE(now, L->probe_t) > LB_TIMEOUT_MS / 2) { L->probe_t = 0; return -2; }
    return -1;
}

/* Timers: beacons, heartbeats, timeouts, countdown. Returns 1 if the screen should be redrawn. */
static inline int lb_tick(Lobby *L, uint32_t now, LbSend send, void *ctx) {
    int redraw = 0;
    for (int i = 0; i < L->nfound; i++)
        if (LB_SINCE(now, L->found[i].seen) > LB_TIMEOUT_MS / 2) { L->found[i] = L->found[--L->nfound]; i--; redraw = 1; }

    if (L->role == LB_HOSTING) {
        if (LB_SINCE(now, L->last_beacon) >= LB_BEACON_MS || !L->last_beacon) {
            L->last_beacon = now | 1;
            LbAddr all = {0xFFFFFFFFu, 0};
            lb_send_beacon(L, send, ctx, all);
        }
        if (L->have_peer && LB_SINCE(now, L->peer_seen) > LB_TIMEOUT_MS) { L->have_peer = 0; L->counting = 0; redraw = 1; }
        if (L->have_peer && LB_SINCE(now, L->last_send) >= LB_SEND_MS) { L->last_send = now; lb_send_state(L, send, ctx, L->peer_addr, L->peer_nonce, LB_OK, now); }
    } else if (L->role == LB_JOINING) {
        if (LB_SINCE(now, L->last_send) >= LB_SEND_MS || !L->last_send) {
            L->last_send = now | 1;
            LbHello k; memset(&k, 0, sizeof k);
            lb_head(L, &k.h, LB_HELLO); k.me = L->me; LB_SEAL(k);
            send(ctx, L->peer_addr, &k, sizeof k);
        }
        if (L->have_peer && LB_SINCE(now, L->peer_seen) > LB_TIMEOUT_MS) { L->role = LB_IDLE; L->have_peer = 0; L->counting = 0; L->reason = LB_LOST; redraw = 1; }
        if (!L->have_peer && L->role == LB_JOINING && LB_SINCE(now, L->join_t) > LB_TIMEOUT_MS) { L->role = LB_IDLE; L->reason = LB_NOREPLY; redraw = 1; }
    }
    if (L->counting) redraw = 1;
    return redraw;
}

static inline int lb_compat(const LbPlayer *a, const LbPlayer *b) {
    int f = 0;
    if (a->mod != b->mod) f |= LB_DIFF_MOD;
    if (a->game != b->game) f |= LB_DIFF_GAME;
    if (!a->save || !b->save) f |= LB_NO_SAVE;
    else if (a->save != b->save) f |= LB_DIFF_SAVE;
    return f;
}
static inline int lb_can_start(const Lobby *L) {
    return L->role == LB_HOSTING && L->have_peer && L->peer.ready && !L->counting &&
           !(lb_compat(&L->me, &L->peer) & (LB_DIFF_MOD | LB_DIFF_GAME));
}
static inline int lb_start(Lobby *L, uint32_t now, LbSend send, void *ctx) {
    if (!lb_can_start(L)) return 0;
    L->counting = 1; L->start_at = now + LB_COUNTDOWN_MS;
    lb_send_state(L, send, ctx, L->peer_addr, L->peer_nonce, LB_OK, now);
    return 1;
}
static inline void lb_set_ready(Lobby *L, int ready) { L->me.ready = (uint8_t)(ready != 0); if (!ready && L->role == LB_JOINING) L->counting = 0; }
static inline void lb_kick(Lobby *L, LbSend send, void *ctx, uint32_t now) {
    if (L->role != LB_HOSTING || !L->have_peer) return;
    L->kicked_nonce = L->peer_nonce; L->have_peer = 0; L->counting = 0;
    lb_send_state(L, send, ctx, L->peer_addr, L->kicked_nonce, LB_KICKED, now);
}
/* 1 once, when the countdown runs out: launch the game now. */
static inline int lb_should_launch(Lobby *L, uint32_t now) {
    if (!L->counting || L->launched || (int32_t)(now - L->start_at) < 0) return 0;
    if (L->role == LB_JOINING && !L->me.ready) return 0;
    L->launched = 1;
    return 1;
}
/* Seconds left on the countdown (for the screen), 0 if none. */
static inline int lb_countdown(const Lobby *L, uint32_t now) {
    if (!L->counting) return 0;
    int32_t left = (int32_t)(L->start_at - now);
    return left <= 0 ? 0 : (left + 999) / 1000;
}
