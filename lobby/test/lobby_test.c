/* Offline checks for the lobby: the state machine on a fake clock and network, then the real LAN transport over
 * loopback (UDP discovery and join, TCP save copy). Binds 127.0.0.1 only. */
#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../lobbyproto.h"
#include "../lobbynet.h"

static int fails, passes;
#define CHECK(c, m) do { if (c) passes++; else { fails++; printf("FAIL: %s\n", m); } } while (0)

/* ---------- fake network: three nodes, a queue, optional tampering ---------- */
#define NNODE 3
typedef struct { LbAddr to, from; uint8_t b[512]; int n; } Pkt;
static Pkt q[4096]; static int qn;
static Lobby node[NNODE];
static LbAddr addr_of(int i) { LbAddr a = {0x0A000001u + (uint32_t)i, (uint16_t)(50000 + i)}; return a; }
static int g_from, g_drop_to = -1;
static void fsend(void *ctx, LbAddr dest, const void *buf, int len) {
    (void)ctx;
    if (qn >= 4096) return;
    Pkt *p = &q[qn++]; p->to = dest; p->from = addr_of(g_from); memcpy(p->b, buf, (size_t)len); p->n = len;
}
static void deliver(uint32_t now) {
    for (int k = 0; k < qn; k++) {
        Pkt p = q[k];
        for (int i = 0; i < NNODE; i++) {
            LbAddr a = addr_of(i);
            int hit = p.to.ip == 0xFFFFFFFFu ? 1 : (p.to.ip == a.ip && (p.to.port == a.port || p.to.port == 0));
            if (!hit || i == g_drop_to) continue;
            if (p.from.ip == a.ip && p.to.ip == 0xFFFFFFFFu) continue;
            g_from = i; lb_on_packet(&node[i], p.from, p.b, p.n, now, fsend, NULL);
        }
    }
    qn = 0;
}
static void run(uint32_t *now, uint32_t ms) {
    for (uint32_t t = 0; t < ms; t += 50) {
        *now += 50;
        for (int i = 0; i < NNODE; i++) { g_from = i; lb_tick(&node[i], *now, fsend, NULL); }
        for (int r = 0; r < 4 && qn; r++) deliver(*now);
    }
}
static LbPlayer pl(const char *name, uint32_t save) {
    LbPlayer p; memset(&p, 0, sizeof p);
    lstrcpynA(p.name, name, sizeof p.name); lstrcpynA(p.skin, "TAYLOR", sizeof p.skin);
    p.mod = 0x11; p.game = 0x22; p.save = save; return p;
}

