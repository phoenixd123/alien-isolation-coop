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

int probe_install(const char *dir, const char *profile, void (*logf)(const char *, ...));
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
    c->ok = 0;
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
        PresencePkt k; struct sockaddr_in from; int fl = sizeof from, n;
        if (g_steam) n = steamnet_recv(&k, sizeof k);
        else n = recvfrom(g_sock, (char *)&k, sizeof k, 0, (struct sockaddr *)&from, &fl);
        if (n <= 0) return;
        if (!proto_check(&k, n)) continue;
        if (!g_steam && g_learn_peer && (!g_peer_fixed || from.sin_addr.s_addr != g_peer_addr.sin_addr.s_addr || from.sin_port != g_peer_addr.sin_port)) {
            g_peer_addr = from; g_peer_fixed = 1;   /* host: talk back to the joiner (latest address wins, handles NAT rebinding) */
            char ip[32]; inet_ntop(AF_INET, &from.sin_addr, ip, sizeof ip);
            logf_("net: peer joined from %s:%u", ip, ntohs(from.sin_port));
        }
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

static void draw(HDC dc, int W, int H) {
    RECT all = {0, 0, W, H};
    HBRUSH black = CreateSolidBrush(RGB(0, 0, 0)); FillRect(dc, &all, black); DeleteObject(black);
    Cam me; Peer pe;
    EnterCriticalSection(&g_cs); me = g_me; pe = g_peer; LeaveCriticalSection(&g_cs);
    SetBkMode(dc, TRANSPARENT);
    char line[160];
    int age = pe.valid ? (int)(GetTickCount() - pe.t_ms) : -1;
    int live = pe.valid && age < 1500;
    if (!live) {
        snprintf(line, sizeof line, "CO-OP: waiting for %s...", pe.valid ? pe.name : "partner");
        SetTextColor(dc, RGB(200, 200, 200)); TextOutA(dc, 12, 10, line, (int)strlen(line));
        if (g_steam) {
            if (steamnet_self()) snprintf(line, sizeof line, "Your Steam ID: %llu  (your friend joins with it)", (unsigned long long)steamnet_self());
            else snprintf(line, sizeof line, "Connecting to Steam...");
            TextOutA(dc, 12, 34, line, (int)strlen(line));
        }
        return;
    }
    if (!me.ok) {
        snprintf(line, sizeof line, "CO-OP: %s connected (you are not in a level)", pe.name);
        SetTextColor(dc, RGB(120, 255, 120)); TextOutA(dc, 12, 10, line, (int)strlen(line));
        return;
    }
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

static DWORD WINAPI overlay_thread(LPVOID p) {
    (void)p;
    WNDCLASSA wc = {0}; wc.lpfnWndProc = ov_proc; wc.hInstance = GetModuleHandleA(NULL); wc.lpszClassName = "AICoopOverlay";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW); RegisterClassA(&wc);
    while (!g_quit && !g_game) { EnumWindows(find_game, (LPARAM)&g_game); Sleep(500); }
    if (g_quit) return 0;
    logf_("overlay: game window %p", (void *)g_game);
    g_ov = CreateWindowExA(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                           "AICoopOverlay", "", WS_POPUP, 0, 0, 10, 10, NULL, NULL, wc.hInstance, NULL);
    SetLayeredWindowAttributes(g_ov, RGB(0, 0, 0), 0, LWA_COLORKEY);
    while (!g_quit) {
        MSG msg; while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageA(&msg); }
        if (!IsWindow(g_game)) break;
        RECT cr; POINT tl = {0, 0}; GetClientRect(g_game, &cr); ClientToScreen(g_game, &tl);
        /* Only draw while the game itself is the active window: never paint over other apps. */
        maybe_dump(cr.right, cr.bottom);
        int show = !IsIconic(g_game) && cr.right > 0 && GetForegroundWindow() == g_game;
        SetWindowPos(g_ov, HWND_TOPMOST, tl.x, tl.y, cr.right, cr.bottom, SWP_NOACTIVATE | (show ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
        InvalidateRect(g_ov, NULL, FALSE);
        Sleep(16);
    }
    return 0;
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
        net_recv();
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
    logf_("coopnet up: profile=%s name=%s fwd_sign=%d", g_profile, g_name, g_fwd_sign);
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
