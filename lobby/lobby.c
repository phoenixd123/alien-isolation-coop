/* lobby.exe - the co-op launcher. Host a game or pick one from your network, both press ready, and both games
 * start already set up to connect. Put it in the game folder next to AI.exe.
 *
 *   lobby.exe                 normal use
 *   lobby.exe --local         only talks to this PC (two copies on one PC, for testing)
 *
 * Internet tab: Steam lobbies (lobbysteam.c). Host one and friends see it in their list, or invite them; the lobby
 * then works the same as on a LAN, over Steam.
 *
 * Remembers your name, skin and the last address you typed in lobby.ini next to it. Uses the co-op save
 * profile in AIC_PROFILE if set (two copies on one PC), otherwise "default".
 */
#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "lobbyproto.h"
#include "lobbynet.h"
#include "lobbyui.h"
#include "lobbysteam.h"
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
static int g_tab;
static int g_bodies, g_focus, g_hover, g_screen = LS_MAIN, g_launched;
static DWORD g_launch_t;
static char g_toast[128]; static int g_toast_bad; static DWORD g_toast_t;
static LbPlayer g_me;
static Lobby g_L;
static LbNet g_net;
static LbCopy g_copy; static int g_copy_seen;
static int g_local;
/* Steam lobby */
static int g_steam_want;            /* 1 hosting, 2 joining: waiting for Steam to put us in the lobby */
static int g_via_steam;             /* the lobby runs over Steam */
static LbsFriend g_friends[16]; static int g_nfriends; static DWORD g_friends_t;
static uint64_t g_invited[8]; static int g_ninvited;
static FILE *g_logf;
static void lg(const char *fmt, ...) {
    if (!g_logf) return;
    SYSTEMTIME t; GetLocalTime(&t);
    fprintf(g_logf, "%02d:%02d:%02d ", t.wHour, t.wMinute, t.wSecond);
    va_list a; va_start(a, fmt); vfprintf(g_logf, fmt, a); va_end(a);
    fputc('\n', g_logf); fflush(g_logf);
}
/* lobby packets: Steam accounts over P2P, the rest on the LAN */
static void xsend(void *ctx, LbAddr d, const void *b, int n) { (void)ctx; if (lbs_is_steam(d)) lbs_send(NULL, d, b, n); else lbnet_send(&g_net, d, b, n); }
static void steam_up(void) {
    if (lbs_ready()) return;
    static DWORD last; if (last && GetTickCount() - last < 5000) return;
    last = GetTickCount();
    lbs_init(g_dir, lg);
}

static void toast(const char *m, int bad) { lstrcpynA(g_toast, m, sizeof g_toast); g_toast_bad = bad; g_toast_t = GetTickCount(); }
static uint32_t now_ms(void) { return GetTickCount(); }

