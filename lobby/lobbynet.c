/* lobbynet - see lobbynet.h. */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lobbynet.h"

#define SAVE_MAGIC  0x56535941u   /* "AISV" */
#define SAVE_MAXF   32
#define SAVE_MAXSZ  (32u << 20)

static int g_wsa;
static void wsa(void) { if (!g_wsa) { WSADATA w; WSAStartup(MAKEWORD(2, 2), &w); g_wsa = 1; } }
static void nonblock(SOCKET s) { u_long one = 1; ioctlsocket(s, FIONBIO, &one); }

static void find_broadcasts(LbNet *n) {
    n->nbcast = 0;
    n->bcast[n->nbcast++] = 0x7F000001u;            /* a second copy on this PC */
    if (n->local_only) return;
    n->bcast[n->nbcast++] = 0xFFFFFFFFu;
    SOCKET s = socket(AF_INET, SOCK_DGRAM, 0);
    INTERFACE_INFO ifs[16]; DWORD got = 0;
    if (s != INVALID_SOCKET && WSAIoctl(s, SIO_GET_INTERFACE_LIST, NULL, 0, ifs, sizeof ifs, &got, NULL, NULL) == 0) {
        int k = (int)(got / sizeof ifs[0]);
        for (int i = 0; i < k && n->nbcast < 16; i++) {
            if (!(ifs[i].iiFlags & IFF_UP) || (ifs[i].iiFlags & IFF_LOOPBACK) || !(ifs[i].iiFlags & IFF_BROADCAST)) continue;
            uint32_t a = ntohl(ifs[i].iiAddress.AddressIn.sin_addr.s_addr), m = ntohl(ifs[i].iiNetmask.AddressIn.sin_addr.s_addr);
            uint32_t b = a | ~m;   /* directed broadcast: reaches VPN LANs (ZeroTier, Radmin) that drop 255.255.255.255 */
            int dup = 0; for (int j = 0; j < n->nbcast; j++) dup |= n->bcast[j] == b;
            if (!dup && b != 0xFFFFFFFFu) n->bcast[n->nbcast++] = b;
        }
    }
    if (s != INVALID_SOCKET) closesocket(s);
}

static int score(uint32_t a) {   /* home LANs first, then other private ranges (VPN LANs), then anything */
    if (!a) return 0;
    if ((a >> 16) == 0xC0A8) return 4;
    if ((a >> 24) == 10 || ((a >> 24) == 172 && ((a >> 16) & 0xF0) == 16)) return 3;
    return 1;
}
/* First LAN address (for "your address" on the host's screen); 0 if none. */
uint32_t lbnet_local_ip(void) {
    wsa();
    SOCKET s = socket(AF_INET, SOCK_DGRAM, 0);
    INTERFACE_INFO ifs[16]; DWORD got = 0; uint32_t best = 0;
    if (s != INVALID_SOCKET && WSAIoctl(s, SIO_GET_INTERFACE_LIST, NULL, 0, ifs, sizeof ifs, &got, NULL, NULL) == 0) {
        int k = (int)(got / sizeof ifs[0]);
        for (int i = 0; i < k; i++) {
            if (!(ifs[i].iiFlags & IFF_UP) || (ifs[i].iiFlags & IFF_LOOPBACK)) continue;
            uint32_t a = ntohl(ifs[i].iiAddress.AddressIn.sin_addr.s_addr);
            if ((a >> 16) == 0xA9FE) continue;   /* 169.254: no DHCP */
            if (score(a) > score(best)) best = a;
        }
    }
    if (s != INVALID_SOCKET) closesocket(s);
    return best;
}

int lbnet_open(LbNet *n, uint16_t disc_port, int local_only) {
    wsa();
    memset(n, 0, sizeof *n);
    n->local_only = local_only; n->disc_port = disc_port;
    uint32_t bind_ip = local_only ? 0x7F000001u : 0;
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return 0;
    BOOL on = TRUE; setsockopt(s, SOL_SOCKET, SO_BROADCAST, (const char *)&on, sizeof on);
    struct sockaddr_in a = {0}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(bind_ip);
    if (bind(s, (struct sockaddr *)&a, sizeof a) != 0) { closesocket(s); return 0; }
    int al = sizeof a; getsockname(s, (struct sockaddr *)&a, &al);
    n->port = ntohs(a.sin_port); nonblock(s); n->sock = (uintptr_t)s;

    SOCKET d = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (d != INVALID_SOCKET) {
        setsockopt(d, SOL_SOCKET, SO_REUSEADDR, (const char *)&on, sizeof on);
        setsockopt(d, SOL_SOCKET, SO_BROADCAST, (const char *)&on, sizeof on);
        struct sockaddr_in b = {0}; b.sin_family = AF_INET; b.sin_port = htons(disc_port); b.sin_addr.s_addr = htonl(bind_ip);
        if (bind(d, (struct sockaddr *)&b, sizeof b) == 0) { nonblock(d); n->disc = (uintptr_t)d; } else closesocket(d);
    }
    find_broadcasts(n);
    return 1;
}

