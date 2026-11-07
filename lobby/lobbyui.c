/* lobbyui - see lobbyui.h. Same look as the in-game overlay (coopnet/gfx.h), on an opaque window. */
#include <windows.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "../coopnet/gfx.h"
#include "lobbyui.h"

#define BASE_W 1000.f
#define BASE_H 640.f
#define BG     C(255, 4, 9, 7)
#define DIM    C(255, 120, 150, 132)

typedef struct { Ctx c; LbHit *hits; int n; int hover; } Ui;

int lbui_init(void) { return gfx_init(); }

static void add_hit(Ui *u, int id, float x, float y, float w, float h) {
    if (u->n < LB_MAXHITS) { LbHit *t = &u->hits[u->n++]; t->id = id; t->x = x; t->y = y; t->w = w; t->h = h; }
}
int lbui_hit(const LbHit *hits, int n, float x, float y) {
    for (int i = n - 1; i >= 0; i--)
        if (x >= hits[i].x && x < hits[i].x + hits[i].w && y >= hits[i].y && y < hits[i].y + hits[i].h) return hits[i].id;
    return LH_NONE;
}
int lbui_in_titlebar(int W, int H, float x, float y) { float s = H / BASE_H; return y < 44 * s && x < W - 100 * s; }

enum { B_NORMAL, B_PRIMARY, B_DANGER };
/* A terminal button: outlined, or filled for the main action. Disabled ones are drawn dim and not clickable. */
static void button(Ui *u, int id, float x, float y, float w, float h, const char *label, int style, int enabled) {
    Ctx *c = &u->c; float s = c->s;
    int hot = enabled && u->hover == id;
    ARGB col = !enabled ? with_a(GREEN_LO, 150) : style == B_DANGER ? RED : style == B_PRIMARY ? GREEN : GREEN;
    if (style == B_PRIMARY && enabled) {
        fill(c, with_a(col, hot ? 255 : 225), x, y, w, h);
        for (float yy = y + 1; yy < y + h; yy += 3 * s) fill(c, C(26, 0, 0, 0), x, yy, w, 1);
        text(c, label, x + w / 2, y + h / 2 - 9 * s, 16 * s, C(255, 4, 16, 10), 1, 4 * s, CENTER, 0);
    } else {
        fill(c, hot ? with_a(col, 40) : C(150, 2, 10, 7), x, y, w, h);
        line(c, with_a(col, hot ? 200 : 110), 1, x, y, x + w, y); line(c, with_a(col, hot ? 200 : 110), 1, x, y + h, x + w, y + h);
        line(c, with_a(col, hot ? 200 : 110), 1, x, y, x, y + h); line(c, with_a(col, hot ? 200 : 110), 1, x + w, y, x + w, y + h);
        brackets(c, col, x, y, w, h, 7 * s, 1.6f * s);
        text(c, label, x + w / 2, y + h / 2 - 8 * s, 14 * s, col, 1, 3 * s, CENTER, enabled);
    }
    if (enabled) add_hit(u, id, x, y, w, h);
}

/* A text field: label above, value in a box, blinking block caret when focused. */
static void field(Ui *u, int id, float x, float y, float w, const char *label, const char *val, const char *placeholder, int focus) {
    Ctx *c = &u->c; float s = c->s, h = 40 * s;
    text(c, label, x, y, 11 * s, GREEN_LO, 1, 3 * s, LEFT, 0);
    y += 18 * s;
    fill(c, C(255, 1, 6, 4), x, y, w, h);
    ARGB fr = focus ? GREEN : u->hover == id ? with_a(GREEN, 150) : with_a(GREEN, 70);
    line(c, fr, 1, x, y, x + w, y); line(c, fr, 1, x, y + h, x + w, y + h); line(c, fr, 1, x, y, x, y + h); line(c, fr, 1, x + w, y, x + w, y + h);
    if (focus) fill(c, GREEN, x, y, 3 * s, h);
    char up[64]; upper(up, sizeof up, val && *val ? val : placeholder);
    float tw = text(c, up, x + 14 * s, y + 11 * s, 16 * s, val && *val ? INK : with_a(DIM, 120), 1, 2 * s, LEFT, 0);
    if (focus && (c->now / 530u) % 2 == 0) fill(c, GREEN, x + 14 * s + (val && *val ? tw + 4 * s : 0), y + 10 * s, 9 * s, 19 * s);
    add_hit(u, id, x, y - 18 * s, w, h + 18 * s);
}