static void test_state_machine(void) {
    uint32_t now = 1000;
    LbPlayer a = pl("Ripley", 0xAA), b = pl("Sam", 0xBB), c = pl("Axel", 0xAA);
    lb_host(&node[0], &a, 101); lb_browse(&node[1], &b, 202); lb_browse(&node[2], &c, 303);
    run(&now, 1200);
    CHECK(node[1].nfound == 1 && node[2].nfound == 1, "both browsers see the lobby");
    CHECK(node[1].found[0].open && node[1].found[0].players == 1 && !strcmp(node[1].found[0].host.name, "Ripley"), "beacon content");
    CHECK(node[0].nfound == 0, "the host doesn't list itself");

    LbFound f = node[1].found[0];
    lb_join(&node[1], &b, 202, f.addr, f.lobby, now);
    run(&now, 600);
    CHECK(node[0].have_peer && !strcmp(node[0].peer.name, "Sam"), "host sees the joiner");
    CHECK(node[1].have_peer && !strcmp(node[1].peer.name, "Ripley"), "joiner is in");
    CHECK(lb_compat(&node[0].me, &node[0].peer) == LB_DIFF_SAVE, "different saves flagged, builds match");
    CHECK(!lb_can_start(&node[0]), "can't start before the joiner is ready");
    run(&now, 1100);
    CHECK(node[2].nfound == 1 && !node[2].found[0].open && node[2].found[0].players == 2, "beacon now says full");

    /* a third player knocks anyway */
    lb_join(&node[2], &c, 303, f.addr, f.lobby, now);
    run(&now, 600);
    CHECK(node[2].role == LB_IDLE && node[2].reason == LB_FULL, "third player told it's full");
    CHECK(node[0].have_peer && node[0].peer_nonce == 202, "the joiner keeps the slot");

    /* ready, start, countdown, both launch together */
    lb_set_ready(&node[1], 1);
    run(&now, 600);
    CHECK(node[0].peer.ready, "host sees ready");
    CHECK(lb_start(&node[0], now, fsend, NULL), "host starts");
    run(&now, 200);
    CHECK(node[1].counting && lb_countdown(&node[1], now) >= 2, "joiner counting down");
    /* the joiner backs out */
    lb_set_ready(&node[1], 0);
    run(&now, 600);
    CHECK(!node[0].counting && !node[1].counting, "un-ready cancels the countdown on both");
    lb_set_ready(&node[1], 1); run(&now, 600);
    g_from = 0; lb_start(&node[0], now, fsend, NULL);
    int lh = 0, lj = 0; uint32_t th = 0, tj = 0;
    for (int i = 0; i < 100 && !(lh && lj); i++) {
        run(&now, 50);
        if (!lh && lb_should_launch(&node[0], now)) { lh = 1; th = now; }
        if (!lj && lb_should_launch(&node[1], now)) { lj = 1; tj = now; }
    }
    CHECK(lh && lj, "both launch");
    CHECK((th > tj ? th - tj : tj - th) <= 200, "within 200 ms of each other");
    CHECK(!lb_should_launch(&node[0], now + 5000), "launch fires once");

    /* mod mismatch blocks start */
    LbPlayer bad = b; bad.mod = 0x99;
    lb_host(&node[0], &a, 111); lb_join(&node[1], &bad, 222, addr_of(0), node[0].lobby, now);
    lb_set_ready(&node[1], 1);
    run(&now, 1200);
    CHECK(node[0].have_peer && !lb_can_start(&node[0]) && !lb_start(&node[0], now, fsend, NULL), "different mod build can't start");

    /* kick */
    lb_kick(&node[0], fsend, NULL, now); run(&now, 600);
    CHECK(node[1].role == LB_IDLE && node[1].reason == LB_KICKED && !node[0].have_peer, "kick");
    lb_join(&node[1], &b, 222, addr_of(0), node[0].lobby, now); run(&now, 600);
    CHECK(node[1].reason == LB_KICKED && !node[0].have_peer, "kicked player stays out");

    /* host closes */
    lb_join(&node[2], &c, 333, addr_of(0), node[0].lobby, now); run(&now, 600);
    CHECK(node[0].have_peer && node[0].peer_nonce == 333, "another player joins");
    g_from = 0; lb_leave(&node[0], fsend, NULL); run(&now, 200);
    CHECK(node[2].role == LB_IDLE && node[2].reason == LB_CLOSED, "host closing tells the joiner");

    /* host vanishes (no LEAVE) */
    lb_host(&node[0], &a, 121); lb_join(&node[2], &c, 343, addr_of(0), node[0].lobby, now); run(&now, 600);
    CHECK(node[2].have_peer, "joined again");
    g_drop_to = 2; run(&now, LB_TIMEOUT_MS + 500); g_drop_to = -1;
    CHECK(node[2].role == LB_IDLE && node[2].reason == LB_LOST, "lost host times out");
    CHECK(!node[0].have_peer || node[0].peer_seen + LB_TIMEOUT_MS > now - 600, "host side consistent");
    run(&now, LB_TIMEOUT_MS + 500);
    CHECK(!node[0].have_peer, "host frees the slot of a silent joiner");

    /* nobody answers */
    lb_join(&node[2], &c, 353, addr_of(1), 0xDEAD, now);
    run(&now, LB_TIMEOUT_MS + 500);
    CHECK(node[2].role == LB_IDLE && node[2].reason == LB_NOREPLY, "no reply");

    /* join by address */
    lb_browse(&node[2], &c, 363); node[2].nfound = 0;
    g_from = 2; lb_probe(&node[2], addr_of(0).ip, now, fsend, NULL);
    deliver(now); deliver(now);
    int ans = lb_probe_answer(&node[2], now);
    CHECK(ans >= 0 && node[2].found[ans].lobby == node[0].lobby, "probe gets the host's beacon");
    g_from = 2; lb_probe(&node[2], 0x0A0000FFu, now, fsend, NULL); deliver(now);
    CHECK(lb_probe_answer(&node[2], now) == -1 && lb_probe_answer(&node[2], now + LB_TIMEOUT_MS) == -2, "probe to nobody times out");

    /* packet checks */
    LbHello k; memset(&k, 0, sizeof k); lb_head(&node[1], &k.h, LB_HELLO); k.me = b; LB_SEAL(k);
    CHECK(lb_check_packet(&k, sizeof k) == LB_HELLO, "valid packet");
    LbHello t = k; t.me.name[0] = 'X';
    CHECK(!lb_check_packet(&t, sizeof t), "tampered packet rejected");
    CHECK(!lb_check_packet(&k, sizeof k - 1), "short packet rejected");
    t = k; t.h.proto = 9; LB_SEAL(t);
    CHECK(!lb_check_packet(&t, sizeof t), "other protocol version rejected");
    char nm[8] = {'a', 1, 'b', (char)200, 'c', 0, 0, 0}; lb_clean(nm, sizeof nm);
    CHECK(!strcmp(nm, "a?b?c"), "names cleaned");
    CHECK(sizeof(LbPlayer) == 64 && sizeof(LbHead) == 20, "layout");
}