static void save_ini(void) {
    WritePrivateProfileStringA("lobby", "name", g_name, g_ini);
    WritePrivateProfileStringA("lobby", "skin", g_skin, g_ini);
    WritePrivateProfileStringA("lobby", "bodies", g_bodies ? "1" : "0", g_ini);
    WritePrivateProfileStringA("lobby", "last_address", g_ip, g_ini);
    WritePrivateProfileStringA("lobby", "tab", g_tab ? "internet" : "lan", g_ini);
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
    if (g_via_steam || g_steam_want) lbs_leave();
    g_via_steam = 0; g_steam_want = 0; g_ninvited = 0;
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

/* Steam lobby: host one, or join a friend's (from the list or an invite). The lobby screen opens once Steam has
   put us in it; then the usual lobby packets go over Steam. */
static void do_steam_host(void) {
    if (!need_name()) return;
    steam_up();
    if (!lbs_ready()) { toast("STEAM ISN'T RUNNING, OR THE GAME ISN'T ON THIS ACCOUNT", 1); return; }
    if (lbs_host()) { g_steam_want = 1; toast("CREATING A STEAM LOBBY...", 0); }
}
static void do_steam_join(uint64_t lobby) {
    if (!need_name()) return;
    steam_up();
    if (!lbs_ready()) { toast("STEAM ISN'T RUNNING", 1); return; }
    if (lbs_join(lobby)) { g_steam_want = 2; toast("JOINING OVER STEAM...", 0); }
}
static void steam_step(uint32_t now) {
    if (!lbs_ready()) return;
    lbs_tick();
    uint64_t inv = lbs_take_invite();
    if (inv && g_screen == LS_MAIN) { lg("steam: invite accepted, lobby %llu", (unsigned long long)inv); do_steam_join(inv); }
    if (g_steam_want && lbs_state() == LBS_FAILED) { toast(g_steam_want == 1 ? "STEAM COULDN'T CREATE A LOBBY" : "COULDN'T JOIN THAT STEAM LOBBY", 1); g_steam_want = 0; lbs_leave(); }
    if (g_steam_want && lbs_state() == LBS_IN) {
        uint64_t owner = lbs_owner();
        if (g_steam_want == 1 && owner == lbs_self()) {
            refresh_me();
            lb_host(&g_L, &g_me, lbnet_random());
            lbs_serve_saves(g_saves, g_L.lobby);
            g_screen = LS_LOBBY; g_focus = 0; g_via_steam = 1; g_ninvited = 0; g_toast[0] = 0;
            save_ini();
        } else if (g_steam_want == 2 && owner && owner != lbs_self()) {
            /* ask the host's lobby window for its beacon over Steam; lb_probe_answer() then joins it */
            LbLeave k; lb_head(&g_L, &k.h, LB_PROBE); k.h.lobby = 0; LB_SEAL(k);
            lbs_send(NULL, lbs_addr(owner), &k, sizeof k);
            g_L.probe_ip = lbs_addr(owner).ip; g_L.probe_t = now | 1;
            g_via_steam = 1;
        }
        g_steam_want = 0;
    }
    if (!g_friends_t || GetTickCount() - g_friends_t > 2000) { g_nfriends = lbs_friends(g_friends, 16); g_friends_t = GetTickCount() | 1; }
}
static void do_invite(int i) {
    if (i < 0 || i >= g_nfriends) return;
    if (lbs_invite(g_friends[i].id)) {
        if (g_ninvited < 8) g_invited[g_ninvited++] = g_friends[i].id;
        char m[96]; snprintf(m, sizeof m, "INVITE SENT TO %s. THEY ACCEPT IT IN STEAM WITH LOBBY.EXE OPEN", g_friends[i].name);
        for (char *q = m; *q; q++) if (*q >= 'a' && *q <= 'z') *q -= 32;
        toast(m, 0);
    } else toast("STEAM DIDN'T SEND THE INVITE", 1);
}

static void launch(void) {
    save_ini();
    LbLaunch l; memset(&l, 0, sizeof l);
    l.host = g_L.role == LB_HOSTING; l.host_ip = g_L.peer_addr.ip;
    if (g_via_steam && lbs_is_steam(g_L.peer_addr)) { l.steam = 1; l.peer_steam = lbs_id(g_L.peer_addr); lbs_shutdown(); }   /* the game takes Steam over */
    l.name = g_name; l.skin = g_skin; l.skins = g_bodies;
    l.profile = g_profile[0] && strcmp(g_profile, "default") ? g_profile : NULL;
    char err[96];
    if (!lb_launch_game(g_dir, &l, err, sizeof err)) { toast(err, 1); g_L.launched = 0; g_L.counting = 0; return; }
    g_launched = 1; g_launch_t = GetTickCount();
    lbnet_stop_saves(&g_net);
}

static void copy_steam_id(void) {
    uint64_t id = lbs_self() ? lbs_self() : lbnet_my_steam_id();
    if (!id || !OpenClipboard(g_wnd)) return;
    char t[24]; int n = snprintf(t, sizeof t, "%llu", (unsigned long long)id);
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, (SIZE_T)n + 1);
    if (h) { memcpy(GlobalLock(h), t, (size_t)n + 1); GlobalUnlock(h); EmptyClipboard(); SetClipboardData(CF_TEXT, h); }
    CloseClipboard();
    toast("STEAM ID COPIED (FOR PLAY-COOP.BAT STEAM-JOIN)", 0);
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
    case LH_TAB_LAN: g_tab = 0; save_ini(); break;
    case LH_TAB_NET: g_tab = 1; save_ini(); steam_up(); break;
    case LH_COPY_ID: copy_steam_id(); break;
    case LH_STEAM_HOST: do_steam_host(); break;
    case LH_JOIN_IP: do_join_ip(); break;
    case LH_LEAVE: lb_leave(&g_L, xsend, NULL); go_main(); break;
    case LH_READY:
        if (g_copy.state == LBC_RUNNING) { toast("WAIT FOR THE SAVE COPY TO FINISH", 1); break; }
        lb_set_ready(&g_L, !g_L.me.ready); break;
    case LH_START: lb_start(&g_L, now, xsend, NULL); break;
    case LH_KICK: lb_kick(&g_L, xsend, NULL, now); toast("PLAYER REMOVED", 0); break;
    case LH_COPY:
        lb_set_ready(&g_L, 0);
        if (lbs_is_steam(g_L.peer_addr) ? !lbs_fetch_saves(lbs_id(g_L.peer_addr), g_L.lobby, g_saves, g_backup, &g_copy)
                                        : !lbnet_fetch_saves(g_L.peer_addr, g_L.lobby, g_saves, g_backup, &g_copy)) toast("COULD NOT START THE COPY", 1);
        g_copy_seen = 0;
        break;
    default:
        if (id >= LH_ROW && id < LH_ROW + g_L.nfound) { LbFound f = g_L.found[id - LH_ROW]; do_join(&f); }
        else if (id >= LH_FRIEND && id < LH_FRIEND + g_nfriends && g_friends[id - LH_FRIEND].hosting) do_steam_join(g_friends[id - LH_FRIEND].lobby);
        else if (id >= LH_INVITE && id < LH_INVITE + g_nfriends) do_invite(id - LH_INVITE);
    }
}