static void section(Ctx *c, float x, float y, const char *title, const char *right) {
    float s = c->s;
    fill(c, GREEN, x, y + 3 * s, 4 * s, 12 * s);
    text(c, title, x + 12 * s, y, 13 * s, GREEN, 1, 4 * s, LEFT, 1);
    if (right) text(c, right, x, y, 11 * s, GREEN_LO, 1, 2 * s, RIGHT, 0);
}

/* small status tag: [ OPEN ] */
static float tag(Ctx *c, float x, float y, const char *t, ARGB col, int right) {
    float s = c->s, w = text_w(c, t, 11 * s, 1, 2 * s) + 16 * s, h = 20 * s;
    if (right) x -= w;
    fill(c, with_a(col, 34), x, y, w, h);
    line(c, with_a(col, 160), 1, x, y, x, y + h); line(c, with_a(col, 160), 1, x + w, y, x + w, y + h);
    text(c, t, x + 8 * s, y + 4 * s, 11 * s, col, 1, 2 * s, LEFT, 0);
    return w;
}

static void scope(Ctx *c, float cx, float cy, float R) {
    float s = c->s;
    disc(c, C(120, 2, 20, 12), cx, cy, R);
    GdipSetPenDashStyle(c->pen, 2);
    for (int i = 1; i < 3; i++) ring(c, with_a(GREEN, 45), 1, cx, cy, R * i / 3.f);
    GdipSetPenDashStyle(c->pen, 0);
    ring(c, with_a(GREEN, 130), 1.4f * s, cx, cy, R);
    float a = (float)(c->now % 2400u) / 2400.f * 360.f - 90.f;
    for (int i = 0; i < 10; i++) { GdipSetSolidFillColor(c->br, with_a(GREEN, 30 - i * 3)); GdipFillPie(c->g, c->br, cx - R, cy - R, 2 * R, 2 * R, a - (i + 1) * 5.f, 5.f); }
    float ar = a * 3.14159265f / 180.f;
    line(c, with_a(GREEN, 190), 1.4f * s, cx, cy, cx + cosf(ar) * R, cy + sinf(ar) * R);
}

static void backdrop(Ctx *c) {
    float s = c->s, W = (float)c->W, H = (float)c->H;
    fill(c, BG, 0, 0, W, H);
    for (float x = 0; x < W; x += 40 * s) fill(c, C(10, 132, 244, 176), x, 0, 1, H);
    for (float y = 0; y < H; y += 40 * s) fill(c, C(10, 132, 244, 176), 0, y, W, 1);
    for (float y = 0; y < H; y += 3 * s) fill(c, C(40, 0, 0, 0), 0, y, W, 1);
    vignette(c, C(255, 0, 0, 0), 150, 120 * s);
    /* window frame */
    line(c, with_a(GREEN, 90), 1, 0, 0, W, 0); line(c, with_a(GREEN, 90), 1, 0, H - 1, W, H - 1);
    line(c, with_a(GREEN, 90), 1, 0, 0, 0, H); line(c, with_a(GREEN, 90), 1, W - 1, 0, W - 1, H);
}