/* ---------- real sockets on loopback ---------- */
static void test_udp(void) {
    LbNet nh, nj;
    CHECK(lbnet_open(&nh, 45591, 1) && lbnet_open(&nj, 45591, 1), "open two loopback lobbies");
    Lobby H, J; memset(&H, 0, sizeof H); memset(&J, 0, sizeof J);
    LbPlayer a = pl("Host", 0xAA), b = pl("Join", 0xAA);
    lb_host(&H, &a, lbnet_random()); lb_browse(&J, &b, lbnet_random());
    DWORD t0 = GetTickCount(); int stage = 0, lh = 0, lj = 0;
    uint8_t buf[512]; LbAddr from; int n;
    while (GetTickCount() - t0 < 8000 && !(lh && lj)) {
        uint32_t now = GetTickCount();
        while ((n = lbnet_recv(&nh, &from, buf, sizeof buf)) > 0) lb_on_packet(&H, from, buf, n, now, lbnet_send, &nh);
        while ((n = lbnet_recv(&nj, &from, buf, sizeof buf)) > 0) lb_on_packet(&J, from, buf, n, now, lbnet_send, &nj);
        lb_tick(&H, now, lbnet_send, &nh); lb_tick(&J, now, lbnet_send, &nj);
        if (stage == 0 && J.nfound) {
            /* the second lobby on this PC may not get the broadcast (one discovery socket wins); probe like "join by address" */
            LbFound f = J.found[0]; lb_join(&J, &b, lbnet_random(), f.addr, f.lobby, now); stage = 1;
        }
        if (stage == 0 && !J.probe_t && GetTickCount() - t0 > 1500) lb_probe(&J, 0x7F000001u, now, lbnet_send, &nj);
        if (stage == 0) lb_probe_answer(&J, now);
        if (stage == 1 && J.have_peer) { lb_set_ready(&J, 1); stage = 2; }
        if (stage == 2 && H.peer.ready && lb_start(&H, now, lbnet_send, &nh)) stage = 3;
        if (!lh && lb_should_launch(&H, now)) lh = 1;
        if (!lj && lb_should_launch(&J, now)) lj = 1;
        Sleep(10);
    }
    CHECK(stage >= 1, "joiner found the host over UDP");
    CHECK(stage >= 3, "joined, ready and started over UDP");
    CHECK(lh && lj, "both launched over UDP");
    CHECK(H.peer_addr.ip == 0x7F000001u && J.peer_addr.port == nh.port, "addresses");
    lbnet_close(&nh); lbnet_close(&nj);
}

static void write_file(const char *p, size_t n, unsigned seed) {
    FILE *f = fopen(p, "wb"); unsigned x = seed;
    for (size_t i = 0; i < n; i++) { x = x * 1103515245u + 12345u; fputc((int)(x >> 16) & 255, f); }
    fclose(f);
}
static int same_file(const char *a, const char *b) {
    FILE *x = fopen(a, "rb"), *y = fopen(b, "rb"); int ok = x && y;
    while (ok) { int c = fgetc(x), d = fgetc(y); if (c != d) ok = 0; if (c == EOF || d == EOF) break; }
    if (x) fclose(x); if (y) fclose(y);
    return ok;
}

