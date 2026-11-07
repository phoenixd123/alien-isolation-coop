/* lobby.exe - the co-op launcher. Host a game or pick one from your network, both press ready, and both games
 * start already set up to connect. Put it in the game folder next to AI.exe.
 *
 *   lobby.exe                 normal use
 *   lobby.exe --local         only talks to this PC (two copies on one PC, for testing)
 *
 * Remembers your name, skin and the last address you typed in lobby.ini next to it. Uses the co-op save
 * profile in AIC_PROFILE if set (two copies on one PC), otherwise "default".
 */
#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "lobbyproto.h"
#include "lobbynet.h"
#include "lobbyui.h"
#include "../coopnet/skins.h"
#include "../coopnet/version.h"

#define BASE_W 1000
#define BASE_H 640
#define TIMER_MS 33
#define TOAST_MS 6000

static HWND g_wnd;
static int g_W, g_H;
static HDC g_memdc; static HBITMAP g_dib; static uint32_t *g_px;
static LbHit g_hits[LB_MAXHITS]; static int g_nhits;

static char g_dir[MAX_PATH], g_ini[MAX_PATH], g_saves[MAX_PATH], g_backup[MAX_PATH], g_profile[64];
static char g_name[24], g_ip[24], g_skin[24], g_version[48];
static int g_bodies, g_focus, g_hover, g_screen = LS_MAIN, g_launched;
static DWORD g_launch_t;
static char g_toast[128]; static int g_toast_bad; static DWORD g_toast_t;
static LbPlayer g_me;
static Lobby g_L;
static LbNet g_net;
static LbCopy g_copy; static int g_copy_seen;
static int g_local;

static void toast(const char *m, int bad) { lstrcpynA(g_toast, m, sizeof g_toast); g_toast_bad = bad; g_toast_t = GetTickCount(); }
static uint32_t now_ms(void) { return GetTickCount(); }

static void save_ini(void) {
    WritePrivateProfileStringA("lobby", "name", g_name, g_ini);
    WritePrivateProfileStringA("lobby", "skin", g_skin, g_ini);
    WritePrivateProfileStringA("lobby", "bodies", g_bodies ? "1" : "0", g_ini);
    WritePrivateProfileStringA("lobby", "last_address", g_ip, g_ini);
}

/* Who we are, with fresh fingerprints (the save changes after a copy). */
static void refresh_me(void) {
    char p[MAX_PATH];
    memset(&g_me, 0, sizeof g_me);
    lstrcpynA(g_me.name, g_name, sizeof g_me.name);
    lstrcpynA(g_me.skin, g_skin, sizeof g_me.skin);
    snprintf(p, sizeof p, "%s\\coopnet.dll", g_dir); g_me.mod = lb_hash_file(p);
    snprintf(p, sizeof p, "%s\\AI.exe", g_dir); g_me.game = lb_game_build(p);
    g_me.save = lb_hash_saves(g_saves);
    g_me.present = 1;
    if (g_me.mod) snprintf(g_version, sizeof g_version, "V%s  BUILD %08X", AIC_RELEASE, g_me.mod);
    else snprintf(g_version, sizeof g_version, "V%s  COOPNET.DLL MISSING", AIC_RELEASE);
}
/* Copy name and skin into the running lobby (they can change on the main screen only, but keep it simple). */
static void sync_me(void) {
    uint8_t ready = g_L.me.ready;
    lstrcpynA(g_L.me.name, g_name, sizeof g_L.me.name); lstrcpynA(g_L.me.skin, g_skin, sizeof g_L.me.skin);
    g_L.me.mod = g_me.mod; g_L.me.game = g_me.game; g_L.me.save = g_me.save; g_L.me.ready = ready;
}

static void cycle_skin(int d) {
    int idx = 0, n = 0, list[NSKINS];
    for (int i = 0; i < NSKINS; i++) if (!SKINS[i].experimental) list[n++] = i;
    for (int i = 0; i < n; i++) if (!strcmp(SKINS[list[i]].name, g_skin)) idx = i;
    idx = (idx + d + n) % n;
    lstrcpynA(g_skin, SKINS[list[idx]].name, sizeof g_skin);
    save_ini();
}

static int need_name(void) {
    if (g_name[0]) return 1;
    toast("TYPE YOUR NAME FIRST", 1); g_focus = LH_NAME;
    return 0;
}

static void go_main(void) {
    lbnet_stop_saves(&g_net);
    lb_browse(&g_L, &g_me, lbnet_random());
    g_screen = LS_MAIN;
}

static void do_host(void) {
    if (!need_name()) return;
    refresh_me();
    lb_host(&g_L, &g_me, lbnet_random());
    if (!lbnet_serve_saves(&g_net, g_saves, g_L.lobby)) toast("SAVE COPY UNAVAILABLE (PORT IN USE)", 1);
    g_screen = LS_LOBBY; g_focus = 0; save_ini();
}