static void titlebar(Ui *u, const LbView *v) {
    Ctx *c = &u->c; float s = c->s, W = (float)c->W;
    fill(c, C(255, 6, 16, 11), 0, 0, W, 44 * s);
    line(c, with_a(GREEN, 80), 1, 0, 44 * s, W, 44 * s);
    disc(c, GREEN, 22 * s, 22 * s, 4 * s);
    float x = 36 * s + text(c, "ISOLATION CO-OP", 36 * s, 13 * s, 16 * s, GREEN, 1, 5 * s, LEFT, 1);
    x += 16 * s;
    text(c, "// LAN LOBBY", x, 15 * s, 13 * s, GREEN_LO, 1, 4 * s, LEFT, 0);
    if (v->version) text(c, v->version, W - 110 * s, 16 * s, 11 * s, GREEN_LO, 1, 2 * s, RIGHT, 0);
    /* minimise and close */
    float bw = 44 * s;
    int hm = u->hover == LH_MIN, hc = u->hover == LH_CLOSE;
    if (hm) fill(c, with_a(GREEN, 40), W - 2 * bw, 0, bw, 44 * s);
    if (hc) fill(c, with_a(RED, 90), W - bw, 0, bw, 44 * s);
    line(c, hm ? GREEN : with_a(GREEN, 160), 1.6f * s, W - 2 * bw + 16 * s, 26 * s, W - bw - 16 * s, 26 * s);
    ARGB xc = hc ? INK : with_a(GREEN, 160);
    line(c, xc, 1.6f * s, W - bw + 16 * s, 16 * s, W - 16 * s, 28 * s); line(c, xc, 1.6f * s, W - 16 * s, 16 * s, W - bw + 16 * s, 28 * s);
    add_hit(u, LH_MIN, W - 2 * bw, 0, bw, 44 * s); add_hit(u, LH_CLOSE, W - bw, 0, bw, 44 * s);
}

static void footer(Ctx *c, const LbView *v, const char *hint) {
    float s = c->s, W = (float)c->W, H = (float)c->H, y = H - 30 * s;
    fill(c, C(255, 6, 16, 11), 0, y, W, 30 * s);
    line(c, with_a(GREEN, 60), 1, 0, y, W, y);
    if (v->toast && *v->toast) {
        ARGB col = v->toast_bad ? RED : AMBER;
        disc(c, col, 22 * s, y + 15 * s, 3.5f * s);
        text(c, v->toast, 34 * s, y + 8 * s, 12 * s, col, 1, 2 * s, LEFT, 1);
    } else if (hint) text(c, hint, 22 * s, y + 8 * s, 12 * s, DIM, 0, 2 * s, LEFT, 0);
}

/* Short warning for a lobby in the list, from comparing builds and saves. NULL if none. */
static const char *compat_word(int f, ARGB *col) {
    if (f & LB_DIFF_MOD) { *col = RED; return "OTHER MOD BUILD"; }
    if (f & LB_DIFF_GAME) { *col = RED; return "OTHER GAME BUILD"; }
    if (f & LB_DIFF_SAVE) { *col = AMBER; return "OTHER SAVE"; }
    return NULL;
}