static void tick(void) {
    uint32_t now = now_ms();
    uint8_t buf[512]; LbAddr from; int n;
    steam_step(now);
    while ((n = lbnet_recv(&g_net, &from, buf, sizeof buf)) > 0) lb_on_packet(&g_L, from, buf, n, now, xsend, NULL);
    while ((n = lbs_recv(&from, buf, sizeof buf)) > 0) lb_on_packet(&g_L, from, buf, n, now, xsend, NULL);
    sync_me();
    lb_tick(&g_L, now, xsend, NULL);

    /* join by address: the probed host answered (or didn't) */
    int pa = lb_probe_answer(&g_L, now);
    if (pa >= 0) { LbFound f = g_L.found[pa]; g_toast[0] = 0; do_join(&f); }
    else if (pa == -2 && g_via_steam) { toast("THE HOST'S LOBBY DIDN'T ANSWER OVER STEAM. TRY AGAIN", 1); go_main(); }
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
    static uint64_t sid; static uint32_t sid_t;
    if (!sid_t || v.now - sid_t > 3000) { sid = lbs_self() ? lbs_self() : lbnet_my_steam_id(); sid_t = v.now | 1; }
    v.tab = g_tab; v.my_steam = sid;
    v.steam_up = lbs_ready(); v.steam_busy = g_steam_want != 0; v.via_steam = g_via_steam;
    v.friends = g_friends; v.nfriends = g_nfriends; v.invited = g_invited; v.ninvited = g_ninvited;
    v.L = &g_L; v.copy = &g_copy;
    static uint32_t ip, ip_t;
    if (!ip_t || v.now - ip_t > 5000) { ip = lbnet_local_ip(); ip_t = v.now | 1; }
    if (ip) lbnet_ip_str(ip, v.my_ip, sizeof v.my_ip);
    if (g_local) lstrcpynA(v.my_ip, "127.0.0.1", sizeof v.my_ip);
    if (g_via_steam) lstrcpynA(v.my_ip, "STEAM", sizeof v.my_ip);
    if (g_L.have_peer || g_L.role == LB_JOINING) {
        if (lbs_is_steam(g_L.peer_addr)) lstrcpynA(v.peer_ip, "STEAM", sizeof v.peer_ip);
        else lbnet_ip_str(g_L.peer_addr.ip, v.peer_ip, sizeof v.peer_ip);
    }
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
    else if (ch == 9) g_focus = g_focus == LH_NAME ? (g_tab ? LH_NAME : LH_IP) : LH_NAME;
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
        if (!g_launched) lb_leave(&g_L, xsend, NULL);
        lbs_shutdown();
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
    { char t[16]; GetPrivateProfileStringA("lobby", "tab", "lan", t, sizeof t, g_ini); g_tab = !strcmp(t, "internet"); }
    g_bodies = GetPrivateProfileIntA("lobby", "bodies", 0, g_ini) != 0;
    lb_clean(g_name, sizeof g_name);
    { const Skin *k = skin_find(g_skin); if (!k || k->experimental) lstrcpynA(g_skin, "TAYLOR", sizeof g_skin); }
    if (!g_name[0]) g_focus = LH_NAME;

    { char lp[MAX_PATH]; snprintf(lp, sizeof lp, "%s\\coop_logs", g_dir); CreateDirectoryA(lp, NULL);
      snprintf(lp, sizeof lp, "%s\\coop_logs\\lobby.log", g_dir); g_logf = fopen(lp, "a"); }
    refresh_me();
    if (g_tab) steam_up();
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