static void do_join(const LbFound *f) {
    if (!need_name()) return;
    if (!f->open) { toast("THAT GAME IS FULL", 1); return; }
    refresh_me();
    lb_join(&g_L, &g_me, lbnet_random(), f->addr, f->lobby, now_ms());
    g_screen = LS_LOBBY; g_focus = 0; save_ini();
}

static void do_join_ip(void) {
    if (!need_name()) return;
    uint32_t ip;
    if (!lbnet_parse_ip(g_ip, &ip)) { toast("THAT ISN'T AN ADDRESS. IT LOOKS LIKE 192.168.1.20", 1); return; }
    lb_probe(&g_L, ip, now_ms(), lbnet_send, &g_net);
    toast("ASKING THAT ADDRESS FOR A GAME...", 0);
    save_ini();
}

static void launch(void) {
    save_ini();
    LbLaunch l; memset(&l, 0, sizeof l);
    l.host = g_L.role == LB_HOSTING; l.host_ip = g_L.peer_addr.ip;
    l.name = g_name; l.skin = g_skin; l.skins = g_bodies;
    l.profile = g_profile[0] && strcmp(g_profile, "default") ? g_profile : NULL;
    char err[96];
    if (!lb_launch_game(g_dir, &l, err, sizeof err)) { toast(err, 1); g_L.launched = 0; g_L.counting = 0; return; }
    g_launched = 1; g_launch_t = GetTickCount();
    lbnet_stop_saves(&g_net);
}

static void click(int id) {
    uint32_t now = now_ms();
    g_focus = (id == LH_NAME || id == LH_IP) ? id : 0;
    switch (id) {
    case LH_CLOSE: PostMessageA(g_wnd, WM_CLOSE, 0, 0); break;
    case LH_MIN: ShowWindow(g_wnd, SW_MINIMIZE); break;
    case LH_SKIN_PREV: cycle_skin(-1); break;
    case LH_SKIN_NEXT: cycle_skin(1); break;
    case LH_BODIES: g_bodies = !g_bodies; save_ini(); break;
    case LH_HOST: do_host(); break;
    case LH_JOIN_IP: do_join_ip(); break;
    case LH_LEAVE: lb_leave(&g_L, lbnet_send, &g_net); go_main(); break;
    case LH_READY:
        if (g_copy.state == LBC_RUNNING) { toast("WAIT FOR THE SAVE COPY TO FINISH", 1); break; }
        lb_set_ready(&g_L, !g_L.me.ready); break;
    case LH_START: lb_start(&g_L, now, lbnet_send, &g_net); break;
    case LH_KICK: lb_kick(&g_L, lbnet_send, &g_net, now); toast("PLAYER REMOVED", 0); break;
    case LH_COPY:
        lb_set_ready(&g_L, 0);
        if (!lbnet_fetch_saves(g_L.peer_addr, g_L.lobby, g_saves, g_backup, &g_copy)) toast("COULD NOT START THE COPY", 1);
        g_copy_seen = 0;
        break;
    default:
        if (id >= LH_ROW && id < LH_ROW + g_L.nfound) { LbFound f = g_L.found[id - LH_ROW]; do_join(&f); }
    }
}

static void tick(void) {
    uint32_t now = now_ms();
    uint8_t buf[512]; LbAddr from; int n;
    while ((n = lbnet_recv(&g_net, &from, buf, sizeof buf)) > 0) lb_on_packet(&g_L, from, buf, n, now, lbnet_send, &g_net);
    sync_me();
    lb_tick(&g_L, now, lbnet_send, &g_net);

    /* join by address: the probed host answered (or didn't) */
    int pa = lb_probe_answer(&g_L, now);
    if (pa >= 0) { LbFound f = g_L.found[pa]; g_toast[0] = 0; do_join(&f); }
    else if (pa == -2) { char ip[24], m[128]; lbnet_ip_str(g_L.probe_ip, ip, sizeof ip); snprintf(m, sizeof m, "NO GAME AT %s. CHECK THE ADDRESS AND THE FIREWALL", ip); toast(m, 1); }

    /* dropped out of a lobby */
    if (g_screen == LS_LOBBY && g_L.role == LB_IDLE && !g_launched) {
        static const char *why[] = {"LEFT THE LOBBY", "THAT GAME IS FULL", "THE HOST REMOVED YOU", "THE HOST CLOSED THE LOBBY",
                                    "LOST THE CONNECTION TO THE HOST", "NO ANSWER FROM THE HOST. CHECK THE FIREWALL ON BOTH PCS"};
        toast(why[g_L.reason >= 0 && g_L.reason <= LB_NOREPLY ? g_L.reason : 0], g_L.reason != LB_OK);
        go_main();
    }

    /* save copy finished */
    if (!g_copy_seen && (g_copy.state == LBC_DONE || g_copy.state == LBC_FAILED)) {
        g_copy_seen = 1;
        toast(g_copy.msg, g_copy.state == LBC_FAILED);
        refresh_me();
    }

    if (!g_launched && lb_should_launch(&g_L, now)) launch();
    if (g_launched && GetTickCount() - g_launch_t > 2500) PostMessageA(g_wnd, WM_CLOSE, 0, 0);
    if (g_toast[0] && GetTickCount() - g_toast_t > TOAST_MS) g_toast[0] = 0;
}