static void screen_main(Ui *u, const LbView *v) {
    Ctx *c = &u->c; float s = c->s, W = (float)c->W, H = (float)c->H;
    float top = 64 * s, ph = H - top - 30 * s - 20 * s;

    /* left: you */
    float lx = 24 * s, lw = 330 * s;
    panel(c, lx, top, lw, ph, GREEN);
    section(c, lx + 20 * s, top + 18 * s, "YOU", NULL);
    field(u, LH_NAME, lx + 20 * s, top + 52 * s, lw - 40 * s, "NAME", v->name, "TYPE YOUR NAME", v->focus == LH_NAME);

    float y = top + 140 * s;
    text(c, "YOUR PARTNER SEES YOU AS", lx + 20 * s, y, 11 * s, GREEN_LO, 1, 3 * s, LEFT, 0);
    y += 18 * s;
    float bw = 40 * s, sw = lw - 40 * s;
    button(u, LH_SKIN_PREV, lx + 20 * s, y, bw, 40 * s, "<", B_NORMAL, 1);
    button(u, LH_SKIN_NEXT, lx + 20 * s + sw - bw, y, bw, 40 * s, ">", B_NORMAL, 1);
    fill(c, C(255, 1, 6, 4), lx + 20 * s + bw + 6 * s, y, sw - 2 * bw - 12 * s, 40 * s);
    char sk[32]; upper(sk, sizeof sk, v->skin ? v->skin : "TAYLOR");
    for (char *p = sk; *p; p++) if (*p == '_') *p = ' ';
    text(c, sk, lx + 20 * s + sw / 2, y + 11 * s, 16 * s, INK, 1, 3 * s, CENTER, 1);

    y += 58 * s;
    /* bodies toggle */
    float bx = lx + 20 * s;
    fill(c, C(255, 1, 6, 4), bx, y, 20 * s, 20 * s);
    line(c, with_a(GREEN, u->hover == LH_BODIES ? 220 : 120), 1, bx, y, bx + 20 * s, y); line(c, with_a(GREEN, u->hover == LH_BODIES ? 220 : 120), 1, bx, y + 20 * s, bx + 20 * s, y + 20 * s);
    line(c, with_a(GREEN, u->hover == LH_BODIES ? 220 : 120), 1, bx, y, bx, y + 20 * s); line(c, with_a(GREEN, u->hover == LH_BODIES ? 220 : 120), 1, bx + 20 * s, y, bx + 20 * s, y + 20 * s);
    if (v->bodies) fill(c, GREEN, bx + 5 * s, y + 5 * s, 10 * s, 10 * s);
    text(c, "EXPERIMENTAL MODE", bx + 32 * s, y + 1 * s, 13 * s, v->bodies ? INK : DIM, 1, 2 * s, LEFT, 0);
    text(c, "CHARACTER BODIES + ONE SHARED ALIEN", bx + 32 * s, y + 20 * s, 10 * s, GREEN_LO, 0, 1 * s, LEFT, 0);
    add_hit(u, LH_BODIES, bx, y - 4 * s, lw - 40 * s, 40 * s);

    /* how it works */
    {
        static const char *steps[] = {"ONE OF YOU HOSTS, THE OTHER JOINS", "THE PARTNER PRESSES READY", "THE HOST PRESSES START", "BOTH GAMES OPEN. LOAD THE SAME SAVE"};
        float sy = y + 54 * s;
        text(c, "HOW IT WORKS", lx + 20 * s, sy, 11 * s, GREEN_LO, 1, 3 * s, LEFT, 0);
        for (int i = 0; i < 4; i++) {
            char n[4]; snprintf(n, sizeof n, "%d", i + 1);
            float yy = sy + 22 * s + i * 21 * s;
            text(c, n, lx + 20 * s, yy, 12 * s, GREEN, 1, 0, LEFT, 0);
            text(c, steps[i], lx + 38 * s, yy, 11 * s, DIM, 0, 1 * s, LEFT, 0);
        }
    }

    /* host */
    float hy = top + ph - 112 * s;
    line(c, with_a(GREEN, 50), 1, lx + 20 * s, hy - 18 * s, lx + lw - 20 * s, hy - 18 * s);
    button(u, LH_HOST, lx + 20 * s, hy, lw - 40 * s, 56 * s, "HOST A GAME", B_PRIMARY, 1);
    text(c, "PLAYERS ON YOUR NETWORK SEE IT", lx + lw / 2, hy + 68 * s, 11 * s, GREEN_LO, 0, 2 * s, CENTER, 0);

    /* right: games on the network */
    float rx = lx + lw + 22 * s, rw = W - rx - 24 * s;
    panel(c, rx, top, rw, ph, GREEN);
    char cnt[32]; snprintf(cnt, sizeof cnt, v->nfound == 1 ? "1 GAME" : "%d GAMES", v->nfound);
    section(c, rx + 20 * s, top + 18 * s, "GAMES ON YOUR NETWORK", NULL);
    text(c, cnt, rx + rw - 20 * s, top + 20 * s, 11 * s, GREEN_LO, 1, 2 * s, RIGHT, 0);
    if ((c->now / 500u) % 2) disc(c, GREEN, rx + rw - 30 * s - text_w(c, cnt, 11 * s, 1, 2 * s), top + 26 * s, 3 * s);

    float ly = top + 54 * s, rowh = 62 * s, listh = ph - 54 * s - 110 * s;
    if (v->nfound == 0) {
        float cy = ly + listh / 2 - 20 * s;
        scope(c, rx + rw / 2, cy, 54 * s);
        text(c, "SCANNING THE NETWORK", rx + rw / 2, cy + 72 * s, 15 * s, GREEN, 1, 4 * s, CENTER, 1);
        text(c, "ASK YOUR FRIEND TO PRESS HOST A GAME", rx + rw / 2, cy + 96 * s, 11 * s, DIM, 0, 2 * s, CENTER, 0);
    }
    int maxrows = (int)(listh / rowh);
    for (int i = 0; i < v->nfound && i < maxrows; i++) {
        const LbFound *f = &v->found[i];
        float y0 = ly + i * rowh, x0 = rx + 16 * s, w0 = rw - 32 * s, h0 = rowh - 8 * s;
        int open = f->open, hot = u->hover == LH_ROW + i && open;
        fill(c, hot ? with_a(GREEN, 26) : C(120, 2, 12, 8), x0, y0, w0, h0);
        fill(c, open ? GREEN : with_a(AMBER, 200), x0, y0, 3 * s, h0);
        char nm[32]; upper(nm, sizeof nm, f->host.name[0] ? f->host.name : "PLAYER");
        text(c, nm, x0 + 18 * s, y0 + 9 * s, 17 * s, INK, 1, 3 * s, LEFT, hot);
        char ip[24], sub[64]; lbnet_ip_str(f->addr.ip, ip, sizeof ip);
        snprintf(sub, sizeof sub, "%d/2 PLAYERS  -  %s", f->players, ip);
        text(c, sub, x0 + 18 * s, y0 + 33 * s, 11 * s, DIM, 0, 2 * s, LEFT, 0);
        float tx = x0 + w0 - 14 * s, ty = y0 + (h0 - 20 * s) / 2;
        if (open) {
            float jw = 86 * s;
            fill(c, hot ? GREEN : with_a(GREEN, 40), tx - jw, ty - 4 * s, jw, 28 * s);
            text(c, "JOIN", tx - jw / 2, ty + 2 * s, 13 * s, hot ? C(255, 4, 16, 10) : GREEN, 1, 4 * s, CENTER, 0);
            tx -= jw + 10 * s;
            add_hit(u, LH_ROW + i, x0, y0, w0, h0);
        } else tx -= tag(c, tx, ty, "FULL", AMBER, 1) + 8 * s;
        if (v->me_main) {
            ARGB col; const char *w = compat_word(lb_compat(v->me_main, &f->host), &col);
            if (w) tag(c, tx, ty, w, col, 1);
        }
    }

    /* join by address */
    float jy = top + ph - 96 * s;
    line(c, with_a(GREEN, 50), 1, rx + 20 * s, jy - 14 * s, rx + rw - 20 * s, jy - 14 * s);
    float jbw = 120 * s;
    field(u, LH_IP, rx + 20 * s, jy, rw - 40 * s - jbw - 12 * s, "OR JOIN BY ADDRESS (VPN LAN, OTHER SUBNET)", v->ip_text, "192.168.1.20", v->focus == LH_IP);
    button(u, LH_JOIN_IP, rx + rw - 20 * s - jbw, jy + 18 * s, jbw, 40 * s, "JOIN", B_NORMAL, v->ip_text && *v->ip_text);

    footer(c, v, "BOTH PLAYERS: PUT LOBBY.EXE IN THE GAME FOLDER. STEAM MUST BE RUNNING.");
}

