/* Draws a card for every skin (docs/img/skins/<name>.png): name, kind, and how many levels it can appear in
 * with and without `install.ps1 -Skins`. Original artwork in the overlay's style; nothing from the game files.
 * Usage: skincards <out dir> */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "../../coopnet/gfx.h"
#include "../../coopnet/skins.h"

GpStatus GP GdipSaveImageToFile(GpBitmap *, const WCHAR *, const CLSID *, const void *);
static const CLSID PNG = {0x557CF406, 0x1A04, 0x11D3, {0x9A, 0x73, 0x00, 0x00, 0xF8, 0x1E, 0xF3, 0x2E}};

#define CW 640
#define CH 300
static const char *LEVELS[] = {"BSP_LV426_PT01", "BSP_LV426_PT02", "BSP_TORRENS", "DLC_CHALLENGEMAP16", "ENG_ALIEN_NEST",
    "ENG_REACTORCORE", "ENG_TOWPLATFORM", "HAB_AIRPORT", "HAB_CORPORATEPENT", "HAB_SHOPPINGCENTRE", "SCI_ANDROIDLAB",
    "SCI_HOSPITALLOWER", "SCI_HOSPITALUPPER", "SCI_HUB", "SOLACE", "TECH_COMMS", "TECH_HUB", "TECH_MUTHRCORE", "TECH_RND",
    "TECH_RND_HZDLAB"};
#define NLEVELS 20
/* what skinprep copies by default, and the one level it can't change */
static int is_main(const char *n) { return !strcmp(n, "TAYLOR") || !strcmp(n, "SAMUELS") || !strcmp(n, "AXEL") || !strcmp(n, "RICARDO") || !strcmp(n, "MARLOW"); }

static int builtin(const Skin *s, const char *lv) { char k[64]; snprintf(k, sizeof k, " %s ", lv); return strstr(s->levels, k) != NULL; }

static void figure(Ctx *c, float cx, float top, float h, int kind, ARGB col) {
    /* kind 0 human, 1 spacesuit, 2 android. A plain wireframe bust, not a likeness. */
    float u = h / 100.f;
    PtF body[6] = {{cx - 34 * u, top + 100 * u}, {cx - 30 * u, top + 62 * u}, {cx - 12 * u, top + 52 * u},
                   {cx + 12 * u, top + 52 * u}, {cx + 30 * u, top + 62 * u}, {cx + 34 * u, top + 100 * u}};
    poly(c, with_a(col, 34), body, 6);
    for (int i = 0; i < 6; i++) { int j = (i + 1) % 6; if (i == 5) break; line(c, with_a(col, 200), 1.6f, body[i].X, body[i].Y, body[j].X, body[j].Y); }
    fill(c, with_a(col, 34), cx - 6 * u, top + 42 * u, 12 * u, 12 * u);
    line(c, with_a(col, 200), 1.4f, cx - 6 * u, top + 42 * u, cx - 6 * u, top + 52 * u); line(c, with_a(col, 200), 1.4f, cx + 6 * u, top + 42 * u, cx + 6 * u, top + 52 * u);
    if (kind == 1) {
        disc(c, with_a(col, 30), cx, top + 26 * u, 26 * u); ring(c, with_a(col, 220), 1.8f, cx, top + 26 * u, 26 * u);
        GdipSetPenColor(c->pen, with_a(col, 160)); GdipSetPenWidth(c->pen, 1.4f);
        GdipDrawArc(c->g, c->pen, cx - 16 * u, top + 12 * u, 32 * u, 26 * u, 200, 140);
        fill(c, with_a(col, 120), cx - 30 * u, top + 70 * u, 60 * u, 3 * u);
    } else {
        disc(c, with_a(col, 34), cx, top + 24 * u, 17 * u); ring(c, with_a(col, 220), 1.6f, cx, top + 24 * u, 17 * u);
        if (kind == 2) { disc(c, AMBER, cx - 6 * u, top + 22 * u, 2.6f * u); disc(c, AMBER, cx + 6 * u, top + 22 * u, 2.6f * u);
                         disc(c, with_a(AMBER, 60), cx - 6 * u, top + 22 * u, 6 * u); disc(c, with_a(AMBER, 60), cx + 6 * u, top + 22 * u, 6 * u); }
    }
    for (float y = top; y < top + 100 * u; y += 4) fill(c, C(50, 0, 0, 0), cx - 40 * u, y, 80 * u, 1);
}

static void bar(Ctx *c, float x, float y, float w, const char *label, int n, ARGB col) {
    char t[48]; snprintf(t, sizeof t, "%d/%d LEVELS", n, NLEVELS);
    text(c, label, x, y, 11, GREEN_LO, 1, 2, LEFT, 0);
    text(c, t, x + w, y, 11, col, 1, 2, RIGHT, 0);
    fill(c, C(160, 0, 0, 0), x, y + 18, w, 7);
    for (int i = 0; i < NLEVELS; i++) fill(c, i < n ? col : with_a(col, 30), x + i * (w / NLEVELS) + 1, y + 18, w / NLEVELS - 2, 7);
}