static void test_save_copy(void) {
    char tmp[MAX_PATH], root[MAX_PATH], hs[MAX_PATH], js[MAX_PATH], bk[MAX_PATH], p[MAX_PATH], q2[MAX_PATH];
    GetTempPathA(sizeof tmp, tmp);
    snprintf(root, sizeof root, "%slobby_test_%lu", tmp, GetCurrentProcessId());
    snprintf(hs, sizeof hs, "%s\\host", root); snprintf(js, sizeof js, "%s\\default", root); snprintf(bk, sizeof bk, "%s\\backup", root);
    CreateDirectoryA(root, NULL); CreateDirectoryA(hs, NULL); CreateDirectoryA(js, NULL);
    const char *hn[] = {"CHSV00.ais", "MSSV01.ais", "PROGRESSION.ais"};
    for (int i = 0; i < 3; i++) { snprintf(p, sizeof p, "%s\\%s", hs, hn[i]); write_file(p, i == 1 ? 2500000 : 40000 + i, 7 + i); }
    snprintf(p, sizeof p, "%s\\CHSV00.ais", js); write_file(p, 1234, 99);
    snprintf(p, sizeof p, "%s\\OLD.ais", js); write_file(p, 555, 98);
    snprintf(p, sizeof p, "%s\\..bad", hs); write_file(p, 10, 1);   /* not a save name: never sent */

    uint32_t h0 = lb_hash_saves(hs), j0 = lb_hash_saves(js);
    CHECK(h0 && j0 && h0 != j0, "save hashes differ before the copy");

    LbNet n; CHECK(lbnet_open(&n, 45592, 1), "open host socket");
    CHECK(lbnet_serve_saves(&n, hs, 0x1234), "serve saves");
    LbAddr host = {0x7F000001u, n.port};

    LbCopy c; memset(&c, 0, sizeof c);
    lbnet_fetch_saves(host, 0x9999, js, bk, &c);   /* wrong lobby id */
    for (int i = 0; i < 500 && c.state == LBC_RUNNING; i++) Sleep(10);
    CHECK(c.state == LBC_FAILED && lb_hash_saves(js) == j0, "wrong lobby id refused, joiner's saves untouched");

    memset(&c, 0, sizeof c);
    lbnet_fetch_saves(host, 0x1234, js, bk, &c);
    for (int i = 0; i < 1000 && c.state == LBC_RUNNING; i++) Sleep(10);
    CHECK(c.state == LBC_DONE, c.msg);
    CHECK(c.files == 3 && c.done_files == 3 && c.bytes == c.total && c.total == 2500000 + 40000 + 40002, "all bytes arrived");
    CHECK(lb_hash_saves(js) == h0, "joiner's saves now hash like the host's");
    int same = 1;
    for (int i = 0; i < 3; i++) { snprintf(p, sizeof p, "%s\\%s", hs, hn[i]); snprintf(q2, sizeof q2, "%s\\%s", js, hn[i]); same &= same_file(p, q2); }
    CHECK(same, "files identical");
    snprintf(p, sizeof p, "%s\\OLD.ais", js);
    CHECK(GetFileAttributesA(p) == INVALID_FILE_ATTRIBUTES, "old files moved out");
    /* the backup folder holds the two old files */
    char pat[MAX_PATH]; snprintf(pat, sizeof pat, "%s\\default-*", bk);
    WIN32_FIND_DATAA fd; HANDLE fh = FindFirstFileA(pat, &fd); int nb = 0;
    if (fh != INVALID_HANDLE_VALUE) {
        char b2[MAX_PATH]; snprintf(b2, sizeof b2, "%s\\%s", bk, fd.cFileName); FindClose(fh);
        uint32_t hb = lb_hash_saves(b2);
        CHECK(hb == j0, "backup is the joiner's old save");
        nb = 1;
    }
    CHECK(nb, "backup folder made");
    lbnet_close(&n);
}

static void test_misc(void) {
    char self[MAX_PATH]; GetModuleFileNameA(NULL, self, sizeof self);
    CHECK(lb_game_build(self) != 0, "PE build fingerprint");
    CHECK(lb_game_build("C:\\nope\\AI.exe") == 0 && lb_hash_file("C:\\nope\\x") == 0 && lb_hash_saves("C:\\nope") == 0, "missing files give 0");
    CHECK(lb_save_name_ok("CHSV00.ais") && lb_save_name_ok("PROGRESSION.ais"), "save names ok");
    CHECK(!lb_save_name_ok("..\\x") && !lb_save_name_ok("a/b") && !lb_save_name_ok(".x") && !lb_save_name_ok("") &&
          !lb_save_name_ok("C:x") && !lb_save_name_ok("0123456789012345678901234567890123"), "bad save names refused");
    uint32_t ip;
    CHECK(lbnet_parse_ip("192.168.1.20", &ip) && ip == 0xC0A80114u, "parse ip");
    CHECK(!lbnet_parse_ip("192.168.1", &ip) && !lbnet_parse_ip("1.2.3.256", &ip) && !lbnet_parse_ip("1.2.3.4x", &ip), "bad ips");
    uint64_t sid = 0;
    CHECK(lb_parse_steam_id("76561198012345678", &sid) && sid == 76561198012345678ull, "steam id");
    CHECK(lb_parse_steam_id("  76561198012345678 ", &sid), "steam id with spaces around");
    CHECK(!lb_parse_steam_id("7656119801234567", &sid) && !lb_parse_steam_id("765611980123456789", &sid), "steam id length");
    CHECK(!lb_parse_steam_id("76561197960265728", &sid) && !lb_parse_steam_id("99999999999999999", &sid), "not an individual account");
    CHECK(!lb_parse_steam_id("7656119801234567x", &sid) && !lb_parse_steam_id("", &sid) && !lb_parse_steam_id(NULL, &sid), "junk");
}

int main(void) {
    test_state_machine();
    test_udp();
    test_save_copy();
    test_misc();
    printf("lobby_test: %d/%d checks passed\n", passes, passes + fails);
    return fails != 0;
}