static void paint(void) {
    LbView v; memset(&v, 0, sizeof v);
    v.now = now_ms(); v.screen = g_screen; v.version = g_version;
    v.name = g_name; v.ip_text = g_ip; v.focus = g_focus; v.skin = g_skin; v.bodies = g_bodies;
    v.found = g_L.found; v.nfound = g_L.nfound; v.me_main = &g_me;
    v.L = &g_L; v.copy = &g_copy;
    static uint32_t ip, ip_t;
    if (!ip_t || v.now - ip_t > 5000) { ip = lbnet_local_ip(); ip_t = v.now | 1; }
    if (ip) lbnet_ip_str(ip, v.my_ip, sizeof v.my_ip);
    if (g_local) lstrcpynA(v.my_ip, "127.0.0.1", sizeof v.my_ip);
    if (g_L.have_peer || g_L.role == LB_JOINING) lbnet_ip_str(g_L.peer_addr.ip, v.peer_ip, sizeof v.peer_ip);
    v.countdown = lb_countdown(&g_L, v.now); v.launched = g_launched;
    v.toast = g_toast; v.toast_bad = g_toast_bad; v.hover = g_hover;
    lbui_render(&v, g_px, g_W, g_H, g_hits, &g_nhits);
    HDC dc = GetDC(g_wnd);
    BitBlt(dc, 0, 0, g_W, g_H, g_memdc, 0, 0, SRCCOPY);
    ReleaseDC(g_wnd, dc);
}

static void type_char(unsigned ch) {
    char *buf = g_focus == LH_NAME ? g_name : g_focus == LH_IP ? g_ip : NULL;
    if (!buf) return;
    int cap = g_focus == LH_NAME ? 20 : 15, n = (int)strlen(buf);
    if (ch == 8) { if (n) buf[n - 1] = 0; }
    else if (ch == 13) { if (g_focus == LH_IP) do_join_ip(); g_focus = 0; }
    else if (ch == 27) g_focus = 0;
    else if (ch == 9) g_focus = g_focus == LH_NAME ? LH_IP : LH_NAME;
    else if (n < cap) {
        int ok = g_focus == LH_NAME ? (ch >= 32 && ch < 127) : ((ch >= '0' && ch <= '9') || ch == '.');
        if (ok) { buf[n] = (char)ch; buf[n + 1] = 0; }
    }
    if (g_focus == 0 || ch == 13) save_ini();
}

static void paste(void) {
    if (!g_focus || !OpenClipboard(g_wnd)) return;
    HANDLE h = GetClipboardData(CF_TEXT);
    const char *t = h ? (const char *)GlobalLock(h) : NULL;
    if (t) { for (int i = 0; t[i] && i < 64; i++) if (t[i] >= 32) type_char((unsigned char)t[i]); GlobalUnlock(h); }
    CloseClipboard();
}