static void player_card(Ui *u, const LbView *v, float x, float y, float w, float h, const char *role, const LbPlayer *p,
                        const char *ip, int present, int waiting_text, int is_me) {
    Ctx *c = &u->c; float s = c->s;
    ARGB acc = !present ? with_a(GREEN, 120) : p->ready ? GREEN : AMBER;
    panel(c, x, y, w, h, acc);
    text(c, role, x + 20 * s, y + 18 * s, 11 * s, GREEN_LO, 1, 4 * s, LEFT, 0);
    if (is_me) tag(c, x + w - 18 * s, y + 14 * s, "YOU", GREEN, 1);
    if (!present) {
        scope(c, x + w / 2, y + h / 2 - 6 * s, 40 * s);
        text(c, waiting_text ? "CONNECTING" : "WAITING FOR A PLAYER", x + w / 2, y + h - 58 * s, 15 * s, GREEN, 1, 4 * s, CENTER, 1);
        if (!waiting_text) text(c, "THEY PICK YOUR GAME FROM THEIR LIST", x + w / 2, y + h - 34 * s, 11 * s, DIM, 0, 2 * s, CENTER, 0);
        return;
    }
    char nm[32]; upper(nm, sizeof nm, p->name[0] ? p->name : "PLAYER");
    text(c, nm, x + 20 * s, y + 46 * s, 30 * s, INK, 1, 4 * s, LEFT, 1);
    char sk[48], skn[28]; upper(skn, sizeof skn, p->skin[0] ? p->skin : "TAYLOR");
    for (char *q = skn; *q; q++) if (*q == '_') *q = ' ';
    snprintf(sk, sizeof sk, "SEEN AS %s", skn);
    text(c, sk, x + 20 * s, y + 92 * s, 13 * s, GREEN, 1, 3 * s, LEFT, 0);
    if (ip && *ip) text(c, ip, x + 20 * s, y + 114 * s, 12 * s, DIM, 0, 2 * s, LEFT, 0);
    /* ready bar */
    float by = y + h - 52 * s;
    fill(c, with_a(acc, 30), x + 20 * s, by, w - 40 * s, 32 * s);
    fill(c, acc, x + 20 * s, by, 4 * s, 32 * s);
    text(c, p->ready ? "READY" : "NOT READY", x + 36 * s, by + 7 * s, 16 * s, acc, 1, 5 * s, LEFT, 1);
    (void)v;
}

