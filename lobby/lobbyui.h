/* lobbyui - the lobby window's screens. Pure drawing into an ARGB buffer plus the clickable areas, so
 * test/lobby_preview.c can render every screen to images without opening a window. */
#pragma once
#include <stdint.h>
#include "lobbyproto.h"
#include "lobbynet.h"
#include "lobbysteam.h"

enum {
    LH_NONE, LH_CLOSE, LH_MIN, LH_NAME, LH_SKIN_PREV, LH_SKIN_NEXT, LH_BODIES, LH_HOST, LH_IP, LH_JOIN_IP,
    LH_LEAVE, LH_READY, LH_START, LH_KICK, LH_COPY,
    LH_TAB_LAN, LH_TAB_NET, LH_STEAMID, LH_COPY_ID, LH_STEAM_HOST, LH_STEAM_JOIN,
    LH_ROW = 100,     /* + index into the found list */
    LH_FRIEND = 200,  /* + index into the friends list: join their Steam lobby */
    LH_INVITE = 300   /* + index into the friends list: invite them to ours */
};
#define LB_MAXHITS 48
typedef struct { int id; float x, y, w, h; } LbHit;

enum { LS_MAIN, LS_LOBBY };

typedef struct {
    uint32_t now;
    int screen;
    const char *version;
    /* main */
    const char *name, *ip_text; int focus;     /* focus: LH_NAME, LH_IP or 0 */
    const char *skin; int bodies;
    const LbFound *found; int nfound;
    const LbPlayer *me_main;                   /* for compat chips in the list */
    int tab;                                   /* 0 LAN, 1 internet (Steam) */
    uint64_t my_steam;
    int steam_up, steam_busy, via_steam;       /* Steam: running for us, creating/joining a lobby, this lobby is over Steam */
    const LbsFriend *friends; int nfriends;    /* online friends, the ones hosting first */
    const uint64_t *invited; int ninvited;
    /* lobby */
    const Lobby *L;
    char my_ip[24], peer_ip[24];
    int countdown, launched;
    const LbCopy *copy;
    /* both */
    const char *toast; int toast_bad;
    int hover;
} LbView;

int  lbui_init(void);
void lbui_render(const LbView *v, uint32_t *argb, int W, int H, LbHit *hits, int *nhits);
int  lbui_hit(const LbHit *hits, int n, float x, float y);   /* id under the point, LH_NONE if none */
int  lbui_in_titlebar(int W, int H, float x, float y);      /* drag area */