static int card(const Skin *s, int idx, int total, const char *dir) {
    static uint32_t px[CW * CH];
    GpBitmap *bmp = NULL; Ctx c; memset(&c, 0, sizeof c);
    GdipCreateBitmapFromScan0(CW, CH, CW * 4, PARGB32, (BYTE *)px, &bmp);
    GdipGetImageGraphicsContext(bmp, &c.g); GdipSetSmoothingMode(c.g, 4); GdipSetTextRenderingHint(c.g, 4);
    GdipCreateSolidFill(0, &c.br); GdipCreatePen1(0, 1, UNIT_PX, &c.pen);
    c.W = CW; c.H = CH; c.s = 1;

    int nb = 0, nw = 0;
    for (int i = 0; i < NLEVELS; i++) {
        int b = builtin(s, LEVELS[i]); nb += b;
        nw += b || (is_main(s->name) && strcmp(LEVELS[i], "TECH_COMMS"));
    }
    int kind = !strncmp(s->name, "SPACESUIT", 9) ? 1 : !strcmp(s->name, "ANDROID") ? 2 : 0;
    ARGB acc = s->experimental ? AMBER : GREEN;

    fill(&c, C(255, 4, 9, 7), 0, 0, CW, CH);
    for (float y = 0; y < CH; y += 3) fill(&c, C(40, 0, 0, 0), 0, y, CW, 1);
    panel(&c, 10, 10, CW - 20, CH - 20, acc);
    char hd[48]; snprintf(hd, sizeof hd, "PARTNER SKIN  %02d/%02d", idx, total);
    text(&c, hd, 30, 26, 11, GREEN_LO, 1, 4, LEFT, 0);

    /* portrait box */
    float bx = 30, by = 52, bw = 170, bh = 214;
    fill(&c, C(200, 1, 8, 5), bx, by, bw, bh);
    brackets(&c, with_a(acc, 160), bx, by, bw, bh, 12, 1.5f);
    figure(&c, bx + bw / 2, by + 20, 160, kind, acc);
    text(&c, "NO IMAGE ON FILE", bx + bw / 2, by + bh - 20, 9, with_a(GREEN_LO, 200), 1, 2, CENTER, 0);

    /* name and tags */
    float x = 226;
    char nm[32]; upper(nm, sizeof nm, s->name); for (char *p = nm; *p; p++) if (*p == '_') *p = ' ';
    float nw_ = text_w(&c, nm, 30, 1, 4); float npx = nw_ > 380 ? 30 * 380 / nw_ : 30;
    text(&c, nm, x, 50, npx, INK, 1, 4, LEFT, 1);
    const char *what = kind == 1 ? "SPACESUIT" : kind == 2 ? "ANDROID" : "HUMAN";
    const char *tier = s->experimental ? "EXPERIMENTAL" : is_main(s->name) ? "MAIN SKIN" : "EXTRA SKIN";
    float tx = x;
    { float w2 = text_w(&c, what, 11, 1, 2) + 16; fill(&c, with_a(GREEN, 34), tx, 96, w2, 20); text(&c, what, tx + 8, 100, 11, GREEN, 1, 2, LEFT, 0); tx += w2 + 8; }
    { float w2 = text_w(&c, tier, 11, 1, 2) + 16; fill(&c, with_a(acc, 34), tx, 96, w2, 20); text(&c, tier, tx + 8, 100, 11, acc, 1, 2, LEFT, 0); }

    bar(&c, x, 150, CW - 30 - x - 6, "LEVELS THAT HAVE IT", nb, acc);
    (void)nw;

    /* note */
    const char *note = s->experimental ? "ONLY WHEN PICKED. ITS ANIMATIONS MAY NOT FIT A PLAYER." :
                       is_main(s->name) ? "ELSEWHERE YOU SHOW AS ANOTHER SKIN THE LEVEL HAS." :
                       "ONLY IN ITS OWN LEVELS. ELSEWHERE YOU SHOW AS A MAIN SKIN.";
    text(&c, note, x, 222, 10, with_a(INK, 190), 0, 1, LEFT, 0);
    char pick[64]; snprintf(pick, sizeof pick, "PICK IT IN THE LOBBY, OR SKIN.TXT: %s", s->name);
    text(&c, pick, x, 242, 10, GREEN_LO, 1, 1, LEFT, 0);

    WCHAR p[MAX_PATH], lname[40]; int i = 0;
    for (; s->name[i] && i < 39; i++) lname[i] = (WCHAR)(s->name[i] >= 'A' && s->name[i] <= 'Z' ? s->name[i] + 32 : s->name[i]);
    lname[i] = 0;
    swprintf(p, MAX_PATH, L"%hs\\%ls.png", dir, lname);
    int r = GdipSaveImageToFile(bmp, p, &PNG, NULL);
    GdipDeleteBrush(c.br); GdipDeletePen(c.pen); GdipDeleteGraphics(c.g); GdipDisposeImage(bmp);
    printf("%s %s: built in %d, with -Skins %d\n", r == 0 ? "wrote" : "FAILED", s->name, nb, nw);
    return r == 0;
}

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : ".";
    if (!gfx_init()) { printf("GDI+ failed\n"); return 1; }
    int ok = 1;
    for (int i = 0; i < NSKINS; i++) ok &= card(&SKINS[i], i + 1, NSKINS, dir);
    return !ok;
}