static void check_row(Ctx *c, float x, float y, float w, const char *what, const char *state, ARGB col, const char *note) {
    float s = c->s;
    text(c, what, x, y, 12 * s, GREEN_LO, 1, 3 * s, LEFT, 0);
    disc(c, col, x + 170 * s, y + 8 * s, 4 * s);
    text(c, state, x + 184 * s, y, 13 * s, col, 1, 3 * s, LEFT, 1);
    if (note) text(c, note, x + w, y + 1 * s, 11 * s, DIM, 0, 1 * s, RIGHT, 0);
}

static void screen_lobby(Ui *u, const LbView *v) {
    Ctx *c = &u->c; float s = c->s, W = (float)c->W, H = (float)c->H;
    const Lobby *L = v->L;
    int host = L->role == LB_HOSTING, peer = L->have_peer;
    float top = 64 * s;

    char hd[64], hn[32];
    upper(hn, sizeof hn, host ? L->me.name : (peer ? L->peer.name : "HOST"));
    snprintf(hd, sizeof hd, "LOBBY // HOSTED BY %s", hn[0] ? hn : "PLAYER");
    section(c, 24 * s, top, hd, NULL);
    if (host && v->my_ip[0]) {
        char a[64]; snprintf(a, sizeof a, "YOUR ADDRESS %s", v->my_ip);
        text(c, a, W - 24 * s, top + 2 * s, 12 * s, GREEN_LO, 1, 2 * s, RIGHT, 0);
    }

    float cy = top + 32 * s, cw = (W - 48 * s - 20 * s) / 2, ch = 214 * s;
    const LbPlayer *hp = host ? &L->me : &L->peer, *gp = host ? &L->peer : &L->me;
    player_card(u, v, 24 * s, cy, cw, ch, "HOST", hp, host ? v->my_ip : v->peer_ip, host || peer, !host && !peer, host);
    player_card(u, v, 24 * s + cw + 20 * s, cy, cw, ch, "PARTNER", gp, host ? v->peer_ip : NULL, !host || peer, 0, !host);

    /* checks */
    float ky = cy + ch + 18 * s, kh = 128 * s, kw = W - 48 * s;
    panel(c, 24 * s, ky, kw, kh, GREEN);
    section(c, 44 * s, ky + 14 * s, "CHECKS", NULL);
    if (!peer) {
        text(c, "WAITING FOR BOTH PLAYERS", 44 * s + kw / 2 - 20 * s, ky + 62 * s, 13 * s, DIM, 1, 3 * s, CENTER, 0);
    } else {
        int f = lb_compat(&L->me, &L->peer);
        float rx = 44 * s, rw = kw - 40 * s - (host ? 0 : 230 * s);
        check_row(c, rx, ky + 44 * s, rw, "MOD BUILD", f & LB_DIFF_MOD ? "DIFFERENT" : "MATCH", f & LB_DIFF_MOD ? RED : GREEN,
                  f & LB_DIFF_MOD ? "BOTH NEED THE SAME RELEASE" : NULL);
        check_row(c, rx, ky + 68 * s, rw, "GAME BUILD", f & LB_DIFF_GAME ? "DIFFERENT" : "MATCH", f & LB_DIFF_GAME ? RED : GREEN,
                  f & LB_DIFF_GAME ? "UPDATE THE GAME IN STEAM" : NULL);
        const char *sv = f & LB_NO_SAVE ? (L->me.save ? "PARTNER HAS NO CO-OP SAVE" : "NO CO-OP SAVE YET") : f & LB_DIFF_SAVE ? "DIFFERENT SAVES" : "SAME SAVE";
        ARGB scol = f & (LB_NO_SAVE | LB_DIFF_SAVE) ? AMBER : GREEN;
        check_row(c, rx, ky + 92 * s, rw, "SAVE", sv, scol, f & (LB_NO_SAVE | LB_DIFF_SAVE) ? "LOAD THE SAME SAVE, OR THE WORLDS DRIFT" : NULL);
        if (!host) {
            float bx = 24 * s + kw - 20 * s - 210 * s, by = ky + 50 * s;
            const LbCopy *cp = v->copy;
            if (cp && cp->state == LBC_RUNNING) {
                text(c, "COPYING THE HOST'S SAVE", bx, by, 11 * s, GREEN, 1, 2 * s, LEFT, 0);
                float pr = cp->total ? (float)cp->bytes / (float)cp->total : 0.f;
                fill(c, C(150, 0, 0, 0), bx, by + 22 * s, 210 * s, 8 * s); fill(c, GREEN, bx, by + 22 * s, 210 * s * (pr > 1 ? 1 : pr), 8 * s);
            } else {
                int can = L->peer.save && (f & (LB_DIFF_SAVE | LB_NO_SAVE)) && !L->counting;
                button(u, LH_COPY, bx, by, 210 * s, 40 * s, "COPY HOST'S SAVE", B_NORMAL, can);
                text(c, "YOURS GETS BACKED UP FIRST", bx + 105 * s, by + 48 * s, 10 * s, GREEN_LO, 0, 1 * s, CENTER, 0);
            }
        }
    }

    /* actions */
    float ay = H - 30 * s - 20 * s - 52 * s;
    button(u, LH_LEAVE, 24 * s, ay, 150 * s, 52 * s, "LEAVE", B_DANGER, !v->launched);
    if (host && peer) button(u, LH_KICK, 24 * s + 162 * s, ay, 130 * s, 52 * s, "KICK", B_NORMAL, !L->counting && !v->launched);
    float mw = 260 * s, mx = W - 24 * s - mw;
    const char *hint;
    if (host) {
        button(u, LH_START, mx, ay, mw, 52 * s, "START", B_PRIMARY, lb_can_start(L) && !v->launched);
        hint = !peer ? "WAITING FOR YOUR PARTNER TO JOIN." :
               !L->peer.ready ? "WAITING FOR YOUR PARTNER TO PRESS READY." :
               (lb_compat(&L->me, &L->peer) & (LB_DIFF_MOD | LB_DIFF_GAME)) ? "BOTH NEED THE SAME GAME AND MOD BUILD TO START." :
               "PRESS START. BOTH GAMES OPEN AND CONNECT, THEN LOAD THE SAME SAVE.";
    } else {
        button(u, LH_READY, mx, ay, mw, 52 * s, L->me.ready ? "NOT READY" : "READY", L->me.ready ? B_NORMAL : B_PRIMARY, peer && !v->launched);
        hint = !peer ? "CONNECTING TO THE HOST..." : L->me.ready ? "WAITING FOR THE HOST TO START." : "PRESS READY WHEN YOU'RE SET.";
    }

    if (v->countdown || v->launched) {
        fill(c, C(200, 2, 6, 4), 0, 44 * s, W, ay - 12 * s - 44 * s);   /* the action row stays visible */
        float bw = 520 * s, bh = 190 * s, bx = (W - bw) / 2, by = (ay - 12 * s + 44 * s - bh) / 2;
        fill(c, BG, bx, by, bw, bh);
        panel(c, bx, by, bw, bh, GREEN);
        fill(c, GREEN, bx, by, bw, 2 * s);
        if (v->launched) {
            text(c, "STARTING THE GAME", W / 2, by + 52 * s, 26 * s, GREEN, 1, 6 * s, CENTER, 1);
            text(c, "LOAD THE SAME SAVE AS YOUR PARTNER", W / 2, by + 108 * s, 13 * s, INK, 0, 2 * s, CENTER, 0);
            text(c, "THIS WINDOW CLOSES IN A MOMENT", W / 2, by + 132 * s, 11 * s, DIM, 0, 2 * s, CENTER, 0);
        } else {
            text(c, "LAUNCHING IN", W / 2, by + 22 * s, 13 * s, GREEN_LO, 1, 6 * s, CENTER, 0);
            char n[8]; snprintf(n, sizeof n, "%d", v->countdown);
            text(c, n, W / 2, by + 46 * s, 84 * s, GREEN, 1, 0, CENTER, 1);
            if (!host) text(c, "PRESS NOT READY TO STOP", W / 2, by + 156 * s, 11 * s, DIM, 0, 2 * s, CENTER, 0);
        }
    }
    footer(c, v, hint);
}

