/* lobbyui - the lobby window's screens. Pure drawing into an ARGB buffer plus the clickable areas, so
 * test/lobby_preview.c can render every screen to images without opening a window. */
#pragma once
#include <stdint.h>
#include "lobbyproto.h"
#include "lobbynet.h"

enum {
    LH_NONE, LH_CLOSE, LH_MIN, LH_NAME, LH_SKIN_PREV, LH_SKIN_NEXT, LH_BODIES, LH_HOST, LH_IP, LH_JOIN_IP,
    LH_LEAVE, LH_READY, LH_START, LH_KICK, LH_COPY,
    LH_ROW = 100   /* + index into the found list */
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