void lbnet_close(LbNet *n) {
    lbnet_stop_saves(n);
    if (n->sock) closesocket((SOCKET)n->sock);
    if (n->disc) closesocket((SOCKET)n->disc);
    n->sock = n->disc = 0;
}

static void send_to(LbNet *n, uint32_t ip, uint16_t port, const void *buf, int len) {
    struct sockaddr_in a = {0}; a.sin_family = AF_INET; a.sin_port = htons(port); a.sin_addr.s_addr = htonl(ip);
    sendto((SOCKET)n->sock, (const char *)buf, len, 0, (struct sockaddr *)&a, sizeof a);
}

void lbnet_send(void *ctx, LbAddr dest, const void *buf, int len) {
    LbNet *n = (LbNet *)ctx;
    if (!n->sock) return;
    uint16_t port = dest.port ? dest.port : n->disc_port;
    if (dest.ip == 0xFFFFFFFFu) { for (int i = 0; i < n->nbcast; i++) send_to(n, n->bcast[i], port, buf, len); return; }
    send_to(n, dest.ip, port, buf, len);
}

static int recv_one(uintptr_t s, LbAddr *from, void *buf, int cap) {
    if (!s) return 0;
    struct sockaddr_in a; int al = sizeof a;
    int r = recvfrom((SOCKET)s, (char *)buf, cap, 0, (struct sockaddr *)&a, &al);
    if (r <= 0) return 0;
    from->ip = ntohl(a.sin_addr.s_addr); from->port = ntohs(a.sin_port);
    return r;
}
int lbnet_recv(LbNet *n, LbAddr *from, void *buf, int cap) {
    int r = recv_one(n->sock, from, buf, cap);
    return r ? r : recv_one(n->disc, from, buf, cap);
}