void lbui_render(const LbView *v, uint32_t *argb, int W, int H, LbHit *hits, int *nhits) {
    *nhits = 0;
    memset(argb, 0, (size_t)W * H * 4);
    if (!g_ok || W <= 0 || H <= 0) return;
    GpBitmap *bmp = NULL; Ui u; memset(&u, 0, sizeof u);
    if (GdipCreateBitmapFromScan0(W, H, W * 4, PARGB32, (BYTE *)argb, &bmp) != 0) return;
    GdipGetImageGraphicsContext(bmp, &u.c.g);
    GdipSetSmoothingMode(u.c.g, 4); GdipSetTextRenderingHint(u.c.g, 4);
    GdipCreateSolidFill(0, &u.c.br); GdipCreatePen1(0, 1, UNIT_PX, &u.c.pen);
    u.c.W = W; u.c.H = H; u.c.now = v->now; u.c.s = (float)H / BASE_H;
    u.hits = hits; u.hover = v->hover;
    backdrop(&u.c);
    titlebar(&u, v);
    if (v->screen == LS_LOBBY && v->L) screen_lobby(&u, v); else screen_main(&u, v);
    *nhits = u.n;
    GdipDeleteBrush(u.c.br); GdipDeletePen(u.c.pen); GdipDeleteGraphics(u.c.g); GdipDisposeImage(bmp);
}