static LRESULT CALLBACK wndproc(HWND w, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_TIMER: tick(); paint(); return 0;
    case WM_PAINT: { PAINTSTRUCT ps; BeginPaint(w, &ps); EndPaint(w, &ps); paint(); return 0; }
    case WM_ERASEBKGND: return 1;
    case WM_MOUSEMOVE: {
        float x = (float)(short)LOWORD(lp), y = (float)(short)HIWORD(lp);
        g_hover = lbui_hit(g_hits, g_nhits, x, y);
        SetCursor(LoadCursorA(NULL, g_hover ? (LPCSTR)IDC_HAND : (LPCSTR)IDC_ARROW));
        return 0;
    }
    case WM_SETCURSOR: if (LOWORD(lp) == HTCLIENT) { SetCursor(LoadCursorA(NULL, g_hover ? (LPCSTR)IDC_HAND : (LPCSTR)IDC_ARROW)); return TRUE; } break;
    case WM_LBUTTONDOWN: {
        float x = (float)(short)LOWORD(lp), y = (float)(short)HIWORD(lp);
        int id = lbui_hit(g_hits, g_nhits, x, y);
        if (id) { click(id); paint(); return 0; }
        g_focus = 0;
        if (lbui_in_titlebar(g_W, g_H, x, y)) { ReleaseCapture(); SendMessageA(w, WM_NCLBUTTONDOWN, HTCAPTION, 0); }
        return 0;
    }
    case WM_CHAR: type_char((unsigned)wp); paint(); return 0;
    case WM_KEYDOWN: if (wp == 'V' && (GetKeyState(VK_CONTROL) & 0x8000)) { paste(); paint(); } return 0;
    case WM_CLOSE:
        if (!g_launched) lb_leave(&g_L, lbnet_send, &g_net);
        save_ini();
        DestroyWindow(w); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcA(w, m, wp, lp);
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show) {
    (void)prev;
    g_local = cmd && strstr(cmd, "--local") != NULL;
    SetProcessDPIAware();

    GetModuleFileNameA(NULL, g_dir, sizeof g_dir);
    char *sl = strrchr(g_dir, 92); if (sl) *sl = 0;
    snprintf(g_ini, sizeof g_ini, "%s\\lobby.ini", g_dir);
    if (!GetEnvironmentVariableA("AIC_PROFILE", g_profile, sizeof g_profile) || !g_profile[0]) lstrcpynA(g_profile, "default", sizeof g_profile);
    snprintf(g_saves, sizeof g_saves, "%s\\coop_saves\\%s", g_dir, g_profile);
    snprintf(g_backup, sizeof g_backup, "%s\\coop_saves\\backup", g_dir);
    GetPrivateProfileStringA("lobby", "name", "", g_name, sizeof g_name, g_ini);
    GetPrivateProfileStringA("lobby", "skin", "TAYLOR", g_skin, sizeof g_skin, g_ini);
    GetPrivateProfileStringA("lobby", "last_address", "", g_ip, sizeof g_ip, g_ini);
    g_bodies = GetPrivateProfileIntA("lobby", "bodies", 0, g_ini) != 0;
    lb_clean(g_name, sizeof g_name);
    { const Skin *k = skin_find(g_skin); if (!k || k->experimental) lstrcpynA(g_skin, "TAYLOR", sizeof g_skin); }
    if (!g_name[0]) g_focus = LH_NAME;

    refresh_me();
    if (!lbui_init()) { MessageBoxA(NULL, "Could not start GDI+.", "Co-op lobby", MB_ICONERROR); return 1; }
    if (!lbnet_open(&g_net, LB_PORT, g_local)) { MessageBoxA(NULL, "Could not open a network socket.", "Co-op lobby", MB_ICONERROR); return 1; }
    if (!g_net.disc) toast("ANOTHER PROGRAM USES PORT 45501. GAMES ON YOUR NETWORK WON'T SHOW UP; JOIN BY ADDRESS", 1);
    { char p[MAX_PATH]; snprintf(p, sizeof p, "%s\\AI.exe", g_dir); if (GetFileAttributesA(p) == INVALID_FILE_ATTRIBUTES) toast("AI.EXE ISN'T HERE. PUT LOBBY.EXE IN THE GAME FOLDER", 1); }
    lb_browse(&g_L, &g_me, lbnet_random());

    HDC sdc = GetDC(NULL); int dpi = GetDeviceCaps(sdc, LOGPIXELSY); ReleaseDC(NULL, sdc);
    g_W = BASE_W * dpi / 96; g_H = BASE_H * dpi / 96;

    WNDCLASSA wc = {0};
    wc.lpfnWndProc = wndproc; wc.hInstance = inst; wc.lpszClassName = "AicLobby";
    wc.hCursor = LoadCursorA(NULL, (LPCSTR)IDC_ARROW); wc.hIcon = LoadIconA(NULL, (LPCSTR)IDI_APPLICATION);
    RegisterClassA(&wc);
    int sx = (GetSystemMetrics(SM_CXSCREEN) - g_W) / 2, sy = (GetSystemMetrics(SM_CYSCREEN) - g_H) / 2;
    g_wnd = CreateWindowExA(0, "AicLobby", "Isolation co-op lobby", WS_POPUP | WS_MINIMIZEBOX | WS_SYSMENU,
                            sx, sy, g_W, g_H, NULL, NULL, inst, NULL);

    BITMAPINFO bi = {0};
    bi.bmiHeader.biSize = sizeof bi.bmiHeader; bi.bmiHeader.biWidth = g_W; bi.bmiHeader.biHeight = -g_H;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
    g_memdc = CreateCompatibleDC(NULL);
    g_dib = CreateDIBSection(g_memdc, &bi, DIB_RGB_COLORS, (void **)&g_px, NULL, 0);
    SelectObject(g_memdc, g_dib);

    ShowWindow(g_wnd, show);
    SetTimer(g_wnd, 1, TIMER_MS, NULL);
    MSG msg;
    while (GetMessageA(&msg, NULL, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageA(&msg); }
    lbnet_close(&g_net);
    return 0;
}