int lbnet_parse_ip(const char *s, uint32_t *ip) {
    unsigned a, b, c, d; char tail;
    if (!s || sscanf(s, "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) != 4 || a > 255 || b > 255 || c > 255 || d > 255) return 0;
    *ip = a << 24 | b << 16 | c << 8 | d;
    return 1;
}
void lbnet_ip_str(uint32_t ip, char *out, int cap) { snprintf(out, (size_t)cap, "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255); }

uint32_t lbnet_random(void) {
    LARGE_INTEGER q; QueryPerformanceCounter(&q);
    uint32_t v = (uint32_t)q.QuadPart ^ (uint32_t)(q.QuadPart >> 32) ^ GetCurrentProcessId() * 2654435761u ^ GetTickCount();
    v ^= v >> 16; v *= 0x7FEB352Du; v ^= v >> 15; v *= 0x846CA68Bu; v ^= v >> 16;
    return v ? v : 1;
}

/* ---------- fingerprints ---------- */
static uint32_t fnv_more(uint32_t h, const void *p, size_t n) {
    const uint8_t *b = (const uint8_t *)p;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 16777619u; }
    return h;
}
static uint32_t hash_file_into(uint32_t h, const char *path, int *ok) {
    FILE *f = fopen(path, "rb");
    if (!f) { *ok = 0; return h; }
    static unsigned char buf[1 << 16]; size_t r;
    while ((r = fread(buf, 1, sizeof buf, f)) > 0) h = fnv_more(h, buf, r);
    fclose(f); *ok = 1;
    return h;
}
uint32_t lb_hash_file(const char *path) {
    int ok; uint32_t h = hash_file_into(2166136261u, path, &ok);
    return ok ? (h ? h : 1) : 0;
}
uint32_t lb_game_build(const char *ai_exe) {
    FILE *f = fopen(ai_exe, "rb");
    if (!f) return 0;
    uint8_t hd[0x400]; size_t r = fread(hd, 1, sizeof hd, f); fclose(f);
    if (r < 0x40) return 0;
    uint32_t pe; memcpy(&pe, hd + 0x3C, 4);
    if (pe + 24 + 60 > r || memcmp(hd + pe, "PE\0\0", 4)) return 0;
    uint32_t v[2]; memcpy(&v[0], hd + pe + 8, 4); memcpy(&v[1], hd + pe + 24 + 56, 4);   /* TimeDateStamp, SizeOfImage */
    uint32_t h = fnv_more(2166136261u, v, sizeof v);
    return h ? h : 1;
}

typedef struct { char name[64]; } Name;
static int list_saves(const char *folder, Name *out, int cap) {
    char pat[MAX_PATH]; snprintf(pat, sizeof pat, "%s\\*", folder);
    WIN32_FIND_DATAA fd; HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    int n = 0;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (!lb_save_name_ok(fd.cFileName) || n >= cap) continue;
        lstrcpynA(out[n++].name, fd.cFileName, 64);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    for (int i = 1; i < n; i++) for (int j = i; j > 0 && lstrcmpiA(out[j - 1].name, out[j].name) > 0; j--) { Name t = out[j]; out[j] = out[j - 1]; out[j - 1] = t; }
    return n;
}
uint32_t lb_hash_saves(const char *folder) {
    Name f[SAVE_MAXF]; int n = list_saves(folder, f, SAVE_MAXF);
    if (!n) return 0;
    uint32_t h = 2166136261u;
    for (int i = 0; i < n; i++) {
        char p[MAX_PATH]; snprintf(p, sizeof p, "%s\\%s", folder, f[i].name);
        char up[64]; int k = 0; for (; f[i].name[k] && k < 63; k++) up[k] = (char)(f[i].name[k] >= 'a' && f[i].name[k] <= 'z' ? f[i].name[k] - 32 : f[i].name[k]);
        h = fnv_more(h, up, (size_t)k);
        int ok; h = hash_file_into(h, p, &ok);
    }
    return h ? h : 1;
}

int lb_save_name_ok(const char *s) {
    int n = 0;
    for (; s[n]; n++) {
        char c = s[n];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.')) return 0;
        if (n >= 31) return 0;
    }
    return n > 0 && s[0] != '.';
}

/* ---------- save copy over TCP ---------- */
static int send_all(SOCKET s, const void *p, int n) {
    const char *b = (const char *)p;
    while (n > 0) { int r = send(s, b, n, 0); if (r <= 0) return 0; b += r; n -= r; }
    return 1;
}
static int recv_all(SOCKET s, void *p, int n) {
    char *b = (char *)p;
    while (n > 0) { int r = recv(s, b, n, 0); if (r <= 0) return 0; b += r; n -= r; }
    return 1;
}

typedef struct { SOCKET ls; char folder[MAX_PATH]; uint32_t lobby; } Server;
static Server g_srv;
static HANDLE g_srv_thread;

static void serve_one(SOCKET c) {
    DWORD to = 15000; setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, (const char *)&to, sizeof to); setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, (const char *)&to, sizeof to);
    uint32_t req[2];
    if (!recv_all(c, req, sizeof req) || req[0] != SAVE_MAGIC || req[1] != g_srv.lobby) return;
    Name f[SAVE_MAXF]; int n = list_saves(g_srv.folder, f, SAVE_MAXF);
    uint32_t cnt = (uint32_t)n;
    if (!send_all(c, &cnt, 4)) return;
    static char buf[1 << 16];
    for (int i = 0; i < n; i++) {
        char p[MAX_PATH]; snprintf(p, sizeof p, "%s\\%s", g_srv.folder, f[i].name);
        FILE *fp = fopen(p, "rb");
        uint32_t size = 0;
        if (fp) { fseek(fp, 0, SEEK_END); long L = ftell(fp); fseek(fp, 0, SEEK_SET); size = L > 0 && (uint32_t)L <= SAVE_MAXSZ ? (uint32_t)L : 0; }
        char nm[32] = {0}; lstrcpynA(nm, f[i].name, 32);
        if (!send_all(c, nm, 32) || !send_all(c, &size, 4)) { if (fp) fclose(fp); return; }
        uint32_t left = size;
        while (left) {
            size_t want = left < sizeof buf ? left : sizeof buf, r = fp ? fread(buf, 1, want, fp) : 0;
            if (r == 0) { memset(buf, 0, want); r = want; }   /* file shrank: keep the stream consistent */
            if (!send_all(c, buf, (int)r)) { fclose(fp); return; }
            left -= (uint32_t)r;
        }
        if (fp) fclose(fp);
    }
}
static DWORD WINAPI serve_thread(LPVOID u) {
    (void)u;
    for (;;) {
        SOCKET c = accept(g_srv.ls, NULL, NULL);
        if (c == INVALID_SOCKET) return 0;   /* listening socket closed: stop */
        serve_one(c);
        shutdown(c, SD_SEND); closesocket(c);
    }
}

int lbnet_serve_saves(LbNet *n, const char *folder, uint32_t lobby) {
    lbnet_stop_saves(n);
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return 0;
    struct sockaddr_in a = {0}; a.sin_family = AF_INET; a.sin_port = htons(n->port); a.sin_addr.s_addr = htonl(n->local_only ? 0x7F000001u : 0);
    if (bind(s, (struct sockaddr *)&a, sizeof a) != 0 || listen(s, 2) != 0) { closesocket(s); return 0; }
    g_srv.ls = s; g_srv.lobby = lobby; lstrcpynA(g_srv.folder, folder, MAX_PATH);
    n->tcp = (uintptr_t)s;
    g_srv_thread = CreateThread(NULL, 0, serve_thread, NULL, 0, NULL);
    return g_srv_thread != NULL;
}
void lbnet_stop_saves(LbNet *n) {
    if (n->tcp) { closesocket((SOCKET)n->tcp); n->tcp = 0; }
    if (g_srv_thread) { WaitForSingleObject(g_srv_thread, 3000); CloseHandle(g_srv_thread); g_srv_thread = NULL; }
}

typedef struct { LbAddr host; uint32_t lobby; char folder[MAX_PATH], backup[MAX_PATH]; LbCopy *c; } Fetch;

static void fail(LbCopy *c, const char *m) { lstrcpynA(c->msg, m, sizeof c->msg); c->state = LBC_FAILED; }

static DWORD WINAPI fetch_thread(LPVOID u) {
    Fetch *F = (Fetch *)u; LbCopy *c = F->c;
    char inc[MAX_PATH]; snprintf(inc, sizeof inc, "%s.incoming", F->folder);
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    struct sockaddr_in a = {0}; a.sin_family = AF_INET; a.sin_port = htons(F->host.port); a.sin_addr.s_addr = htonl(F->host.ip);
    DWORD to = 15000; setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&to, sizeof to);
    if (s == INVALID_SOCKET || connect(s, (struct sockaddr *)&a, sizeof a) != 0) { fail(c, "COULD NOT REACH THE HOST"); goto out; }
    uint32_t req[2] = {SAVE_MAGIC, F->lobby}, cnt = 0;
    if (!send_all(s, req, sizeof req) || !recv_all(s, &cnt, 4)) { fail(c, "THE HOST DID NOT ANSWER"); goto out; }
    if (cnt == 0) { fail(c, "THE HOST HAS NO CO-OP SAVE YET"); goto out; }
    if (cnt > SAVE_MAXF) { fail(c, "BAD REPLY FROM THE HOST"); goto out; }
    c->files = (int)cnt;
    CreateDirectoryA(inc, NULL);
    char names[SAVE_MAXF][32];
    static char buf[1 << 16];
    for (uint32_t i = 0; i < cnt; i++) {
        uint32_t size;
        if (!recv_all(s, names[i], 32) || !recv_all(s, &size, 4)) { fail(c, "COPY INTERRUPTED"); goto out; }
        names[i][31] = 0;
        if (!lb_save_name_ok(names[i]) || size > SAVE_MAXSZ) { fail(c, "BAD FILE FROM THE HOST"); goto out; }
        char p[MAX_PATH]; snprintf(p, sizeof p, "%s\\%s", inc, names[i]);
        FILE *fp = fopen(p, "wb");
        if (!fp) { fail(c, "COULD NOT WRITE THE SAVE"); goto out; }
        c->total += size;
        while (size) {
            int want = size < sizeof buf ? (int)size : (int)sizeof buf;
            if (!recv_all(s, buf, want)) { fclose(fp); fail(c, "COPY INTERRUPTED"); goto out; }
            fwrite(buf, 1, (size_t)want, fp); size -= (uint32_t)want; c->bytes += (uint32_t)want;
        }
        fclose(fp); c->done_files = (int)i + 1;
    }
    /* back up what's there, then move the new files in */
    {
        SYSTEMTIME t; GetLocalTime(&t);
        const char *prof = strrchr(F->folder, 92); prof = prof ? prof + 1 : F->folder;
        char bk[MAX_PATH]; CreateDirectoryA(F->backup, NULL);
        snprintf(bk, sizeof bk, "%s\\%s-%04d%02d%02d-%02d%02d%02d", F->backup, prof, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
        Name old[SAVE_MAXF]; int no = list_saves(F->folder, old, SAVE_MAXF);
        if (no) {
            if (!CreateDirectoryA(bk, NULL)) { fail(c, "COULD NOT BACK UP YOUR SAVE"); goto out; }
            for (int i = 0; i < no; i++) {
                char from[MAX_PATH], to2[MAX_PATH];
                snprintf(from, sizeof from, "%s\\%s", F->folder, old[i].name); snprintf(to2, sizeof to2, "%s\\%s", bk, old[i].name);
                if (!MoveFileA(from, to2)) { fail(c, "COULD NOT BACK UP YOUR SAVE"); goto out; }
            }
        }
        CreateDirectoryA(F->folder, NULL);
        for (uint32_t i = 0; i < cnt; i++) {
            char from[MAX_PATH], to2[MAX_PATH];
            snprintf(from, sizeof from, "%s\\%s", inc, names[i]); snprintf(to2, sizeof to2, "%s\\%s", F->folder, names[i]);
            if (!MoveFileExA(from, to2, MOVEFILE_REPLACE_EXISTING)) { fail(c, "COULD NOT WRITE THE SAVE"); goto out; }
        }
        RemoveDirectoryA(inc);
        snprintf(c->msg, sizeof c->msg, no ? "SAVE COPIED. YOURS IS BACKED UP" : "SAVE COPIED");
        c->state = LBC_DONE;
    }
out:
    if (s != INVALID_SOCKET) closesocket(s);
    free(F);
    return 0;
}

int lbnet_fetch_saves(LbAddr host, uint32_t lobby, const char *folder, const char *backup_root, LbCopy *c) {
    if (c->state == LBC_RUNNING) return 0;
    wsa();
    memset(c, 0, sizeof *c); c->state = LBC_RUNNING;
    Fetch *F = (Fetch *)calloc(1, sizeof *F);
    F->host = host; F->lobby = lobby; F->c = c;
    lstrcpynA(F->folder, folder, MAX_PATH); lstrcpynA(F->backup, backup_root, MAX_PATH);
    HANDLE h = CreateThread(NULL, 0, fetch_thread, F, 0, NULL);
    if (!h) { free(F); fail(c, "COULD NOT START THE COPY"); return 0; }
    CloseHandle(h);
    return 1;
}

/* ---------- launch ---------- */
int lb_launch_game(const char *game_dir, const LbLaunch *l, char *err, int errcap) {
    char exe[MAX_PATH]; snprintf(exe, sizeof exe, "%s\\AI.exe", game_dir);
    if (GetFileAttributesA(exe) == INVALID_FILE_ATTRIBUTES) { snprintf(err, (size_t)errcap, "AI.EXE NOT FOUND NEXT TO THE LOBBY"); return 0; }
    char peer[32];
    SetEnvironmentVariableA("AIC_COOP_MODE", l->host ? "host" : "join");
    if (l->host) SetEnvironmentVariableA("AIC_COOP_PEER", NULL);
    else { lbnet_ip_str(l->host_ip, peer, sizeof peer); snprintf(peer + strlen(peer), sizeof peer - strlen(peer), ":%d", LB_GAME_PORT); SetEnvironmentVariableA("AIC_COOP_PEER", peer); }
    SetEnvironmentVariableA("AIC_COOP_NAME", l->name && *l->name ? l->name : NULL);
    SetEnvironmentVariableA("AIC_SKIN", l->skin && *l->skin ? l->skin : NULL);
    SetEnvironmentVariableA("AIC_SKINS", l->skins ? "1" : NULL);
    if (l->profile && *l->profile) SetEnvironmentVariableA("AIC_PROFILE", l->profile);
    STARTUPINFOA si = {0}; si.cb = sizeof si; PROCESS_INFORMATION pi = {0};
    if (!CreateProcessA(exe, NULL, NULL, NULL, FALSE, 0, NULL, game_dir, &si, &pi)) {
        snprintf(err, (size_t)errcap, "COULD NOT START THE GAME (ERROR %lu)", GetLastError());
        return 0;
    }
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    return 1;
}
