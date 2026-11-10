/* Renders every lobby screen to PNG files so the window can be checked without opening it.
 * Usage: lobby_preview <out dir> [width height] */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../lobbyui.h"

typedef void GpImage;
int WINAPI GdipCreateBitmapFromScan0(INT, INT, INT, INT, BYTE *, GpImage **);
int WINAPI GdipSaveImageToFile(GpImage *, const WCHAR *, const CLSID *, const void *);
int WINAPI GdipDisposeImage(GpImage *);
static const CLSID PNG = {0x557CF406, 0x1A04, 0x11D3, {0x9A, 0x73, 0x00, 0x00, 0xF8, 0x1E, 0xF3, 0x2E}};

static uint32_t *buf; static int W, H; static const char *dir;
static LbHit hits[LB_MAXHITS]; static int nhits;

static void shot(const char *name, const LbView *v) {
    lbui_render(v, buf, W, H, hits, &nhits);
    GpImage *img = NULL;
    GdipCreateBitmapFromScan0(W, H, W * 4, 0x0026200A /* 32bpp ARGB */, (BYTE *)buf, &img);
    WCHAR p[MAX_PATH]; swprintf(p, MAX_PATH, L"%hs\\lobby_%hs.png", dir, name);
    int r = GdipSaveImageToFile(img, p, &PNG, NULL);
    GdipDisposeImage(img);
    printf("%s %s (%d clickable areas)\n", r == 0 ? "wrote" : "FAILED", name, nhits);
}

static LbPlayer player(const char *name, const char *skin, uint32_t save, int ready) {
    LbPlayer p; memset(&p, 0, sizeof p);
    lstrcpynA(p.name, name, sizeof p.name); lstrcpynA(p.skin, skin, sizeof p.skin);
    p.mod = 0x1111; p.game = 0x2222; p.save = save; p.ready = (uint8_t)ready; p.present = 1;
    return p;
}

int main(int argc, char **argv) {
    dir = argc > 1 ? argv[1] : ".";
    W = argc > 3 ? atoi(argv[2]) : 1000; H = argc > 3 ? atoi(argv[3]) : 640;
    if (!lbui_init()) { printf("GDI+ failed\n"); return 1; }
    buf = malloc((size_t)W * H * 4);
    LbPlayer me = player("Ripley", "TAYLOR", 0xAAAA, 0);
    LbView v; memset(&v, 0, sizeof v);
    v.version = "V0.4.0  BUILD 1A2B3C4D"; v.me_main = &me;

    /* main, nothing found yet, typing a name */
    v.now = 1200; v.screen = LS_MAIN; v.name = "Ripley"; v.ip_text = ""; v.focus = LH_NAME; v.skin = "TAYLOR";
    shot("1_scanning", &v);

    /* main with three games: open, full, different save; hovering the first */
    LbFound f[3]; memset(f, 0, sizeof f);
    f[0].addr.ip = 0xC0A8010Au; f[0].host = player("Sam", "SAMUELS", 0xAAAA, 1); f[0].players = 1; f[0].open = 1;
    f[1].addr.ip = 0xC0A80117u; f[1].host = player("Marlow", "MARLOW", 0xAAAA, 1); f[1].players = 2; f[1].open = 0;
    f[2].addr.ip = 0x0A930002u; f[2].host = player("Axel", "AXEL", 0xBBBB, 1); f[2].players = 1; f[2].open = 1;
    v.found = f; v.nfound = 3; v.focus = 0; v.hover = LH_ROW; v.bodies = 1; v.skin = "DR_KUHLMAN"; v.now = 1700;
    shot("2_games", &v);
    v.hover = 0; v.ip_text = "10.147.17.4"; v.focus = LH_IP; v.toast = "NO ANSWER FROM 10.147.17.9. CHECK THE ADDRESS AND THE FIREWALL"; v.toast_bad = 1;
    shot("3_join_by_ip", &v);
    v.toast = NULL; v.focus = 0;

    /* internet tab: Steam running, a friend's ID typed in */
    static const LbsFriend fr[3] = {{76561198087654321ull, 109775241000000001ull, "Dropper", 1, 1}, {76561198000000002ull, 0, "Amanda", 1, 0}, {76561198000000003ull, 0, "Ripley", 1, 0}};
    v.tab = 1; v.my_steam = 76561198012345678ull; v.steam_up = 1; v.friends = fr; v.nfriends = 3; v.focus = 0; v.hover = LH_FRIEND;
    shot("2b_internet", &v);
    v.my_steam = 0; v.steam_up = 0; v.friends = NULL; v.nfriends = 0; v.focus = 0; v.hover = 0;
    shot("2c_internet_no_steam", &v);
    v.tab = 0;

    /* hosting, waiting */
    Lobby L; memset(&L, 0, sizeof L);
    L.role = LB_HOSTING; L.me = player("Ripley", "TAYLOR", 0xAAAA, 1);
    v.screen = LS_LOBBY; v.L = &L; lstrcpynA(v.my_ip, "192.168.1.10", sizeof v.my_ip); v.now = 2100;
    shot("4_host_waiting", &v);
    v.via_steam = 1; v.friends = fr; v.nfriends = 3; { static const uint64_t inv[1] = {76561198000000002ull}; v.invited = inv; v.ninvited = 1; }
    lstrcpynA(v.my_ip, "STEAM", sizeof v.my_ip); v.hover = LH_INVITE;
    shot("4b_steam_lobby_invite", &v);
    v.via_steam = 0; v.friends = NULL; v.nfriends = 0; v.invited = NULL; v.ninvited = 0; v.hover = 0; lstrcpynA(v.my_ip, "192.168.1.10", sizeof v.my_ip);

    /* partner in, not ready, different save */
    L.have_peer = 1; L.peer = player("Sam", "SAMUELS", 0xBBBB, 0); lstrcpynA(v.peer_ip, "192.168.1.23", sizeof v.peer_ip);
    shot("5_host_partner_in", &v);

    /* partner ready, same save, hovering Start */
    L.peer.ready = 1; L.peer.save = 0xAAAA; v.hover = LH_START;
    shot("6_host_ready", &v);

    /* the joiner's view: different save, copying */
    Lobby J; memset(&J, 0, sizeof J);
    J.role = LB_JOINING; J.have_peer = 1; J.me = player("Sam", "SAMUELS", 0xBBBB, 0); J.peer = player("Ripley", "TAYLOR", 0xAAAA, 1);
    v.L = &J; v.hover = LH_COPY; lstrcpynA(v.peer_ip, "192.168.1.10", sizeof v.peer_ip); v.my_ip[0] = 0;
    shot("7_join_diff_save", &v);
    LbCopy cp; memset(&cp, 0, sizeof cp); cp.state = LBC_RUNNING; cp.total = 6300000; cp.bytes = 4100000; v.copy = &cp; v.hover = 0;
    shot("8_join_copying", &v);
    v.copy = NULL; J.me.save = 0xAAAA; J.me.ready = 1; v.toast = "SAVE COPIED. YOURS IS BACKED UP"; v.toast_bad = 0;
    shot("9_join_ready", &v);
    v.toast = NULL;

    /* joiner connecting */
    J.have_peer = 0; J.me.ready = 0;
    shot("10_join_connecting", &v);

    /* countdown and launch (host) */
    L.counting = 1; v.L = &L; v.countdown = 2; v.hover = 0;
    shot("11_countdown", &v);
    v.countdown = 0; v.launched = 1;
    shot("12_launching", &v);

    /* mismatched mod build */
    L.counting = 0; v.launched = 0; L.peer.mod = 0x9999;
    shot("13_mod_mismatch", &v);
    return 0;
}
